/*
 * Standalone compat: Xbox XAPI input on nxdk's USB/XID host driver.
 *
 * Halo calls the original XDK entry points by absolute address. The generated
 * standalone trampolines redirect those addresses to the decorated symbols
 * below. Handles are stable pointers to one of four port slots; reports stay in
 * Xbox XINPUT_STATE layout so the game-side input code needs no adaptation.
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
int usbh_int_xfer(standalone_usb_transfer *transfer);
int usbh_xid_rumble(standalone_xid_device *device, uint16_t left, uint16_t right);
void standalone_keyboard_usb_init(void);
int standalone_keyboard_usb_refresh(void);
void standalone_keyboard_push(const uint8_t *report);

#define USBH_OK 0
#define ERROR_SUCCESS_CODE 0
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
  uint8_t report[XID_REPORT_BYTES];
} standalone_input_slot;

static standalone_input_slot input_slots[MAXIMUM_GAMEPADS];
static uint32_t input_connected_mask;
static uint32_t input_reported_mask;
/* The debug keyboard is always port 0; compat/xapi/keyboard.c owns its
   keystroke queue, so the handle is only a non-null token for Halo. */
static uint32_t keyboard_reported_mask;
static uint8_t keyboard_handle;
static int input_initialized;

static int input_port_from_device(standalone_xid_device *device)
{
  standalone_xid_device *candidate;
  int port;

  port = 0;
  candidate = usbh_xid_get_device_list();
  while (candidate != NULL && port < MAXIMUM_GAMEPADS) {
    if (candidate->descriptor.type == XID_GAMEPAD_TYPE) {
      if (candidate == device) {
        return port;
      }
      port++;
    }
    candidate = candidate->next;
  }
  return -1;
}

static void input_read_complete(standalone_usb_transfer *transfer)
{
  standalone_xid_device *device;
  standalone_input_slot *slot;
  uint32_t length;
  int port;

  device = (standalone_xid_device *)transfer->context;
  port = input_port_from_device(device);
  if (port < 0) {
    return;
  }
  slot = &input_slots[port];
  if (transfer->status < 0 || transfer->transfer_length < sizeof(standalone_xid_gamepad_report)) {
    return;
  }
  length = transfer->transfer_length;
  if (length > XID_REPORT_BYTES) {
    length = XID_REPORT_BYTES;
  }
  memcpy(slot->report, transfer->buffer, length);
  slot->packet_number++;

  /* Re-arm from the completion like XAPI's interrupt-in completion (0x24cc10)
     and nxdk's SDL joystick driver. A failed transfer is not resubmitted;
     input_refresh_devices queues a fresh read once nxdk marks it done. */
  transfer->transfer_length = 0;
  transfer->done = 0;
  if (usbh_int_xfer(transfer) != USBH_OK) {
    transfer->done = 1;
  }
}

/* XID debug keyboard reports carry no XID header: modifiers, reserved and six
   HID usages, the USB boot keyboard layout (xemu hw/xbox/xid-keyboard.c). */
static void keyboard_read_complete(standalone_usb_transfer *transfer)
{
  if (transfer->status < 0 || transfer->transfer_length < KEYBOARD_REPORT_BYTES) {
    return;
  }
  standalone_keyboard_push(transfer->buffer);
  transfer->transfer_length = 0;
  transfer->done = 0;
  if (usbh_int_xfer(transfer) != USBH_OK) {
    transfer->done = 1;
  }
}

/* Returns 1 while an XID debug keyboard is attached and keeps its read armed.
   The caller has polled the hubs. */
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
}

static void input_initialize_once(void)
{
  int i;

  if (input_initialized) {
    return;
  }
  compat_memset(input_slots, 0, sizeof(input_slots));
  usbh_core_init();
  standalone_keyboard_usb_init();
  usbh_xid_init();
  for (i = 0; i < 500; i++) {
    usbh_pooling_hubs();
    Sleep(1);
  }
  input_initialized = 1;
  input_refresh_devices();
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
    usbh_pooling_hubs();
    changed = (uint32_t)(standalone_keyboard_usb_refresh() | keyboard_refresh_xid()) ^
              keyboard_reported_mask;
    *insertions = changed & ~keyboard_reported_mask;
    *removals = changed & keyboard_reported_mask;
    keyboard_reported_mask ^= changed;
    return changed != 0;
  }
  if ((uint32_t)type != XDEVICE_GAMEPAD_ADDRESS) {
    return 0;
  }
  input_refresh_devices();
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
  input_refresh_devices();
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

int __stdcall standalone_XInputSetState(void *handle, void *feedback)
{
  standalone_input_slot *slot;
  uint8_t *bytes;
  uint32_t *status;
  uint16_t left;
  uint16_t right;

  slot = (standalone_input_slot *)handle;
  if (slot < &input_slots[0] || slot >= &input_slots[MAXIMUM_GAMEPADS] ||
      !slot->connected) {
    return ERROR_DEVICE_NOT_CONNECTED_CODE;
  }
  bytes = (uint8_t *)feedback;
  status = (uint32_t *)feedback;
  left = *(uint16_t *)(bytes + 0x42);
  right = *(uint16_t *)(bytes + 0x44);
  if (usbh_xid_rumble(slot->device, left, right) != USBH_OK) {
    return ERROR_DEVICE_NOT_CONNECTED_CODE;
  }
  *status = 0;
  return ERROR_SUCCESS_CODE;
}

uint32_t __stdcall standalone_XGetDevices(void *type)
{
  input_initialize_once();
  if ((uint32_t)type == XDEVICE_GAMEPAD_ADDRESS) {
    input_refresh_devices();
    return input_connected_mask;
  }
  if ((uint32_t)type == XDEVICE_DEBUG_KEYBOARD_ADDRESS) {
    return keyboard_reported_mask;
  }
  return 0;
}
