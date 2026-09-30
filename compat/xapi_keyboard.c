/*
 * Standalone compat: XAPI debug keyboard queue, XInputDebugInitKeyboardQueue
 * (0x252d73) and XInputDebugGetKeystroke (0x252ad9).
 *
 * Halo drains the queue every frame in input_update_keyboard_devices
 * (0xcfdb0), whether or not a keyboard handle is open. Keystrokes come from:
 *
 *   - a keyboard: a USB HID keyboard in boot protocol, or an XID debug
 *     keyboard (xemu's debug keyboard setting). crt/nxdk_keyboard.c queues
 *     their raw reports, which are diffed here into Windows virtual-key
 *     events;
 *   - halo_remote_console, a RAM mailbox tools/console_send.py writes over
 *     XBDM. It either evaluates a console command or types keystrokes, and
 *     needs no keyboard attached.
 *
 * XINPUT_DEBUG_KEYSTROKE is {VirtualKey, Ascii, Flags, Reserved}. Halo reads
 * flag bits 0-2 as the modifiers, 0x40 as key-up, and asserts Ascii < 0x80.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef HALO_KEYBOARD_HOST_TEST
#define __stdcall
#define KEYBOARD_SYMBOL(s)
#else
#define KEYBOARD_SYMBOL(s) __asm__(s)
#endif

#define ERROR_SUCCESS_CODE 0
#define ERROR_HANDLE_EOF_CODE 0x26

#define KEYSTROKE_CTRL 0x01
#define KEYSTROKE_SHIFT 0x02
#define KEYSTROKE_ALT 0x04
#define KEYSTROKE_CAPSLOCK 0x08
#define KEYSTROKE_NUMLOCK 0x10
#define KEYSTROKE_SCROLLLOCK 0x20
#define KEYSTROKE_KEYUP 0x40
#define KEYSTROKE_REPEAT 0x80

#define QUEUE_KEYDOWN 0x01
#define QUEUE_KEYREPEAT 0x02
#define QUEUE_KEYUP 0x04
#define QUEUE_CAPACITY 64

#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0d
#define VK_LSHIFT 0xa0
#define VK_RSHIFT 0xa1
#define VK_LCONTROL 0xa2
#define VK_RCONTROL 0xa3
#define VK_LMENU 0xa4
#define VK_RMENU 0xa5
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1b
#define VK_SPACE 0x20
#define VK_LWIN 0x5b
#define VK_RWIN 0x5c
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_OEM_3 0xc0

/* Remote console mailbox. The host writes text and mode, then sets
 * request = done + 1; the title sets done = request when finished. */
#define REMOTE_EVALUATE 0
#define REMOTE_TYPE 1
#define REMOTE_TEXT 256
#define REMOTE_CHARS_PER_FRAME 16

typedef struct keystroke {
  uint8_t virtual_key;
  uint8_t ascii;
  uint8_t flags;
  uint8_t reserved;
} keystroke;

typedef struct keyboard_queue_parameters {
  uint32_t flags;
  uint32_t queue_size;
  uint32_t repeat_delay;
  uint32_t repeat_interval;
} keyboard_queue_parameters;

typedef struct remote_console {
  char magic[8];
  volatile uint32_t request;
  volatile uint32_t done;
  volatile uint32_t mode;
  volatile int32_t result;
  char text[REMOTE_TEXT];
} remote_console;

remote_console halo_remote_console = {{'H', 'A', 'L', 'O', 'C', 'O', 'N', '1'}, 0, 0, 0, 0, {0}};

int standalone_keyboard_usb_read(uint8_t *report);
uint32_t standalone_keyboard_ticks(void);
#ifdef HALO_KEYBOARD_HOST_TEST
_Bool hs_console_evaluate(const char *command);
#endif

int __stdcall standalone_XInputDebugInitKeyboardQueue(keyboard_queue_parameters *parameters)
    KEYBOARD_SYMBOL("_FUN_00252d73@4");
