/*
 * Standalone compat: XAPILIB launch-data entry points.
 *
 * Behavior transcribed from cachebeta.xbe.
 */
#include "xbox.h"

#define ERROR_NOT_FOUND 0x490
#define LDT_FROM_DASHBOARD 2
#define LDT_FROM_DEBUGGER_CMDLINE 3
/* XBE header: certificate pointer at image base + 0x118, title ID at +8. */
#define XBE_CERTIFICATE_ADDRESS 0x10118

/* Kernel data export: the import slot holds the variable's address. xboxkrnl.h
 * declares it without dllimport, so name the slot directly. */
extern PLAUNCH_DATA_PAGE *standalone_imp_LaunchDataPage __asm__("__imp__LaunchDataPage");

/* decl.h carries a placeholder `void XGetLaunchInfo(void)` for this kb entry;
 * bind the real __stdcall signature to the decorated name explicitly. */
DWORD __stdcall standalone_XGetLaunchInfo(DWORD *launch_data_type, void *launch_data)
    __asm__("_XGetLaunchInfo@8");

/* 0x1d2518. Hands the kernel launch-data page to the title exactly once. */
DWORD __stdcall standalone_XGetLaunchInfo(DWORD *launch_data_type, void *launch_data)
{
  PLAUNCH_DATA_PAGE page;
  DWORD type;
  const DWORD *src;
  DWORD *dst;
  int i;

  page = *standalone_imp_LaunchDataPage;
  if (!page) {
    return ERROR_NOT_FOUND;
  }
  type = page->Header.dwLaunchDataType;
  if (type != LDT_FROM_DASHBOARD && type != LDT_FROM_DEBUGGER_CMDLINE &&
      page->Header.dwTitleId !=
          *(DWORD *)(*(unsigned char **)XBE_CERTIFICATE_ADDRESS + 8)) {
    return ERROR_NOT_FOUND;
  }
  *launch_data_type = type;
  src = (const DWORD *)page->LaunchData;
  dst = (DWORD *)launch_data;
  for (i = 0; i < 0x300; i++) {
    dst[i] = src[i];
  }
  *standalone_imp_LaunchDataPage = NULL;
  MmFreeContiguousMemory(page);
  return 0;
}
