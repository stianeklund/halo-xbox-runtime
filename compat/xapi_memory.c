/*
 * Standalone compat: XAPILIB physical-memory entry points (physmem.obj).
 *
 * Behavior transcribed from cachebeta.xbe; each routine is a thin wrapper over
 * the kernel export the original imports through its thunk table.
 */
#include "xbox.h"

#define ERROR_NOT_ENOUGH_MEMORY 8

/* 0x1d36d4. addr == -1: anywhere; otherwise exactly [addr, addr+size) with the
 * caller's alignment ignored. */
void *__stdcall XPhysicalAlloc(size_t size, unsigned int addr, unsigned int alignment,
                               unsigned int protect)
{
  ULONG_PTR low;
  ULONG_PTR high;
  void *result;

  if (addr == (unsigned int)-1) {
    low = 0;
    high = (ULONG_PTR)-1;
  } else {
    alignment = 0;
    low = addr;
    high = addr + size - 1;
  }
  result = MmAllocateContiguousMemoryEx(size, low, high, alignment, protect);
  if (!result) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  }
  return result;
}

/* 0x1d371d. A zero size is a no-op. */
void __stdcall physical_memory_protect(void *addr, unsigned int size, unsigned int protect)
{
  if (size != 0) {
    MmSetAddressProtect(addr, size, protect);
  }
}
