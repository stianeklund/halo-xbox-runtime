/*
 * Standalone compat: LIBCMT string internals kb.json only knows by address.
 *
 * __ascii_stricmp (0x1e3a50) is the "C" locale core of _stricmp that Halo's
 * crt_stricmp (0x1dd801) calls. A logging stub returning 0 reports every pair
 * of strings as equal: FUN_001bd1b0 then "finds" the UI map in empty cache
 * slot 0 and cache_file_header_verify rejects the zeroed header ("'levels\ui\ui'
 * does not appear to be a cache file"). Same algorithm as the MSVC routine:
 * fold 'A'..'Z' only, stop at the first difference or the terminator, return
 * the difference of the folded bytes.
 */
#include <stddef.h>

int __ascii_stricmp(const char *a, const char *b)
{
  int f;
  int l;

  do {
    f = (unsigned char)*a++;
    if (f >= 'A' && f <= 'Z')
      f -= 'A' - 'a';
    l = (unsigned char)*b++;
    if (l >= 'A' && l <= 'Z')
      l -= 'A' - 'a';
  } while (f != 0 && f == l);
  return f - l;
}

/* __wcsnicmp (0x1dc34b), default "C" locale path in 2276: compare unsigned
 * UTF-16 code units, fold ASCII capitals only, and return their difference.
 * get_icon_type relies on nonzero for a mismatch; a zero-returning stub
 * incorrectly identifies every icon name as table entry 0. */
int __wcsnicmp(const wchar_t *a, const wchar_t *b, size_t count)
{
  int f;
  int l;

  if (count == 0)
    return 0;
  do {
    f = (unsigned short)*a++;
    if (f >= 'A' && f <= 'Z')
      f -= 'A' - 'a';
    l = (unsigned short)*b++;
    if (l >= 'A' && l <= 'Z')
      l -= 'A' - 'a';
    count--;
  } while (count != 0 && f != 0 && f == l);
  return f - l;
}
