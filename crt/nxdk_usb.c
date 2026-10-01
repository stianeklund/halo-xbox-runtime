/* USB plumbing for the XAPI input compat (compat/xapi/input.c).
 *
 * nxdk's OHCI driver hands out transfer descriptors with an unlocked
 * check-then-set (mem_alloc.c alloc_ohci_TD) and masks the controller
 * interrupt only while linking them, but runs completions straight from that
 * interrupt. A completion that resubmits can take the TD a thread is
 * allocating; two endpoints then share it, and a gamepad poll lands in a
 * 6-byte rumble buffer (xemu aborts in usb_packet_copy).
 *
 * So the stack is entered only from threads holding usb_lock. Completions
 * record their result and queue a DPC that wakes the worker, which re-arms
 * reads and finishes output requests under the lock: the work XAPI's own
 * completions do at DISPATCH_LEVEL. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>

static CRITICAL_SECTION usb_lock;
/* Auto-reset: completions that land before the worker runs share one pass. */
static KEVENT usb_event;
static KDPC usb_dpc;
static int initialized;

void standalone_input_service(void);

void standalone_usb_lock(void) { EnterCriticalSection(&usb_lock); }
void standalone_usb_unlock(void) { LeaveCriticalSection(&usb_lock); }

static void NTAPI usb_dpc_routine(PKDPC dpc, PVOID context, PVOID argument1, PVOID argument2)
{
    (void)dpc;
    (void)context;
    (void)argument1;
    (void)argument2;
    KeSetEvent(&usb_event, IO_NO_INCREMENT, FALSE);
}

/* Called from USB completions, in the OHCI interrupt. */
void standalone_usb_signal(void)
{
    KeInsertQueueDpc(&usb_dpc, NULL, NULL);
}

static DWORD WINAPI usb_worker(void *context)
{
    (void)context;
    for (;;) {
        KeWaitForSingleObject(&usb_event, Executive, KernelMode, FALSE, NULL);
        standalone_usb_lock();
        standalone_input_service();
        standalone_usb_unlock();
    }
    return 0;
}

/* Call before usbh_core_init so the first completion has a DPC to queue.
 * Without the worker, input.c still services completions on each device
 * poll, a frame late. */
void standalone_usb_init(void)
{
    HANDLE thread;
    if (initialized) return;
    InitializeCriticalSection(&usb_lock);
    KeInitializeEvent(&usb_event, SynchronizationEvent, FALSE);
    KeInitializeDpc(&usb_dpc, usb_dpc_routine, NULL);
    initialized = 1;
    thread = CreateThread(NULL, 0x4000, usb_worker, NULL, 0, NULL);
    if (!thread) {
        DbgPrint("[input] USB worker thread failed; servicing per poll\n");
        return;
    }
    /* XAPI re-arms from DPCs, ahead of every thread. */
    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(thread);
}

/* XINPUT_FEEDBACK_HEADER: dwStatus, hEvent, 4 reserved bytes, then the
 * KEVENT XAPI references from hEvent at submission (0x24d0ae) and signals
 * when the report completes (0x24cdbf). */
void standalone_usb_feedback_begin(uint32_t *header)
{
    PVOID event = NULL;
    if (header[1] &&
        !NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)header[1], &ExEventObjectType, &event))) {
        header[1] = 0;
        event = NULL;
    }
    header[3] = (uint32_t)event;
}

void standalone_usb_feedback_end(uint32_t *header, uint32_t status)
{
    PKEVENT event = (PKEVENT)header[3];
    header[0] = status;
    if (event) {
        KeSetEvent(event, IO_NO_INCREMENT, FALSE);
        ObfDereferenceObject(event);
    }
}
