/*
 * Standalone compat: Xbox XAPI input on nxdk's USB/XID host driver.
 *
 * Halo calls the original XDK entry points by absolute address. The generated
 * standalone trampolines redirect those addresses to the decorated symbols
 * below. Handles are stable pointers to one of four port slots; reports stay in
 * Xbox XINPUT_STATE layout so the game-side input code needs no adaptation.
 *
 * nxdk's USB stack is not safe to enter from its own completions (see
 * crt/nxdk_usb.c). Completions here only record what happened and signal the
 * USB worker; everything that calls into nxdk runs on a thread holding the
 * USB lock, either a public entry point below or standalone_input_service.
 */
#include "xbox.h"

#define MAXIMUM_GAMEPADS 4
#define XID_MAX_TRANSFER_QUEUE 4

typedef struct standalone_usb_transfer {
  void *device;
  uint8_t setup[8];
  void *endpoint;
  uint8_t *buffer;
  volatile uint8_t done;
  uint8_t pad_15[3];
  uint32_t data_length;
  uint32_t transfer_length;
  uint8_t new_iso_schedule;
  uint8_t pad_21;
  uint16_t iso_start_frame;
  uint16_t iso_lengths[8];
  uint8_t *iso_buffers[8];
  int iso_status[8];
  int transfer_descriptor_count;
  int status;
  int interval;
  void *context;
  void *callback;
  void *next;
} standalone_usb_transfer;

typedef struct standalone_xid_descriptor {
  uint8_t length;
  uint8_t descriptor_type;
  uint16_t version;
  uint8_t type;
  uint8_t subtype;
  uint8_t maximum_input_report_size;
  uint8_t maximum_output_report_size;
  uint16_t alternate_product_ids[4];
} __attribute__((packed)) standalone_xid_descriptor;

typedef struct standalone_xid_device {
  uint16_t vendor_id;
  uint16_t product_id;
  standalone_xid_descriptor descriptor;
  standalone_usb_transfer *transfers[XID_MAX_TRANSFER_QUEUE];
  void *interface;
  uint32_t uid;
  struct standalone_xid_device *next;
  void *user_data;
} standalone_xid_device;

typedef struct standalone_xid_gamepad_report {
  uint8_t start;
  uint8_t length;
  uint16_t buttons;
  uint8_t a;
  uint8_t b;
  uint8_t x;
  uint8_t y;
  uint8_t black;
  uint8_t white;
  uint8_t left_trigger;
  uint8_t right_trigger;
  int16_t left_x;
  int16_t left_y;
  int16_t right_x;
  int16_t right_y;
} __attribute__((packed)) standalone_xid_gamepad_report;

void *__cdecl compat_memset(void *dst, int value, size_t size) __asm__("_memset");
void __stdcall Sleep(uint32_t milliseconds);
void usbh_core_init(void);
int usbh_pooling_hubs(void);
void usbh_xid_init(void);
standalone_xid_device *usbh_xid_get_device_list(void);
int usbh_xid_read(standalone_xid_device *device, uint8_t endpoint, void *callback);
int usbh_xid_write(standalone_xid_device *device, uint8_t endpoint, uint8_t *buffer,
                   uint32_t length, void *callback);
void standalone_keyboard_usb_init(void);
int standalone_keyboard_usb_refresh(void);
void standalone_keyboard_usb_service(void);
void standalone_keyboard_push(const uint8_t *report);
void standalone_usb_init(void);
void standalone_usb_lock(void);
void standalone_usb_unlock(void);
void standalone_usb_signal(void);
void standalone_usb_feedback_begin(uint32_t *header);
void standalone_usb_feedback_end(uint32_t *header, uint32_t status);
void standalone_input_service(void);

