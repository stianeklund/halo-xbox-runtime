/*
 * Standalone compat: RAD callback threads (RADCB, cachebeta.xbe
 * 0x232130-0x2326e0).
 *
 * A handler thread wakes at its rate and dispatches the registered callbacks
 * in priority order; each callback has a mutex that the game thread takes to
 * suspend it. Bink runs one handler for IO read-ahead and one for sound.
 */
#include "internal.h"

#define RADCB_HANDLER_NEW 0
#define RADCB_HANDLER_RUNNING 1
#define RADCB_HANDLER_SUSPENDED 2
#define RADCB_HANDLER_STOPPING 3
#define RADCB_HANDLER_SUSPEND_REQUESTED 4
#define RADCB_MAXIMUM_WAIT_CALLBACKS 16
/* RAD used 0x3000; nxdk's thread startup and ReadFile want more headroom. */
#define RADCB_THREAD_STACK_BYTES 0x4000
#define RADCB_FREE_EMPTY_HANDLERS 1
#define RADCB_BUSY_WAIT_MS 2

#define WAIT_OBJECT_0 0
#define WAIT_INFINITE 0xffffffffu
#define CREATE_SUSPENDED 4

struct radcb_handler {
  volatile long state;
  unsigned long timeout;
  void *stop_event;
  void *thread;
  unsigned long thread_id;
  void *mutex;
  radcb_callback *callbacks;
  unsigned long iteration;
  unsigned long registered_count;
};

static void wait_for_two_handles(void *first, void *second)
{
  void *handles[2];

  handles[0] = first;
  handles[1] = second;
  switch (rad_WaitForMultipleObjects(2, handles, 0, WAIT_INFINITE)) {
  case WAIT_OBJECT_0:
    rad_WaitForSingleObject(second, WAIT_INFINITE);
    break;
  case WAIT_OBJECT_0 + 1:
    rad_WaitForSingleObject(first, WAIT_INFINITE);
    break;
  }
}

static void insert_priority_callback(radcb_callback **head, radcb_callback *callback)
{
  radcb_callback *current;

  if (!*head || callback->priority >= (*head)->priority) {
    callback->priority_next = *head;
    *head = callback;
    return;
  }
  for (current = *head; current->priority_next; current = current->priority_next) {
    if (callback->priority >= current->priority_next->priority) {
      callback->priority_next = current->priority_next;
      current->priority_next = callback;
      return;
    }
  }
  callback->priority_next = 0;
  current->priority_next = callback;
}

/* Dispatches the highest-priority callback whose mutex is free until no
 * callback asks for service. */
static void dispatch_ready_callbacks(struct radcb_handler *handler)
{
  void *handles[RADCB_MAXIMUM_WAIT_CALLBACKS];
  radcb_callback *callbacks[RADCB_MAXIMUM_WAIT_CALLBACKS];

  handler->iteration++;
  for (;;) {
    radcb_callback *callback, *priority_head = 0;
    unsigned long count, ready;

    for (callback = handler->callbacks; callback; callback = callback->next) {
      if (handler->state != RADCB_HANDLER_RUNNING) {
        return;
      }
      callback->priority = callback->get_priority(callback, handler->iteration);
      if (callback->priority) {
        insert_priority_callback(&priority_head, callback);
      }
    }
    for (count = 0, callback = priority_head; callback && count < RADCB_MAXIMUM_WAIT_CALLBACKS;
         callback = callback->priority_next, count++) {
      handles[count] = callback->mutex;
      callbacks[count] = callback;
    }
    if (!count) {
      return;
    }
    /* RAD polled (timeout 0) and retried at once while the game thread held
     * every wanted callback; block briefly instead, so the thread sleeps
     * until one is released. The bound keeps suspend requests prompt. */
    ready = rad_WaitForMultipleObjects(count, handles, 0, RADCB_BUSY_WAIT_MS);
    if (ready < count) {
      callbacks[ready]->dispatch(callbacks[ready], handler->iteration);
      rad_ReleaseMutex(handles[ready]);
    }
  }
}

