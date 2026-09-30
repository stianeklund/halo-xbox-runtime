/* Host tests for compat/xapi/keyboard.c: HID report diffing, repeat, the
 * remote console mailbox and the queue contract Halo's drain loop relies on. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t vk, ascii, flags, reserved; } keystroke;
typedef struct { uint32_t flags, queue_size, repeat_delay, repeat_interval; } parameters;
typedef struct {
  char magic[8];
  volatile uint32_t request, done, mode;
  volatile int32_t result;
  char text[256];
} remote_console;

int standalone_XInputDebugInitKeyboardQueue(parameters *);
int standalone_XInputDebugGetKeystroke(keystroke *);
extern remote_console halo_remote_console;

static uint8_t reports[16][8];
static unsigned int report_count, report_next;
static uint32_t now;
static char evaluated[256];
static int evaluations;

int standalone_keyboard_usb_read(uint8_t *report)
{
  if (report_next == report_count) return 0;
  memcpy(report, reports[report_next++ % 16], 8);
  return 1;
}

uint32_t standalone_keyboard_ticks(void) { return now; }

_Bool hs_console_evaluate(const char *command)
{
  strcpy(evaluated, command);
  evaluations++;
  return strcmp(command, "bad") != 0;
}

static void report(uint8_t modifiers, uint8_t a, uint8_t b)
{
  uint8_t *r = reports[report_count++ % 16];
  memset(r, 0, 8);
  r[0] = modifiers;
  r[2] = a;
  r[3] = b;
}

static keystroke next(void)
{
  keystroke k;
  assert(standalone_XInputDebugGetKeystroke(&k) == 0);
  assert(k.ascii < 0x80 && k.reserved == 0);
  /* Halo's key table maps these past NUMBER_OF_KEYS and asserts. */
  assert(k.vk != 0x10 && k.vk != 0x11);
  return k;
}

#define expect(vk, ascii, flags) expect_at(__LINE__, vk, ascii, flags)
static void expect_at(int line, uint8_t vk, uint8_t ascii, uint8_t flags)
{
  keystroke k = next();
  if (k.vk != vk || k.ascii != ascii || k.flags != flags) {
    fprintf(stderr, "line %d: got vk=%02x ascii=%02x flags=%02x, want %02x %02x %02x\n",
            line, k.vk, k.ascii, k.flags, vk, ascii, flags);
    abort();
  }
}

static void empty(void)
{
  keystroke k;
  assert(standalone_XInputDebugGetKeystroke(&k) == 0x26);
}

static void reset(void)
{
  parameters p = {7, 64, 500, 100};
  report(0, 0, 0);
  assert(standalone_XInputDebugInitKeyboardQueue(&p) == 0);
  empty();
}

