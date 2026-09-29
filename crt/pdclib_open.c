/*
 * Standalone CRT: nxdk's _PDCLIB_open with "a+" accepted.
 *
 * _PDCLIB_filemode("a+") yields FAPPEND | FWRITE | FRW, but nxdk's xbox
 * _PDCLIB_open only has a case for FAPPEND | FRW, so every "a+" fopen falls
 * into "invalid mode" and fails without setting an error. Halo's
 * debug_string_to_display opens d:\debug.txt with "a+b", so nothing was ever
 * logged. This TU compiles nxdk's own implementation under another name and
 * clears FWRITE for "a+" before calling it; the object's _PDCLIB_open takes
 * precedence over the libpdclib.lib member at link time.
 */
#include <stdio.h>

#define _PDCLIB_open pdclib_open_nxdk
#include "platform/xbox/functions/_PDCLIB/_PDCLIB_open.c"
#undef _PDCLIB_open

_PDCLIB_fd_t _PDCLIB_open(const char * const filename, unsigned int mode)
{
  if ((mode & _PDCLIB_FAPPEND) && (mode & _PDCLIB_FRW)) {
    mode &= ~_PDCLIB_FWRITE;
  }
  return pdclib_open_nxdk(filename, mode);
}
