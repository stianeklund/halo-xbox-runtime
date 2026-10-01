/*
 * Standalone compat: RAD Bink memory, timer and error helpers.
 *
 * radmalloc/radfree over the title's RADSetMemory callbacks, the
 * pushmalloc/popmalloc block allocator BinkOpen carves its buffers from,
 * RADTimerRead and BinkSetError (cachebeta.xbe 0x22de60-0x22df50,
 * 0x2314b0-0x231520, 0x2328c0).
 */
#include "internal.h"

#define RAD_ALLOCATION_ALIGNMENT 32
#define RAD_ALLOCATION_OVERHEAD 64
#define RAD_ALLOCATOR_STANDARD 0
#define RAD_ALLOCATOR_CALLBACK 3
/* Each pushed size and the running total stay below 1 GiB, so neither the
 * rounding nor the sums can wrap; PENDING_ALLOCATION_OVERFLOW marks a
 * pushmalloc that would have. */
#define PENDING_ALLOCATION_LIMIT 0x40000000ul
#define PENDING_ALLOCATION_OVERFLOW 0xfffffffful

typedef void *(__stdcall *rad_malloc_proc)(unsigned long);
typedef void(__stdcall *rad_free_proc)(void *);

static unsigned long pending_allocation_count;
static unsigned long pending_allocation_sizes[128];
static void **pending_allocation_destinations[128];

/* 0x2328c0. RADTimerRead: milliseconds. The Xbox tick count is 1ms. */
unsigned long FUN_002328c0(void)
{
  return rad_GetTickCount();
}

/* 0x22df50. BinkSetError */
void __stdcall FUN_0022df50(const char *error)
{
  unsigned long i;

  for (i = 0; i + 1 < sizeof(bink_error) && error[i]; i++) {
    bink_error[i] = error[i];
  }
  bink_error[i] = 0;
}

/* 0x2314b0. radmalloc: 32-byte aligned, 64 bytes of overhead holding the
 * release routine and the alignment adjustment in front of the block. */
void *__stdcall FUN_002314b0(unsigned long size)
{
  unsigned char *allocation = 0;
  unsigned char allocator = RAD_ALLOCATOR_STANDARD;
  unsigned char adjustment;
  unsigned char *prefix;

  if (!size || size > 0xfffffffful - RAD_ALLOCATION_OVERHEAD) {
    return 0;
  }
  if (rad_allocate_callback) {
    allocation = ((rad_malloc_proc)rad_allocate_callback)(size + RAD_ALLOCATION_OVERHEAD);
    if (allocation == (unsigned char *)-1) {
      return 0;
    }
    if (allocation) {
      allocator = RAD_ALLOCATOR_CALLBACK;
    }
  }
  if (!allocation) {
    allocation = rad_malloc(size + RAD_ALLOCATION_OVERHEAD);
    if (!allocation) {
      return 0;
    }
  }
  adjustment = (unsigned char)(RAD_ALLOCATION_OVERHEAD -
                               ((unsigned long)allocation & (RAD_ALLOCATION_ALIGNMENT - 1)));
  allocation += adjustment;
  prefix = allocation - 8;
  *(rad_free_proc *)prefix = allocator == RAD_ALLOCATOR_CALLBACK ?
    (rad_free_proc)rad_release_callback : 0;
  prefix[6] = allocator;
  prefix[7] = adjustment;
  return allocation;
}

/* 0x231520. radfree */
void __stdcall FUN_00231520(void *allocation)
{
  unsigned char *prefix;

  if (!allocation) {
    return;
  }
  prefix = (unsigned char *)allocation - 8;
  if (prefix[6] == RAD_ALLOCATOR_CALLBACK) {
    (*(rad_free_proc *)prefix)((unsigned char *)allocation - prefix[7]);
  } else {
    rad_free((unsigned char *)allocation - prefix[7]);
  }
}

/* 0x22de60. pushmalloc: queue a sub-allocation of the next popmalloc block;
 * starts are spread over 32-byte cache-line slots. Not in the original: a
 * total that would overflow (sizes come from the file header) saturates, so
 * the popmalloc fails instead of handing out an undersized block. */
void __stdcall FUN_0022de60(void **destination, unsigned long size)
{
  unsigned long cache_lines = pending_allocation_bytes / 32;

  if (pending_allocation_count >= 128) {
    return;
  }
  if (size > PENDING_ALLOCATION_LIMIT || pending_allocation_bytes > PENDING_ALLOCATION_LIMIT) {
    pending_allocation_bytes = PENDING_ALLOCATION_OVERFLOW;
    return;
  }
  size = (size + 15) & ~15ul;
  cache_lines = (cache_lines - size / 32 + 1) % 32;
  size += cache_lines * 32;
  pending_allocation_bytes += size;
  pending_allocation_sizes[pending_allocation_count] = size;
  pending_allocation_destinations[pending_allocation_count] = destination;
  pending_allocation_count++;
}

/* 0x22dec0. popmalloc: one radmalloc for size plus every pushed block. */
void *__stdcall FUN_0022dec0(unsigned long size)
{
  unsigned char *allocation;
  unsigned long i;

  if (size > PENDING_ALLOCATION_LIMIT || pending_allocation_bytes > PENDING_ALLOCATION_LIMIT) {
    allocation = 0;
  } else {
    size = (size + 15) & ~15ul;
    allocation = FUN_002314b0(pending_allocation_bytes + size);
  }
  pending_allocation_bytes = 0;
  if (allocation) {
    unsigned char *next = allocation + size;
    for (i = 0; i < pending_allocation_count; i++) {
      *pending_allocation_destinations[i] = next;
      next += pending_allocation_sizes[i];
    }
  }
  pending_allocation_count = 0;
  return allocation;
}

/* 0x22df30 */
void *bpopmalloc(unsigned long size, BINK *bink)
{
  bink->totalmem += pending_allocation_bytes + size;
  return FUN_0022dec0(size);
}