int main(void)
{
  unsigned int i;
  const char *typed;

  assert(memcmp(halo_remote_console.magic, "HALOCON1", 8) == 0);
  reset();

  /* Press and release 'a'. */
  report(0, 0x04, 0); expect('A', 'a', 0);
  report(0, 0, 0); expect('A', 0, 0x40);
  empty();

  /* Left Shift + '1' types '!' with the shift flag. */
  report(0x02, 0, 0); expect(0xa0, 0, 0x02);
  report(0x02, 0x1e, 0); expect('1', '!', 0x02);
  report(0, 0, 0); expect('1', 0, 0x40); expect(0xa0, 0, 0x40);

  /* Right Ctrl suppresses ASCII; the flag is CTRL. */
  report(0x10, 0x06, 0); expect(0xa3, 0, 0x01); expect('C', 0, 0x01);
  report(0, 0, 0); expect('C', 0, 0x40); expect(0xa3, 0, 0x40);

  /* Caps Lock toggles and is reported in every later keystroke. */
  report(0, 0x39, 0); expect(0x14, 0, 0x08);
  report(0, 0, 0); expect(0x14, 0, 0x48);
  report(0, 0x05, 0); expect('B', 'B', 0x08);
  report(0x20, 0x05, 0); expect(0xa1, 0, 0x0a);
  report(0, 0, 0); expect('B', 0, 0x48); expect(0xa1, 0, 0x48);
  report(0, 0x39, 0); report(0, 0, 0); next(); next();

  /* Every usage and modifier yields keys Halo accepts (checked in next()). */
  for (i = 0; i < 0x100; i++) {
    keystroke k;
    report((uint8_t)(i & 0xff), (uint8_t)i, 0);
    report(0, 0, 0);
    while (standalone_XInputDebugGetKeystroke(&k) == 0)
      assert(k.vk != 0x10 && k.vk != 0x11 && k.ascii < 0x80);
  }
  /* The sweep toggled Caps, Scroll and Num Lock on; toggle them back. */
  report(0, 0x39, 0); report(0, 0x47, 0); report(0, 0x53, 0); report(0, 0, 0);
  {
    keystroke k;
    while (standalone_XInputDebugGetKeystroke(&k) == 0) {}
  }
  reset();

  /* F1 and F12 from a USB keyboard. */
  report(0, 0x3a, 0x45); report(0, 0, 0);
  expect(0x70, 0, 0); expect(0x7b, 0, 0); expect(0x70, 0, 0x40); expect(0x7b, 0, 0x40);

  /* Console key, Enter, keypad with NumLock off then on. */
  report(0, 0x35, 0); report(0, 0, 0); expect(0xc0, '`', 0); expect(0xc0, 0, 0x40);
  report(0, 0x28, 0); report(0, 0, 0); expect(0x0d, '\r', 0); expect(0x0d, 0, 0x40);
  report(0, 0x59, 0); report(0, 0, 0); expect(0x23, 0, 0); expect(0x23, 0, 0x40);
  report(0, 0x53, 0); report(0, 0x59, 0x53); report(0, 0, 0);
  expect(0x90, 0, 0x10); expect(0x61, '1', 0x10); expect(0x61, 0, 0x50); expect(0x90, 0, 0x50);
  report(0, 0x53, 0); report(0, 0, 0); next(); next();
  empty();

  /* Rollover error reports keep the previous state. */
  report(0, 0x04, 0); expect('A', 'a', 0);
  report(0, 0x01, 0x01); empty();
  report(0, 0, 0); expect('A', 0, 0x40);

  /* Repeat after the delay, then every period, until release. */
  now = 1000;
  report(0, 0x05, 0); expect('B', 'b', 0);
  now = 1499; empty();
  now = 1500; expect('B', 'b', 0x80); empty();
  now = 1600; expect('B', 'b', 0x80);
  report(0, 0x05, 0x04); expect('A', 'a', 0);
  now = 2100; expect('A', 'a', 0x80);
  report(0, 0x05, 0); expect('A', 0, 0x40);
  now = 3000; empty();
  report(0, 0, 0); expect('B', 0, 0x40);

  /* Unused usages produce nothing; the queue is bounded. */
  report(0, 0x03, 0); report(0, 0, 0); empty();
  {
    parameters small = {7, 4, 500, 100};
    standalone_XInputDebugInitKeyboardQueue(&small);
    for (i = 0; i < 4; i++) { report(0, 0x04, 0); report(0, 0, 0); }
    for (i = 0; i < 4; i++) next();
    empty();
  }
  {
    parameters down_only = {1, 64, 500, 100};
    standalone_XInputDebugInitKeyboardQueue(&down_only);
    now = 5000;
    report(0, 0x04, 0); expect('A', 'a', 0);
    now = 6000; empty();
    report(0, 0, 0); empty();
  }
  reset();

  /* Remote evaluate runs the command once and records the result. */
  strcpy(halo_remote_console.text, "rasterizer_stats 0");
  halo_remote_console.mode = 0;
  halo_remote_console.request = halo_remote_console.done + 1;
  empty();
  assert(evaluations == 1 && strcmp(evaluated, "rasterizer_stats 0") == 0);
  assert(halo_remote_console.done == halo_remote_console.request);
  assert(halo_remote_console.result == 1);
  empty();
  assert(evaluations == 1);
  strcpy(halo_remote_console.text, "bad");
  halo_remote_console.request++;
  empty();
  assert(halo_remote_console.result == 0 && evaluations == 2);

  /* Remote typing: '~' presses the console key, newline presses Enter. */
  typed = "~Ab(1)\n";
  strcpy(halo_remote_console.text, typed);
  halo_remote_console.mode = 1;
  halo_remote_console.request++;
  expect(0xc0, '~', 0x02); expect(0xc0, 0, 0x42);
  expect('A', 'A', 0x02); expect('A', 0, 0x42);
  expect('B', 'b', 0); expect('B', 0, 0x40);
  expect('9', '(', 0x02); expect('9', 0, 0x42);
  expect('1', '1', 0); expect('1', 0, 0x40);
  expect('0', ')', 0x02); expect('0', 0, 0x42);
  expect(0x0d, '\r', 0); expect(0x0d, 0, 0x40);
  assert(halo_remote_console.done == halo_remote_console.request);
  empty();

  /* Named keys arrive as 0x80 + HID usage: F1, F12, Escape, Up. */
  strcpy(halo_remote_console.text, "\xba\xc5\xa9\xd2");
  halo_remote_console.request++;
  expect(0x70, 0, 0); expect(0x70, 0, 0x40);
  expect(0x7b, 0, 0); expect(0x7b, 0, 0x40);
  expect(0x1b, 0x1b, 0); expect(0x1b, 0, 0x40);
  expect(0x26, 0, 0); expect(0x26, 0, 0x40);
  empty();

  /* Long text is typed across several frames without overflowing. */
  memset(halo_remote_console.text, 'x', 100);
  halo_remote_console.text[100] = 0;
  halo_remote_console.request++;
  for (i = 0; i < 100; i++) { expect('X', 'x', 0); expect('X', 0, 0x40); }
  empty();
  assert(halo_remote_console.done == halo_remote_console.request);
  return 0;
}
