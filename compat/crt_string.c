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
#include "xbox.h"

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
