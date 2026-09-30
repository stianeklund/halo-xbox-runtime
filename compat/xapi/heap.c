/*
 * Standalone compat: XAPILIB Local* heap wrappers (xvutil.obj).
 *
 * The originals forward to RtlAllocateHeap / RtlFreeHeap / RtlReAllocateHeap /
 * RtlSizeHeap on the process heap (0x632a28). Here they sit on pdclib's heap.
 * pdclib cannot report an allocation's size, so each block carries its
 * requested size in a header in front of the returned pointer.
 */
#include "xbox.h"

#define LMEM_MOVEABLE 0x02
#define LMEM_ZEROINIT 0x40
#define HEAP_HEADER_SIZE 8 /* keeps the returned pointer 8-byte aligned */

void *__cdecl compat_malloc(size_t size) __asm__("_malloc");
void *__cdecl compat_realloc(void *memory, size_t size) __asm__("_realloc");
void __cdecl compat_free(void *memory) __asm__("_free");
void *__cdecl compat_memset(void *dst, int value, size_t size) __asm__("_memset");

/* decl.h carries `void f(void)` placeholders for these kb entries (the game
 * calls them by address); bind the real __stdcall signatures to the
 * decorated kb names so the legacy trampolines reach them. */
void *__stdcall standalone_LocalAlloc(unsigned int flags, unsigned int size)
    __asm__("_FUN_001d0c48@8");
void *__stdcall standalone_LocalFree(void *memory) __asm__("_LocalFree@4");
void *__stdcall standalone_LocalReAlloc(void *memory, unsigned int size, unsigned int flags)
    __asm__("_FUN_001d0c65@12");
unsigned int __stdcall standalone_LocalSize(void *memory) __asm__("_FUN_001d0c02@4");

static unsigned int *block_header(void *memory)
{
  return (unsigned int *)((unsigned char *)memory - HEAP_HEADER_SIZE);
}

/* 0x1d0c48 */
void *__stdcall standalone_LocalAlloc(unsigned int flags, unsigned int size)
{
  unsigned int *block;

  block = (unsigned int *)compat_malloc(size + HEAP_HEADER_SIZE);
  if (!block) {
    return 0;
  }
  block[0] = size;
  if (flags & LMEM_ZEROINIT) {
    compat_memset(block + 2, 0, size);
  }
  return block + 2;
}

/* 0x1d0c16: NULL on success, the pointer back on failure. */
void *__stdcall standalone_LocalFree(void *memory)
{
  if (memory) {
    compat_free(block_header(memory));
  }
  return 0;
}

/* 0x1d0c65. Without LMEM_MOVEABLE the original reallocates in place only;
 * report failure then unless the block already fits. */
void *__stdcall standalone_LocalReAlloc(void *memory, unsigned int size, unsigned int flags)
{
  unsigned int *block;
  unsigned int old_size;

  old_size = block_header(memory)[0];
  if (!(flags & LMEM_MOVEABLE)) {
    if (size > old_size) {
      return 0;
    }
    block_header(memory)[0] = size;
    return memory;
  }
  block = (unsigned int *)compat_realloc(block_header(memory), size + HEAP_HEADER_SIZE);
  if (!block) {
    return 0;
  }
  block[0] = size;
  if ((flags & LMEM_ZEROINIT) && size > old_size) {
    compat_memset((unsigned char *)(block + 2) + old_size, 0, size - old_size);
  }
  return block + 2;
}

/* 0x1d0c02 */
unsigned int __stdcall standalone_LocalSize(void *memory)
{
  return block_header(memory)[0];
}