static unsigned long __stdcall handler_thread(void *context)
{
  struct radcb_handler *handler = context;

  while (rad_WaitForSingleObject(handler->stop_event, handler->timeout) != WAIT_OBJECT_0) {
    rad_WaitForSingleObject(handler->mutex, WAIT_INFINITE);
    if (handler->state == RADCB_HANDLER_RUNNING && handler->callbacks) {
      dispatch_ready_callbacks(handler);
    } else if (handler->state == RADCB_HANDLER_STOPPING) {
      rad_ReleaseMutex(handler->mutex);
      return 0;
    }
    rad_ReleaseMutex(handler->mutex);
  }
  return 0;
}

static int unlink_callback(struct radcb_handler *handler, radcb_callback *callback)
{
  radcb_callback *current;

  if (!handler || !handler->callbacks) {
    return 0;
  }
  current = handler->callbacks;
  if (current == callback) {
    handler->callbacks = current->next;
    return 1;
  }
  for (; current->next; current = current->next) {
    if (current->next == callback) {
      current->next = callback->next;
      return 1;
    }
  }
  return 0;
}

static void request_stop(struct radcb_handler *handler)
{
  long previous_state = handler->state;

  rad_SetEvent(handler->stop_event);
  handler->state = RADCB_HANDLER_STOPPING;
  if (previous_state == RADCB_HANDLER_NEW) {
    rad_ResumeThread(handler->thread);
  } else if (previous_state == RADCB_HANDLER_SUSPENDED) {
    rad_ReleaseMutex(handler->mutex);
  }
}

static void destroy_handler_resources(struct radcb_handler *handler)
{
  rad_CloseHandle(handler->mutex);
  rad_CloseHandle(handler->thread);
  rad_CloseHandle(handler->stop_event);
  FUN_00231520(handler);
}

static void suspend_handler(struct radcb_handler *handler)
{
  handler->state = RADCB_HANDLER_SUSPEND_REQUESTED;
  rad_WaitForSingleObject(handler->mutex, WAIT_INFINITE);
  handler->state = RADCB_HANDLER_SUSPENDED;
}

static void free_handler(struct radcb_handler *handler)
{
  request_stop(handler);
  rad_WaitForSingleObject(handler->thread, WAIT_INFINITE);
  destroy_handler_resources(handler);
}

/* 0x232390. RADCB_resume_handler */
void __stdcall FUN_00232390(struct radcb_handler *handler)
{
  if (handler->state == RADCB_HANDLER_NEW) {
    handler->state = RADCB_HANDLER_RUNNING;
    rad_ResumeThread(handler->thread);
  } else {
    handler->state = RADCB_HANDLER_RUNNING;
    rad_ReleaseMutex(handler->mutex);
  }
}

/* 0x232430. RADCB_try_to_suspend_callback */
int __stdcall FUN_00232430(struct radcb_handler *handler, radcb_callback *callback)
{
  (void)handler;
  if (callback && callback->mutex) {
    return rad_WaitForSingleObject(callback->mutex, 0) == WAIT_OBJECT_0;
  }
  return 1;
}

/* 0x232460. RADCB_suspend_callback */
void __stdcall FUN_00232460(struct radcb_handler *handler, radcb_callback *callback)
{
  (void)handler;
  if (callback && callback->mutex) {
    rad_WaitForSingleObject(callback->mutex, WAIT_INFINITE);
  }
}

/* 0x232490. RADCB_suspend_2_callbacks */
void __stdcall FUN_00232490(struct radcb_handler *first_handler, radcb_callback *first_callback,
                            struct radcb_handler *second_handler, radcb_callback *second_callback)
{
  (void)first_handler;
  (void)second_handler;
  if (first_callback->mutex && second_callback->mutex) {
    wait_for_two_handles(first_callback->mutex, second_callback->mutex);
  } else {
    /* BinkNextFrame passes the IO callback even when IO is not threaded */
    FUN_00232460(first_handler, first_callback);
    FUN_00232460(second_handler, second_callback);
  }
}