int __stdcall standalone_XInputDebugGetKeystroke(keystroke *out)
    KEYBOARD_SYMBOL("_FUN_00252ad9@4");

static keystroke queue[QUEUE_CAPACITY];
static unsigned int queue_head, queue_count, queue_size = QUEUE_CAPACITY;
static uint32_t queue_flags = QUEUE_KEYDOWN | QUEUE_KEYREPEAT | QUEUE_KEYUP;
static uint32_t repeat_delay = 500, repeat_interval = 100;
static uint8_t previous_report[8];
static uint8_t locks;
static uint8_t repeat_usage;
static uint32_t repeat_due;
static unsigned int remote_cursor;

/* HID keyboard usages 0x28-0x38 and their unshifted/shifted ASCII. */
static const uint8_t punctuation_vk[] = {
  VK_RETURN, VK_ESCAPE, VK_BACK, VK_TAB, VK_SPACE, 0xbd, 0xbb, 0xdb, 0xdd,
  0xdc, 0xdc, 0xba, 0xde, VK_OEM_3, 0xbc, 0xbe, 0xbf};
static const char punctuation_plain[] = "\r\x1b\b\t -=[]\\\\;'`,./";
static const char punctuation_shift[] = "\r\x1b\b\t _+{}||:\"~<>?";
static const char digits_shift[] = ")!@#$%^&*(";
/* Usages 0x39-0x65 (CapsLock onwards). */
static const uint8_t function_vk[] = {
  VK_CAPITAL, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
  0x7a, 0x7b, 0x2c, VK_SCROLL, 0x13, 0x2d, 0x24, 0x21, 0x2e, 0x23, 0x22,
  0x27, 0x25, 0x28, 0x26, VK_NUMLOCK, 0x6f, 0x6a, 0x6d, 0x6b, VK_RETURN,
  0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x60, 0x6e, 0xe2,
  0x5d};
/* Keypad 1-9, 0, '.' (usages 0x59-0x63) with NumLock off. */
static const uint8_t keypad_navigation[] = {
  0x23, 0x28, 0x22, 0x25, 0x0c, 0x27, 0x24, 0x26, 0x21, 0x2d, 0x2e};
/* Modifier byte bits: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui. */
/* Sided codes: Halo's virtual-key table (0x281160) maps generic VK_SHIFT and
 * VK_CONTROL to key codes 105/106, past NUMBER_OF_KEYS (104), and asserts on
 * them (input_xbox.c #782). The sided codes map to no key; Halo reads the
 * modifiers from the flags instead. */
static const uint8_t modifier_vk[] = {
  VK_LCONTROL, VK_LSHIFT, VK_LMENU, VK_LWIN, VK_RCONTROL, VK_RSHIFT, VK_RMENU, VK_RWIN};

static uint8_t modifier_flags(uint8_t modifiers)
{
  uint8_t flags = locks;
  if (modifiers & 0x11) flags |= KEYSTROKE_CTRL;
  if (modifiers & 0x22) flags |= KEYSTROKE_SHIFT;
  if (modifiers & 0x44) flags |= KEYSTROKE_ALT;
  return flags;
}

/* Translates a HID usage to a virtual key and, when `ascii` is non-null, the
 * character it types under `flags`. Returns 0 for unmapped usages. */
