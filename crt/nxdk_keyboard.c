/* Keyboard reports for the XAPI debug keyboard compat (compat/xapi/keyboard.c).
 *
 * Two devices produce the same 8-byte report (modifiers, reserved, 6 usages):
 * a USB HID keyboard in boot protocol, read here, and an XID debug keyboard
 * (XID type 2, e.g. xemu's), read by compat/xapi/input.c. nxdk's prebuilt
 * nxdk_usb.lib omits the HID class driver, so crt/CMakeLists.txt compiles it
 * next to this file.
 *
 * Interrupt-in completions run from the OHCI interrupt and only append
 * reports to a single-producer ring; the game thread diffs them. */
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

static void report_received(HID_DEV_T *hdev, uint16_t ep_addr, int status,
                            uint8_t *data, uint32_t length)
{
    (void)ep_addr;
    if (hdev == keyboard && status == 0 && length >= REPORT_BYTES)
        standalone_keyboard_push(data);
}

/* Registers the class driver. Call after usbh_core_init and before the first
 * hub poll so a keyboard present at boot is probed. */
void standalone_keyboard_usb_init(void)
{
    usbh_hid_init();
}

/* Returns 1 while a boot keyboard is attached. Starts reading a newly attached
 * one; the caller has already polled the hubs this frame. */
int standalone_keyboard_usb_refresh(void)
{
    HID_DEV_T *hdev;

    for (hdev = usbh_hid_get_device_list(); hdev != NULL; hdev = hdev->next) {
        if (hdev->bSubClassCode == 1 && hdev->bProtocolCode == 1)
            break;
    }
    if (hdev == NULL) {
        keyboard = NULL;
        return 0;
    }
    if (hdev != keyboard || hdev->uid != keyboard_uid) {
        keyboard = hdev;
        keyboard_uid = hdev->uid;
        report_tail = report_head;
        /* Boot protocol fixes the report at modifiers, reserved, 6 usages;
         * idle 0 reports only on change. */
        usbh_hid_set_protocol(hdev, 0);
        usbh_hid_set_idle(hdev, 0, 0);
    }
    /* Returns HID_RET_XFER_IS_RUNNING while the read is armed. */
    usbh_hid_start_int_read(hdev, 0, report_received);
    return 1;
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