#define USBH_OK 0
#define USBH_ERR_MEMORY_OUT (-10)
#define USBH_ERR_EP_NOT_FOUND (-107)
#define USBH_ERR_ABORT (-205)
#define USBH_ERR_DISCONNECTED (-259)
#define ERROR_SUCCESS_CODE 0
#define ERROR_OUTOFMEMORY_CODE 0xe
#define ERROR_GEN_FAILURE_CODE 0x1f
#define ERROR_NOT_SUPPORTED_CODE 0x32
#define ERROR_IO_PENDING_CODE 0x3e5
#define ERROR_IO_DEVICE_CODE 0x45d
#define ERROR_DEVICE_NOT_CONNECTED_CODE 0x48f
#define XID_REPORT_BYTES 32
#define XID_GAMEPAD_TYPE 1
#define XID_DEBUG_KEYBOARD_TYPE 2
#define KEYBOARD_REPORT_BYTES 8
#define XDEVICE_GAMEPAD_ADDRESS 0x24b29c
#define XDEVICE_MEMORY_UNIT_ADDRESS 0x24b218
#define XDEVICE_DEBUG_KEYBOARD_ADDRESS 0x24b2a8

typedef struct standalone_xinput_gamepad {
  uint16_t buttons;
  uint8_t analog_buttons[8];
  int16_t thumb_lx;
  int16_t thumb_ly;
  int16_t thumb_rx;
  int16_t thumb_ry;
} standalone_xinput_gamepad;

typedef struct standalone_xinput_state {
  uint32_t packet_number;
  standalone_xinput_gamepad gamepad;
} standalone_xinput_state;

typedef struct standalone_input_slot {
  standalone_xid_device *device;
  uint32_t uid;
  uint32_t packet_number;
  uint8_t connected;
  /* Set by a completed read; the USB worker re-arms it. */
  volatile uint8_t read_done;
  uint8_t report[XID_REPORT_BYTES];
} standalone_input_slot;

/* XINPUT_FEEDBACK: the header (dwStatus, hEvent, then XAPI's reserved space)
   ahead of the output report, whose report id and length XAPI writes at 0x40
   before sending from there (0x24c427). */
#define FEEDBACK_REPORT_OFFSET 0x40
#define GAMEPAD_OUTPUT_REPORT_BYTES 6

/* XInputSetState requests on one device, in submission order. nxdk completes
   an endpoint's transfers in order but does not say which request finished,
   so completions count off this queue. Keyed by device rather than port so a
   pad that changes port keeps its requests. */
#define FEEDBACK_QUEUE 4
#define FEEDBACK_DEVICES 8

typedef struct standalone_feedback_queue {
  standalone_xid_device *device;
  uint32_t uid;
  uint32_t *feedback[FEEDBACK_QUEUE];
  volatile int result[FEEDBACK_QUEUE];
  uint32_t submitted;
  volatile uint32_t completed;
  uint32_t finished;
} standalone_feedback_queue;

static standalone_input_slot input_slots[MAXIMUM_GAMEPADS];
static standalone_feedback_queue feedback_queues[FEEDBACK_DEVICES];
static uint32_t input_connected_mask;
static uint32_t input_reported_mask;
/* The debug keyboard is always port 0; compat/xapi/keyboard.c owns its
   keystroke queue, so the handle is only a non-null token for Halo. */
static uint32_t keyboard_reported_mask;
static uint8_t keyboard_handle;
static volatile uint8_t keyboard_read_done;
static int input_initialized;

/* Called from completions, in the OHCI interrupt. A pad that changed port
   since its read was armed reports to its new port. */
static standalone_input_slot *input_slot_from_device(standalone_xid_device *device)
{
  int port;

  for (port = 0; port < MAXIMUM_GAMEPADS; port++) {
    if (input_slots[port].device == device) {
      return &input_slots[port];
    }
  }
  return NULL;
}

/* XAPI's interrupt-in completion (0x24cc10) copies the report, bumps the
   packet number and resubmits. The resubmission waits for the USB worker. A
   failed transfer is left for input_refresh_devices, which queues a fresh read
   once nxdk marks it done. */