/* 0x2324f0. RADCB_resume_callback */
void __stdcall FUN_002324f0(struct radcb_handler *handler, radcb_callback *callback)
{
  (void)handler;
  if (callback && callback->mutex) {
    rad_ReleaseMutex(callback->mutex);
  }
}

/* 0x232600. RADCB_idle_on_callbacks */
void FUN_00232600(void)
{
  rad_Sleep(0);
}

/* 0x232130. RADCB_allocate_handler: frequency is passes per second. */
struct radcb_handler *__stdcall FUN_00232130(unsigned long rate)
{
  struct radcb_handler *handler = FUN_002314b0(sizeof(*handler));

  if (!handler) {
    return 0;
  }
  handler->state = RADCB_HANDLER_NEW;
  handler->callbacks = 0;
  handler->iteration = 1;
  handler->registered_count = 0;
  handler->timeout = 1000 / (rate ? rate : 1);
  handler->thread = rad_CreateThread(0, RADCB_THREAD_STACK_BYTES, handler_thread, handler,
                                     CREATE_SUSPENDED, &handler->thread_id);
  if (!handler->thread) {
    FUN_00231520(handler);
    return 0;
  }
  handler->mutex = rad_CreateMutexA(0, 0, 0);
  handler->stop_event = rad_CreateEventA(0, 0, 0, 0);
  return handler;
}

/* 0x2321c0. RADCB_register_callback */
void __stdcall FUN_002321c0(struct radcb_handler *handler, radcb_callback *callback,
                            radcb_callback_proc get_priority, radcb_callback_proc dispatch)
{
  if (!handler) {
    return;
  }
  callback->get_priority = get_priority;
  callback->dispatch = dispatch;
  callback->mutex = rad_CreateMutexA(0, 0, 0);
  /* Publish under the handler lock: its thread may be walking the list. */
  suspend_handler(handler);
  callback->next = handler->callbacks;
  handler->callbacks = callback;
  handler->registered_count++;
  FUN_00232390(handler);
}

static int finish_unregistration(struct radcb_handler *handler, radcb_callback *callback,
                                 unsigned long flags)
{
  int freed;

  handler->registered_count--;
  if ((flags & RADCB_FREE_EMPTY_HANDLERS) && !handler->callbacks) {
    free_handler(handler);
    freed = 1;
  } else {
    suspend_handler(handler);
    FUN_00232390(handler);
    freed = 0;
  }
  rad_CloseHandle(callback->mutex);
  callback->mutex = 0;
  return freed;
}

/* 0x2326e0. RADCB_unregister_2_callbacks: bit 0/1 of the result report that
 * the first/second handler was freed. Each list is edited with its handler
 * suspended so the thread never walks a half-unlinked record. */
int __stdcall FUN_002326e0(struct radcb_handler *first_handler, radcb_callback *first_callback,
                           struct radcb_handler *second_handler, radcb_callback *second_callback,
                           unsigned long flags)
{
  int removed = 0, freed = 0;

  if (first_handler) {
    suspend_handler(first_handler);
    if (unlink_callback(first_handler, first_callback)) {
      removed |= 1;
    }
    FUN_00232390(first_handler);
  }
  if (second_handler) {
    suspend_handler(second_handler);
    if (unlink_callback(second_handler, second_callback)) {
      removed |= 2;
    }
    FUN_00232390(second_handler);
  }
  if (removed & 1) {
    freed |= finish_unregistration(first_handler, first_callback, flags);
  }
  if (removed & 2) {
    freed |= finish_unregistration(second_handler, second_callback, flags) << 1;
  }
  return freed;
}