static uint8_t usage_key(uint8_t usage, uint8_t flags, uint8_t *ascii)
{
  int shift = (flags & KEYSTROKE_SHIFT) != 0;
  uint8_t vk = 0;
  char c = 0;

  if (usage >= 0x04 && usage <= 0x1d) {
    vk = (uint8_t)('A' + usage - 0x04);
    c = (char)(shift != ((flags & KEYSTROKE_CAPSLOCK) != 0) ? vk : vk + 32);
  } else if (usage >= 0x1e && usage <= 0x27) {
    unsigned int digit = usage == 0x27 ? 0 : usage - 0x1d;
    vk = (uint8_t)('0' + digit);
    c = shift ? digits_shift[digit] : (char)vk;
  } else if (usage >= 0x28 && usage <= 0x38) {
    vk = punctuation_vk[usage - 0x28];
    c = (shift ? punctuation_shift : punctuation_plain)[usage - 0x28];
  } else if (usage >= 0x39 && usage <= 0x65) {
    vk = function_vk[usage - 0x39];
    if (usage >= 0x54 && usage <= 0x57) {
      c = "/*-+"[usage - 0x54];
    } else if (usage == 0x58) {
      c = '\r';
    } else if (usage >= 0x59 && usage <= 0x63) {
      if (flags & KEYSTROKE_NUMLOCK) {
        c = usage == 0x63 ? '.' : usage == 0x62 ? '0' : (char)('1' + usage - 0x59);
      } else {
        vk = keypad_navigation[usage - 0x59];
      }
    }
  }
  if (ascii) *ascii = (flags & (KEYSTROKE_CTRL | KEYSTROKE_ALT)) ? 0 : (uint8_t)c;
  return vk;
}

static void queue_push(uint8_t vk, uint8_t ascii, uint8_t flags)
{
  keystroke *k;
  uint32_t kind = flags & KEYSTROKE_KEYUP ? QUEUE_KEYUP :
                  flags & KEYSTROKE_REPEAT ? QUEUE_KEYREPEAT : QUEUE_KEYDOWN;
  if (!vk || !(queue_flags & kind) || queue_count >= queue_size) return;
  k = &queue[(queue_head + queue_count) % QUEUE_CAPACITY];
  k->virtual_key = vk;
  k->ascii = ascii & 0x7f;
  k->flags = flags;
  k->reserved = 0;
  queue_count++;
}

static int report_has(const uint8_t *report, uint8_t usage)
{
  unsigned int i;
  for (i = 2; i < 8; i++)
    if (report[i] == usage) return 1;
  return 0;
}

static void keyboard_report(const uint8_t *report, uint32_t now)
{
  unsigned int i;
  uint8_t flags, ascii, vk, changed;

  /* All-ones usages are the boot protocol's rollover error: keep state. */
  if (report[2] == 1) return;

  /* Releases, then modifier changes, then presses: a key released together
   * with Shift goes up first, a key pressed together with Shift after. */
  flags = modifier_flags(report[0]);
  for (i = 2; i < 8; i++) {
    uint8_t usage = previous_report[i];
    if (usage < 4 || report_has(report, usage)) continue;
    queue_push(usage_key(usage, flags, NULL), 0, flags | KEYSTROKE_KEYUP);
    if (usage == repeat_usage) repeat_usage = 0;
  }
  changed = report[0] ^ previous_report[0];
  for (i = 0; i < 8; i++) {
    if (!(changed & (1u << i))) continue;
    queue_push(modifier_vk[i], 0, report[0] & (1u << i) ? flags : flags | KEYSTROKE_KEYUP);
  }
  for (i = 2; i < 8; i++) {
    uint8_t usage = report[i];
    if (usage < 4 || report_has(previous_report, usage)) continue;
    if (usage == 0x39) locks ^= KEYSTROKE_CAPSLOCK;
    if (usage == 0x47) locks ^= KEYSTROKE_SCROLLLOCK;
    if (usage == 0x53) locks ^= KEYSTROKE_NUMLOCK;
    flags = modifier_flags(report[0]);
    vk = usage_key(usage, flags, &ascii);
    queue_push(vk, ascii, flags);
    if (vk) {
      repeat_usage = usage;
      repeat_due = now + repeat_delay;
    }
  }
  for (i = 0; i < 8; i++) previous_report[i] = report[i];
}

static void keyboard_repeat(uint32_t now)
{
  uint8_t flags, ascii, vk;
  if (!repeat_usage || (int32_t)(now - repeat_due) < 0) return;
  flags = modifier_flags(previous_report[0]);
  vk = usage_key(repeat_usage, flags, &ascii);
  queue_push(vk, ascii, flags | KEYSTROKE_REPEAT);
  repeat_due = now + (repeat_interval ? repeat_interval : 1);
}