static void input_read_complete(standalone_usb_transfer *transfer)
{
  standalone_input_slot *slot;
  uint32_t length;

  slot = input_slot_from_device((standalone_xid_device *)transfer->context);
  if (slot == NULL || transfer->status < 0 ||
      transfer->transfer_length < sizeof(standalone_xid_gamepad_report)) {
    return;
  }
  length = transfer->transfer_length;
  if (length > XID_REPORT_BYTES) {
    length = XID_REPORT_BYTES;
  }
  memcpy(slot->report, transfer->buffer, length);
  slot->packet_number++;
  slot->read_done = 1;
  standalone_usb_signal();
}

/* XID debug keyboard reports carry no XID header: modifiers, reserved and six
   HID usages, the USB boot keyboard layout (xemu hw/xbox/xid-keyboard.c). */
static void keyboard_read_complete(standalone_usb_transfer *transfer)
{
  if (transfer->status < 0 || transfer->transfer_length < KEYBOARD_REPORT_BYTES) {
    return;
  }
  standalone_keyboard_push(transfer->buffer);
  keyboard_read_done = 1;
  standalone_usb_signal();
}

static standalone_feedback_queue *feedback_queue_find(standalone_xid_device *device,
                                                      uint32_t uid)
{
  int i;

  for (i = 0; i < FEEDBACK_DEVICES; i++) {
    if (feedback_queues[i].device == device && feedback_queues[i].uid == uid) {
      return &feedback_queues[i];
    }
  }
  return NULL;
}

/* Win32 status for an nxdk transfer result, following XAPI's USBD mapping
   (0x24d81f): a removed device completes as ERROR_GEN_FAILURE, controller
   errors as ERROR_IO_DEVICE. */
static uint32_t feedback_status(int status)
{
  switch (status) {
  case USBH_OK:
    return ERROR_SUCCESS_CODE;
  case USBH_ERR_ABORT:
  case USBH_ERR_DISCONNECTED:
    return ERROR_GEN_FAILURE_CODE;
  case USBH_ERR_MEMORY_OUT:
    return ERROR_OUTOFMEMORY_CODE;
  default:
    return ERROR_IO_DEVICE_CODE;
  }
}

static void feedback_complete(standalone_usb_transfer *transfer)
{
  standalone_xid_device *device;
  standalone_feedback_queue *queue;
  uint32_t completed;

  device = (standalone_xid_device *)transfer->context;
  queue = feedback_queue_find(device, device->uid);
  if (queue == NULL) {
    return;
  }
  completed = queue->completed;
  queue->result[completed % FEEDBACK_QUEUE] = transfer->status;
  __asm__ volatile("" ::: "memory");
  queue->completed = completed + 1;
  standalone_usb_signal();
}

/* Writes back finished requests: dwStatus, then hEvent, as XAPI's output
   completion (0x24cdbf) does at DISPATCH_LEVEL. */
static void feedback_finish(standalone_feedback_queue *queue)
{
  uint32_t index;

  while (queue->finished != queue->completed) {
    index = queue->finished % FEEDBACK_QUEUE;
    standalone_usb_feedback_end(queue->feedback[index], feedback_status(queue->result[index]));
    queue->finished++;
  }
}

/* nxdk aborts a removed device's transfers before it forgets the device, so
   its queue has normally drained; anything left fails here. */
static void feedback_release_removed(void)
{
  standalone_feedback_queue *queue;
  standalone_xid_device *device;
  int i;

  for (i = 0; i < FEEDBACK_DEVICES; i++) {
    queue = &feedback_queues[i];
    if (queue->device == NULL) {
      continue;
    }
    for (device = usbh_xid_get_device_list(); device != NULL; device = device->next) {
      if (device == queue->device && device->uid == queue->uid) {
        break;
      }
    }
    if (device != NULL) {
      continue;
    }
    feedback_finish(queue);
    while (queue->finished != queue->submitted) {
      standalone_usb_feedback_end(queue->feedback[queue->finished % FEEDBACK_QUEUE],
                                  ERROR_GEN_FAILURE_CODE);
      queue->finished++;
    }
    compat_memset(queue, 0, sizeof(*queue));
  }
}

