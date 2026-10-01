/* Keyboard reports for the XAPI debug keyboard compat (compat/xapi/keyboard.c).
 *
 * Two devices produce the same 8-byte report (modifiers, reserved, 6 usages):
 * a USB HID keyboard in boot protocol, read here, and an XID debug keyboard
 * (XID type 2, e.g. xemu's), read by compat/xapi/input.c. nxdk's prebuilt
 * nxdk_usb.lib omits the HID class driver, so crt/CMakeLists.txt compiles it
 * next to this file.
 *
 * Interrupt-in completions run from the OHCI interrupt and only append
 * reports to a single-producer ring; the game thread diffs them. They never
 * resubmit (see crt/nxdk_usb.c): every function here except the completion
 * runs with the USB lock held. */
#include <windows.h>
#include <string.h>
#include "usbh_lib.h"
#include "usbh_hid.h"

#define REPORT_BYTES 8
#define REPORT_RING 32

static uint8_t reports[REPORT_RING][REPORT_BYTES];
static uint8_t last_report[REPORT_BYTES];
static volatile unsigned int report_head, report_tail;
static HID_DEV_T *keyboard;
static uint32_t keyboard_uid;
/* The keyboard's interrupt-in read, listed in keyboard->utr_list so
 * hid_disconnect quits and frees it. */
static UTR_T *keyboard_utr;
static volatile uint8_t keyboard_rearm;

void standalone_usb_signal(void);

/* hid_core.c / hid_driver.c log through sysprintf; keep them quiet. */
void standalone_hid_log(const char *format, ...)
{
    (void)format;
}

uint32_t standalone_keyboard_ticks(void)
{
    return GetTickCount();
}

/* Called from USB completions only. Devices that answer every poll with the
 * current state (xemu's XID keyboard) would fill the ring with duplicates,
 * so only changes are queued. */
void standalone_keyboard_push(const uint8_t *report)
{
    unsigned int head = report_head;
    if (memcmp(report, last_report, REPORT_BYTES) == 0)
        return;
    if (head - report_tail >= REPORT_RING)
        return;
    memcpy(reports[head % REPORT_RING], report, REPORT_BYTES);
    memcpy(last_report, report, REPORT_BYTES);
    __asm__ volatile("" ::: "memory");
    report_head = head + 1;
}

/* nxdk's usbh_hid_start_int_read resubmits from this completion, in the
 * OHCI interrupt, so the read is ours and the USB worker re-arms it. */
static void report_received(UTR_T *utr)
{
    if (utr != keyboard_utr || utr->status != 0)
        return;
    if (utr->xfer_len >= REPORT_BYTES)
        standalone_keyboard_push(utr->buff);
    keyboard_rearm = 1;
    standalone_usb_signal();
}

/* hid_disconnect frees keyboard_utr; a hub poll may have run it since the
 * last refresh. */
static int keyboard_attached(void)
{
    HID_DEV_T *hdev;

    for (hdev = usbh_hid_get_device_list(); hdev != NULL; hdev = hdev->next) {
        if (hdev == keyboard && hdev->uid == keyboard_uid)
            return 1;
    }
    return 0;
}

/* Submits the read, or resubmits it once the last one has finished. */
static void keyboard_arm(void)
{
    IFACE_T *iface = (IFACE_T *)keyboard->iface;
    EP_INFO_T *ep;
    UTR_T *utr;
    int slot;

    keyboard_rearm = 0;
    if (keyboard_utr != NULL) {
        if (!keyboard_utr->bIsTransferDone)
            return;
        keyboard_utr->bIsTransferDone = 0;
        keyboard_utr->xfer_len = 0;
        if (usbh_int_xfer(keyboard_utr) != USBH_OK)
            keyboard_utr->bIsTransferDone = 1;
        return;
    }
    if (iface == NULL || iface->udev == NULL)
        return;
    ep = usbh_iface_find_ep(iface, 0, EP_ADDR_DIR_IN | EP_ATTR_TT_INT);
    if (ep == NULL)
        return;
    for (slot = 0; slot < CONFIG_HID_DEV_MAX_PIPE && keyboard->utr_list[slot] != NULL; slot++)
        ;
    if (slot == CONFIG_HID_DEV_MAX_PIPE)
        return;
    utr = alloc_utr(iface->udev);
    if (utr == NULL)
        return;
    utr->buff = usbh_alloc_mem(ep->wMaxPacketSize);
    if (utr->buff == NULL) {
        free_utr(utr);
        return;
    }
    utr->context = keyboard;
    utr->ep = ep;
    utr->data_len = ep->wMaxPacketSize;
    utr->func = report_received;
    keyboard_utr = utr;
    if (usbh_int_xfer(utr) != USBH_OK) {
        keyboard_utr = NULL;
        usbh_free_mem(utr->buff, ep->wMaxPacketSize);
        free_utr(utr);
        return;
    }
    keyboard->utr_list[slot] = utr;
}

/* Registers the class driver. Call after usbh_core_init and before the first
 * hub poll so a keyboard present at boot is probed. */
void standalone_keyboard_usb_init(void)
{
    usbh_hid_init();
}

/* Returns 1 while a boot keyboard is attached. Starts reading a newly attached
 * one, and retries a read that failed; the caller has already polled the hubs
 * this frame. */
int standalone_keyboard_usb_refresh(void)
{
    HID_DEV_T *hdev;

    for (hdev = usbh_hid_get_device_list(); hdev != NULL; hdev = hdev->next) {
        if (hdev->bSubClassCode == 1 && hdev->bProtocolCode == 1)
            break;
    }
    if (hdev == NULL) {
        keyboard = NULL;
        keyboard_utr = NULL;
        return 0;
    }
    if (hdev != keyboard || hdev->uid != keyboard_uid) {
        keyboard = hdev;
        keyboard_uid = hdev->uid;
        keyboard_utr = NULL;
        report_tail = report_head;
        /* Boot protocol fixes the report at modifiers, reserved, 6 usages;
         * idle 0 reports only on change. */
        usbh_hid_set_protocol(hdev, 0);
        usbh_hid_set_idle(hdev, 0, 0);
    }
    keyboard_arm();
    return 1;
}

/* Called by the USB worker after a completed read. */
void standalone_keyboard_usb_service(void)
{
    if (keyboard_rearm && keyboard != NULL && keyboard_attached())
        keyboard_arm();
}

/* Pops the oldest report into `report`; returns 0 when none is pending. */
int standalone_keyboard_usb_read(uint8_t *report)
{
    unsigned int tail = report_tail;
    if (tail == report_head)
        return 0;
    __asm__ volatile("" ::: "memory");
    memcpy(report, reports[tail % REPORT_RING], REPORT_BYTES);
    report_tail = tail + 1;
    return 1;
}
