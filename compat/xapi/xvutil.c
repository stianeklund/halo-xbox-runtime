/*
 * Standalone compat: small XAPILIB queries kb.json names after their Halo
 * use.
 *
 *   file_get_full_attributes (0x1d0e96)        = GetFileAttributesA
 *   xbox_query_global_memory_status (0x1d0da1) = GlobalMemoryStatus
 *   plus the FUN_* routines below (Sleep, SetFileAttributesA,
 *   RemoveDirectoryA, XQueryValue, ...) and XapiSetLastNTError
 *
 * A logging stub returning 0 is wrong for both: 0 from GetFileAttributesA
 * means "exists", which sends input_xbox.c down its d:\write.xts recording
 * path.
 */
#include "xbox.h"

unsigned long __cdecl nxdk_GetFileAttributesA(const char *path) __asm__("_GetFileAttributesA");

int __stdcall standalone_file_get_full_attributes(const char *path)
    __asm__("_file_get_full_attributes@4");
void __stdcall standalone_xbox_query_global_memory_status(void *status)
    __asm__("_xbox_query_global_memory_status@4");

#define MEMORYSTATUS_SIZE 0x20
#define USER_ADDRESS_SPACE_SIZE 0x7ffe0000
#define PAGE_SHIFT 12

int __stdcall standalone_file_get_full_attributes(const char *path)
{
  return (int)nxdk_GetFileAttributesA(path);
}

/* Fills a MEMORYSTATUS from MmQueryStatistics, as the original does. */
void __stdcall standalone_xbox_query_global_memory_status(void *status)
{
  MM_STATISTICS stats;
  unsigned long *out;

  stats.Length = sizeof(stats);
  MmQueryStatistics(&stats);
  out = (unsigned long *)status;
  out[1] = 0; /* dwMemoryLoad */
  out[4] = 0; /* dwTotalPageFile */
  out[5] = 0; /* dwAvailPageFile */
  out[2] = stats.TotalPhysicalPages << PAGE_SHIFT;
  out[3] = stats.AvailablePages << PAGE_SHIFT;
  out[6] = USER_ADDRESS_SPACE_SIZE;
  out[0] = MEMORYSTATUS_SIZE;
  out[7] = USER_ADDRESS_SPACE_SIZE - stats.VirtualMemoryBytesReserved;
}

/* ------------------------------------------------------------------------
 * Further XAPI routines kb.json only knows by address (FUN_*), identified
 * from their bodies in cachebeta.xbe.
 */


void __cdecl nxdk_Sleep(unsigned long milliseconds) __asm__("_Sleep");
int __cdecl nxdk_SetFileAttributesA(const char *path, unsigned long attributes)
    __asm__("_SetFileAttributesA");
int __cdecl nxdk_RemoveDirectoryA(const char *path) __asm__("_RemoveDirectoryA");
void __cdecl nxdk_SetLastError_xv(unsigned long error) __asm__("_SetLastError");
void *__stdcall standalone_LocalAlloc_xv(unsigned int flags, unsigned int size)
    __asm__("_FUN_001d0c48@8");

void __stdcall standalone_Sleep(unsigned long milliseconds) __asm__("_FUN_001d0362@4");
void *__stdcall standalone_heap_alloc(unsigned int flags, unsigned int size)
    __asm__("_FUN_001d0bb9@8");
int __stdcall standalone_SetFileAttributesA(const char *path, unsigned int attributes)
    __asm__("_FUN_001d0df0@8");
int __stdcall standalone_RemoveDirectoryA(const char *path) __asm__("_FUN_001d347c@4");
int __stdcall standalone_XQueryValue(int value_index, void *type_out, void *value_buf,
                                     int value_len, void *result_len_out)
    __asm__("_FUN_001d4464@20");
void __stdcall standalone_XapiSetLastNTError(int status) __asm__("_XapiSetLastNTError@4");

/* 0x1d0362: SleepEx(milliseconds, FALSE). */
void __stdcall standalone_Sleep(unsigned long milliseconds)
{
  nxdk_Sleep(milliseconds);
}

/* 0x1d0bb9: RtlAllocateHeap on the process heap with HEAP_ZERO_MEMORY taken
 * from LMEM_ZEROINIT, i.e. LocalAlloc semantics. Shares heap.c's
 * blocks so LocalFree / LocalSize work on the result. */
void *__stdcall standalone_heap_alloc(unsigned int flags, unsigned int size)
{
  return standalone_LocalAlloc_xv(flags, size);
}

/* 0x1d0df0 */
int __stdcall standalone_SetFileAttributesA(const char *path, unsigned int attributes)
{
  return nxdk_SetFileAttributesA(path, attributes);
}

/* 0x1d347c */
int __stdcall standalone_RemoveDirectoryA(const char *path)
{
  return nxdk_RemoveDirectoryA(path);
}

/* 0x1d4464: XQueryValue = RtlNtStatusToDosError(ExQueryNonVolatileSetting()). */
int __stdcall standalone_XQueryValue(int value_index, void *type_out, void *value_buf,
                                     int value_len, void *result_len_out)
{
  return (int)RtlNtStatusToDosError(ExQueryNonVolatileSetting(
      (ULONG)value_index, (PULONG)type_out, value_buf, (ULONG)value_len,
      (PULONG)result_len_out));
}

/* 0x1d2296 */
void __stdcall standalone_XapiSetLastNTError(int status)
{
  nxdk_SetLastError_xv(RtlNtStatusToDosError(status));
}