/* Returns 1 while an XID debug keyboard is attached and keeps its read armed.
   The caller holds the USB lock and has polled the hubs. */
static int keyboard_refresh_xid(void)
{
  standalone_xid_device *device;

  for (device = usbh_xid_get_device_list(); device != NULL; device = device->next) {
    if (device->descriptor.type == XID_DEBUG_KEYBOARD_TYPE) {
      usbh_xid_read(device, 0, keyboard_read_complete);
      return 1;
    }
  }
  return 0;
}

/* Runs on the USB worker after completions, and on each device poll. The
   caller holds the USB lock. */
void standalone_input_service(void)
{
  standalone_input_slot *slot;
  int i;

  for (i = 0; i < FEEDBACK_DEVICES; i++) {
    feedback_finish(&feedback_queues[i]);
  }
  for (i = 0; i < MAXIMUM_GAMEPADS; i++) {
    slot = &input_slots[i];
    if (slot->read_done) {
      slot->read_done = 0;
      if (slot->device != NULL) {
        usbh_xid_read(slot->device, 0, input_read_complete);
      }
    }
  }
  if (keyboard_read_done) {
    keyboard_read_done = 0;
    keyboard_refresh_xid();
  }
  standalone_keyboard_usb_service();
}

/* The caller holds the USB lock. */
static void input_refresh_devices(void)
{
  standalone_xid_device *device;
  uint32_t connected;
  int port;

  usbh_pooling_hubs();
  connected = 0;
  port = 0;
  device = usbh_xid_get_device_list();
  while (device != NULL && port < MAXIMUM_GAMEPADS) {
    if (device->descriptor.type == XID_GAMEPAD_TYPE) {
      input_slots[port].device = device;
      input_slots[port].uid = device->uid;
      input_slots[port].connected = 1;
      connected |= 1u << port;
      /* Arms the first read, or a new one after a failed transfer. nxdk
         refuses while the completion-resubmitted read is still in flight. */
      usbh_xid_read(device, 0, input_read_complete);
      port++;
    }
    device = device->next;
  }
  while (port < MAXIMUM_GAMEPADS) {
    input_slots[port].device = NULL;
    input_slots[port].connected = 0;
    port++;
  }
  input_connected_mask = connected;
  feedback_release_removed();
  standalone_input_service();
}

static void input_initialize_once(void)
{
  int i;

  if (input_initialized) {
    return;
  }
  compat_memset(input_slots, 0, sizeof(input_slots));
  standalone_usb_init();
  standalone_usb_lock();
  usbh_core_init();
  standalone_keyboard_usb_init();
  usbh_xid_init();
  standalone_usb_unlock();
  for (i = 0; i < 500; i++) {
    standalone_usb_lock();
    usbh_pooling_hubs();
    standalone_usb_unlock();
    Sleep(1);
  }
  input_initialized = 1;
  standalone_usb_lock();
  input_refresh_devices();
  standalone_usb_unlock();
}

void __stdcall standalone_XInitDevices(uint32_t count, void *types)
    __asm__("_XInitDevices@8");
void __stdcall standalone_FUN_0024c92d(uint32_t count, void *types)
    __asm__("_FUN_0024c92d@8");
int __stdcall standalone_XGetDeviceChanges(void *type, uint32_t *insertions,
                                           uint32_t *removals)
    __asm__("_XGetDeviceChanges@12");
void *__stdcall standalone_XInputOpen(void *type, uint32_t port, uint32_t slot,
                                      void *parameters)
    __asm__("_XInputOpen@16");
void __stdcall standalone_XInputClose(void *handle) __asm__("_XInputClose@4");
int __stdcall standalone_XInputGetState(void *handle, standalone_xinput_state *state)
    __asm__("_XInputGetState@8");