/* Types one character from the mailbox as a key press and release, using a
 * US layout. Newline presses Enter; both '`' and '~' press the console key.
 * A byte 0x80 + usage presses that HID usage unshifted (F-keys, arrows...). */
static void type_character(char c)
{
  uint8_t usage, flags, vk, ascii;
  if ((uint8_t)c >= 0x80) {
    usage = (uint8_t)((uint8_t)c - 0x80);
    vk = usage_key(usage, locks, &ascii);
    queue_push(vk, ascii, locks);
    queue_push(vk, 0, locks | KEYSTROKE_KEYUP);
    return;
  }
  if (c == '\n' || c == '\r') {
    usage = 0x28;
  } else if (c >= 'a' && c <= 'z') {
    usage = (uint8_t)(0x04 + c - 'a');
  } else if (c >= 'A' && c <= 'Z') {
    usage = (uint8_t)(0x04 + c - 'A');
  } else if (c >= '1' && c <= '9') {
    usage = (uint8_t)(0x1e + c - '1');
  } else if (c == '0') {
    usage = 0x27;
  } else {
    unsigned int i;
    usage = 0;
    for (i = 0; i < 10 && !usage; i++)
      if (digits_shift[i] == c) usage = (uint8_t)(i ? 0x1d + i : 0x27);
    for (i = 1; i < sizeof(punctuation_plain) - 1 && !usage; i++)
      if (punctuation_plain[i] == c || punctuation_shift[i] == c) usage = (uint8_t)(0x28 + i);
    if (!usage) return;
  }
  /* Press Shift when the unshifted key types something else. */
  flags = (uint8_t)(locks & ~KEYSTROKE_CAPSLOCK);
  usage_key(usage, flags, &ascii);
  if (usage != 0x28 && ascii != (uint8_t)c) flags |= KEYSTROKE_SHIFT;
  vk = usage_key(usage, flags, &ascii);
  queue_push(vk, ascii, flags);
  queue_push(vk, 0, flags | KEYSTROKE_KEYUP);
}

static void remote_poll(void)
{
  remote_console *r = &halo_remote_console;
  unsigned int typed;

  if (r->request == r->done) return;
  r->text[REMOTE_TEXT - 1] = 0;
  if (r->mode == REMOTE_EVALUATE) {
    r->result = hs_console_evaluate(r->text) ? 1 : 0;
  } else {
    /* Leave room in the queue: each character is a press and a release. */
    for (typed = 0; typed < REMOTE_CHARS_PER_FRAME && r->text[remote_cursor] &&
                    queue_count + 2 <= queue_size; typed++)
      type_character(r->text[remote_cursor++]);
    if (r->text[remote_cursor]) return;
    r->result = 1;
  }
  remote_cursor = 0;
  r->done = r->request;
}

int __stdcall standalone_XInputDebugInitKeyboardQueue(keyboard_queue_parameters *parameters)
{
  if (parameters) {
    queue_flags = parameters->flags;
    queue_size = parameters->queue_size && parameters->queue_size < QUEUE_CAPACITY ?
                 parameters->queue_size : QUEUE_CAPACITY;
    repeat_delay = parameters->repeat_delay;
    repeat_interval = parameters->repeat_interval;
  }
  queue_head = queue_count = 0;
  return ERROR_SUCCESS_CODE;
}

int __stdcall standalone_XInputDebugGetKeystroke(keystroke *out)
{
  uint8_t report[8];
  uint32_t now = standalone_keyboard_ticks();

  while (standalone_keyboard_usb_read(report))
    keyboard_report(report, now);
  keyboard_repeat(now);
  if (!queue_count) remote_poll();
  if (!queue_count) return ERROR_HANDLE_EOF_CODE;
  *out = queue[queue_head];
  queue_head = (queue_head + 1) % QUEUE_CAPACITY;
  queue_count--;
  return ERROR_SUCCESS_CODE;
}
