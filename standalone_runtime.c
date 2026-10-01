/*
 * Standalone (nxdk) build runtime support.
 *
 * Not part of the reconstructed game: this is the bring-up scaffolding the
 * generated stubs (tools/gen_legacy_image.py) call into when code
 * reaches something the standalone image cannot provide yet.
 */
#include "xbox.h"

#define HALO_MISSING_SLOTS 1024

static const char *halo_missing_seen[HALO_MISSING_SLOTS];

/* First-hit order of missing symbols, read back over XBDM by
 * tools/missing_dump.py (DbgPrint output is not always forwarded). */
const char *halo_missing_order[HALO_MISSING_SLOTS];
unsigned int halo_missing_count;

/* Log each missing symbol once; the generated stub then returns 0. */
void halo_standalone_missing(const char *name)
{
  unsigned int slot = ((unsigned int)name >> 2) % HALO_MISSING_SLOTS;
  unsigned int probe;

  for (probe = 0; probe < HALO_MISSING_SLOTS; probe++) {
    unsigned int i = (slot + probe) % HALO_MISSING_SLOTS;
    if (halo_missing_seen[i] == name) {
      return;
    }
    if (halo_missing_seen[i] == 0) {
      halo_missing_seen[i] = name;
      if (halo_missing_count < HALO_MISSING_SLOTS) {
        halo_missing_order[halo_missing_count] = name;
      }
      halo_missing_count++;
      break;
    }
  }
  DbgPrint("[halo-standalone] missing: %s\n", name);
}

/* ------------------------------------------------------------------------
 * First-exception snapshot. main()'s __except filter calls this before
 * Halo's own filter (shell_xbox.c, HALO_STANDALONE only): by the time
 * halt_and_catch_fire runs, the unwind has reused the stack that held the
 * EXCEPTION_RECORD / CONTEXT, so capture them here. Read back with
 * tools/crash_report.py.
 */
#define HALO_CRASH_CONTEXT_SIZE 0x238 /* Xbox CONTEXT through SegSs */
#define HALO_CRASH_RECORD_SIZE 0x50
#define HALO_CRASH_STACK_WORDS 256
#define HALO_CRASH_CONTEXT_EIP 0x224
#define HALO_CRASH_CONTEXT_ESP 0x230
#define HALO_CRASH_CONTEXT_EBP 0x220

unsigned int halo_crash_count;
unsigned char halo_crash_record[HALO_CRASH_RECORD_SIZE];
unsigned char halo_crash_context[HALO_CRASH_CONTEXT_SIZE];
unsigned long halo_crash_stack[HALO_CRASH_STACK_WORDS];

/* compat/d3d8/draw.c */
void compat_d3d_close_open_begin(void);

void standalone_report_exception(void *exception_pointers)
{
  void **pointers = (void **)exception_pointers;
  const unsigned long *record = (const unsigned long *)pointers[0];
  const unsigned char *context = (const unsigned char *)pointers[1];
  const unsigned long *stack;
  unsigned int i;

  if (halo_crash_count++ == 0) {
    for (i = 0; i < HALO_CRASH_RECORD_SIZE; i++) {
      halo_crash_record[i] = ((const unsigned char *)record)[i];
    }
    for (i = 0; i < HALO_CRASH_CONTEXT_SIZE; i++) {
      halo_crash_context[i] = context[i];
    }
    stack = *(const unsigned long *const *)(context + HALO_CRASH_CONTEXT_ESP);
    for (i = 0; i < HALO_CRASH_STACK_WORDS; i++) {
      halo_crash_stack[i] = stack[i];
    }
    DbgPrint("[halo-standalone] exception %08lx at %08lx (%08lx %08lx) eip %08lx esp %08lx ebp %08lx\n",
             record[0], record[3], record[5], record[6],
             *(const unsigned long *)(context + HALO_CRASH_CONTEXT_EIP),
             *(const unsigned long *)(context + HALO_CRASH_CONTEXT_ESP),
             *(const unsigned long *)(context + HALO_CRASH_CONTEXT_EBP));
  }
  /* Every exception: halt_and_catch_fire draws next. After the snapshot, so
   * a fault here cannot lose it. */
  compat_d3d_close_open_begin();
}