int __stdcall standalone_XInputSetState(void *handle, void *feedback)
    __asm__("_XInputSetState@8");
uint32_t __stdcall standalone_XGetDevices(void *type) __asm__("_XGetDevices@4");

void __stdcall standalone_XInitDevices(uint32_t count, void *types)
{
  (void)count;
  (void)types;
  input_initialize_once();
}

void __stdcall standalone_FUN_0024c92d(uint32_t count, void *types)
{
  standalone_XInitDevices(count, types);
}

int __stdcall standalone_XGetDeviceChanges(void *type, uint32_t *insertions,
                                           uint32_t *removals)
{
  uint32_t changed;

  input_initialize_once();
  *insertions = 0;
  *removals = 0;
  if ((uint32_t)type == XDEVICE_DEBUG_KEYBOARD_ADDRESS) {
    standalone_usb_lock();
    usbh_pooling_hubs();
    changed = (uint32_t)(standalone_keyboard_usb_refresh() | keyboard_refresh_xid()) ^
              keyboard_reported_mask;
    standalone_usb_unlock();
    *insertions = changed & ~keyboard_reported_mask;
    *removals = changed & keyboard_reported_mask;
    keyboard_reported_mask ^= changed;
    return changed != 0;
  }
  if ((uint32_t)type != XDEVICE_GAMEPAD_ADDRESS) {
    return 0;
  }
  standalone_usb_lock();
  input_refresh_devices();
  standalone_usb_unlock();
  changed = input_connected_mask ^ input_reported_mask;
  *insertions = changed & input_connected_mask;
  *removals = changed & input_reported_mask;
  input_reported_mask = input_connected_mask;
  return changed != 0;
}

void *__stdcall standalone_XInputOpen(void *type, uint32_t port, uint32_t slot,
                                      void *parameters)
{
  (void)slot;
  (void)parameters;
  input_initialize_once();
  if ((uint32_t)type == XDEVICE_DEBUG_KEYBOARD_ADDRESS) {
    return port == 0 && keyboard_reported_mask ? &keyboard_handle : NULL;
  }
  standalone_usb_lock();
  input_refresh_devices();
  standalone_usb_unlock();
  if ((uint32_t)type != XDEVICE_GAMEPAD_ADDRESS || port >= MAXIMUM_GAMEPADS ||
      !input_slots[port].connected) {
    return NULL;
  }
  return &input_slots[port];
}

void __stdcall standalone_XInputClose(void *handle)
{
  (void)handle;
}

int __stdcall standalone_XInputGetState(void *handle, standalone_xinput_state *state)
{
  standalone_input_slot *slot;
  standalone_xid_gamepad_report copy;
  const standalone_xid_gamepad_report *report;
  uint32_t packet_number;
  uint32_t flags;

  slot = (standalone_input_slot *)handle;
  if (slot < &input_slots[0] || slot >= &input_slots[MAXIMUM_GAMEPADS] ||
      !slot->connected) {
    return ERROR_DEVICE_NOT_CONNECTED_CODE;
  }
  /* XAPI only copies the latest report at raised IRQL (0x24c3b6); nxdk
     completes transfers from the USB ISR, so mask interrupts for the copy. */
  __asm__ volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
  memcpy(&copy, slot->report, sizeof(copy));
  packet_number = slot->packet_number;
  __asm__ volatile("pushl %0\n\tpopfl" : : "r"(flags) : "memory", "cc");
  report = &copy;
  state->packet_number = packet_number;
  state->gamepad.buttons = report->buttons;
  state->gamepad.analog_buttons[0] = report->a;
  state->gamepad.analog_buttons[1] = report->b;
  state->gamepad.analog_buttons[2] = report->x;
  state->gamepad.analog_buttons[3] = report->y;
  state->gamepad.analog_buttons[4] = report->black;
  state->gamepad.analog_buttons[5] = report->white;
  state->gamepad.analog_buttons[6] = report->left_trigger;
  state->gamepad.analog_buttons[7] = report->right_trigger;
  state->gamepad.thumb_lx = report->left_x;
  state->gamepad.thumb_ly = report->left_y;
  state->gamepad.thumb_rx = report->right_x;
  state->gamepad.thumb_ry = report->right_y;
  return ERROR_SUCCESS_CODE;
}

/* Queues one output report; returns the dwStatus it leaves. The caller holds
   the USB lock. */
static uint32_t feedback_submit(standalone_xid_device *device, uint32_t *header)
{
  standalone_feedback_queue *queue;
  uint8_t *report;
  int result;

  report = (uint8_t *)header + FEEDBACK_REPORT_OFFSET;
  report[0] = 0;
  report[1] = GAMEPAD_OUTPUT_REPORT_BYTES;
  if (report[1] > device->descriptor.maximum_output_report_size) {
    report[1] = device->descriptor.maximum_output_report_size;
  }
  queue = feedback_queue_find(device, device->uid);
  if (queue == NULL) {
    queue = feedback_queue_find(NULL, 0);
    if (queue == NULL) {
      return header[0] = ERROR_OUTOFMEMORY_CODE;
    }
    queue->device = device;
    queue->uid = device->uid;
  }
  feedback_finish(queue);
  if (queue->submitted - queue->finished == FEEDBACK_QUEUE) {
    return header[0] = ERROR_OUTOFMEMORY_CODE;
  }
  standalone_usb_feedback_begin(header);
  header[0] = ERROR_IO_PENDING_CODE;
  queue->feedback[queue->submitted % FEEDBACK_QUEUE] = header;
  result = usbh_xid_write(device, 0, report, report[1], feedback_complete);
  if (result == USBH_OK) {
    queue->submitted++;
  } else if (result == USBH_ERR_DISCONNECTED || result == USBH_ERR_EP_NOT_FOUND) {
    standalone_usb_feedback_end(header, ERROR_DEVICE_NOT_CONNECTED_CODE);
  } else {
    standalone_usb_feedback_end(header, feedback_status(result));
  }
  return header[0];
}

/* Asynchronous like XAPI's (0x24c427): queues the output report and returns
   dwStatus, ERROR_IO_PENDING until the report completes. Halo only sends again
   once dwStatus leaves ERROR_IO_PENDING (input_flush_rumble). */
int __stdcall standalone_XInputSetState(void *handle, void *feedback)
{
  standalone_input_slot *slot;
  uint32_t *header;
  uint32_t status;

  slot = (standalone_input_slot *)handle;
  if (slot < &input_slots[0] || slot >= &input_slots[MAXIMUM_GAMEPADS]) {
    return ERROR_DEVICE_NOT_CONNECTED_CODE;
  }
  header = (uint32_t *)feedback;
  /* The lock also keeps the worker from completing the request before
     dwStatus reads ERROR_IO_PENDING, which XAPI gets from raised IRQL. */
  standalone_usb_lock();
  if (!slot->connected || slot->device == NULL) {
    status = header[0] = ERROR_DEVICE_NOT_CONNECTED_CODE;
  } else if (slot->device->descriptor.maximum_output_report_size == 0) {
    status = header[0] = ERROR_NOT_SUPPORTED_CODE;
  } else {
    status = feedback_submit(slot->device, header);
  }
  standalone_usb_unlock();
  return status;
}

uint32_t __stdcall standalone_XGetDevices(void *type)
{
  input_initialize_once();
  if ((uint32_t)type == XDEVICE_GAMEPAD_ADDRESS) {
    standalone_usb_lock();
    input_refresh_devices();
    standalone_usb_unlock();
    return input_connected_mask;
  }
  if ((uint32_t)type == XDEVICE_DEBUG_KEYBOARD_ADDRESS) {
    return keyboard_reported_mask;
  }
  return 0;
}
