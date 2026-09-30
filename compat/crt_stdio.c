/*
 * Standalone compat: the LIBCMT stdio/string routines kb.json names crt_*.
 *
 * cachebeta.xbe links MSVC LIBCMT; the standalone build links nxdk's pdclib
 * instead, so these forward to it. Until they existed the generated stubs
 * returned 0: crt_fopen NULL (debug_string_to_display wrote nothing to
 * d:\debug.txt) and crt_sprintf / crt_vsnprintf left every buffer untouched.
 *
 * Streams are pdclib FILE pointers except the LIBCMT _iob entries that lifted
 * code passes by address (&_iob[1] stdout, &_iob[2] stderr): those go to
 * DbgPrint, the Xbox's only console.
 */
#include "xbox.h"

typedef char *crt_va_list;
#define crt_va_start(ap, last) __builtin_va_start(ap, last)
#define crt_va_end(ap) __builtin_va_end(ap)

void *__cdecl pd_fopen(const char *filename, const char *mode) __asm__("_fopen");
int __cdecl pd_fclose(void *stream) __asm__("_fclose");
int __cdecl pd_fflush(void *stream) __asm__("_fflush");
char *__cdecl pd_fgets(char *buffer, int max_count, void *stream) __asm__("_fgets");
unsigned int __cdecl pd_fread(void *buffer, unsigned int size, unsigned int count, void *stream)
  __asm__("_fread");
int __cdecl pd_vfprintf(void *stream, const char *format, crt_va_list args)
  __asm__("_vfprintf");
int __cdecl pd_vsprintf(char *buffer, const char *format, crt_va_list args)
  __asm__("_vsprintf");
int __cdecl pd_vsnprintf(char *buffer, unsigned int count, const char *format,
                         crt_va_list args) __asm__("_vsnprintf");

/* LIBCMT _iob[0..2], 0x20 bytes each (stderr is 0x331050). */
unsigned long __stdcall pd_GetLastError(void) __asm__("_GetLastError@0");

#define CRT_IOB_FIRST 0x331010ul
#define CRT_IOB_END 0x331070ul
#define CRT_FORMAT_BUFFER 4096

static int crt_is_iob(void *stream)
{
  return (unsigned long)stream >= CRT_IOB_FIRST && (unsigned long)stream < CRT_IOB_END;
}

void *crt_fopen(const char *filename, const char *mode)
{
  void *stream;

  stream = pd_fopen(filename, mode);
  if (stream == 0)
    DbgPrint("[crt] fopen('%s', '%s') failed, last error %lu\n", filename, mode,
             pd_GetLastError());
  return stream;
}

int crt_fclose(void *stream)
{
  if (crt_is_iob(stream))
    return 0;
  return pd_fclose(stream);
}

int crt_fflush(void *stream)
{
  if (stream == 0 || crt_is_iob(stream))
    return 0;
  return pd_fflush(stream);
}

char *crt_fgets(char *buffer, int max_count, void *stream)
{
  if (crt_is_iob(stream))
    return 0;
  return pd_fgets(buffer, max_count, stream);
}

/* 0x1db3f7 (kb FUN_001db3f7) is LIBCMT fread, not fwrite: its _filbuf path
 * (call at 0x1db3b5) stores the refilled byte into the caller's buffer. The
 * callers are zlib's gz reads and the "z:\last_solo.txt" reader at 0x101e4c. */
unsigned int FUN_001db3f7(void *ptr, unsigned int size, unsigned int count, void *stream)
{
  if (crt_is_iob(stream))
    return 0;
  return pd_fread(ptr, size, count, stream);
}

int crt_fprintf(void *stream, const char *format, ...)
{
  char line[CRT_FORMAT_BUFFER];
  crt_va_list args;
  int result;

  crt_va_start(args, format);
  if (crt_is_iob(stream)) {
    result = pd_vsnprintf(line, sizeof(line), format, args);
    DbgPrint("%s", line);
  } else {
    result = pd_vfprintf(stream, format, args);
  }
  crt_va_end(args);
  return result;
}

int crt_sprintf(char *buffer, const char *format, ...)
{
  crt_va_list args;
  int result;

  crt_va_start(args, format);
  result = pd_vsprintf(buffer, format, args);
  crt_va_end(args);
  return result;
}

/* MSVC _vsnprintf: no terminator when the output fills the buffer, and -1
 * when it does not fit. Output past CRT_FORMAT_BUFFER - 1 characters is cut
 * there. */
int crt_vsnprintf(char *buffer, size_t count, const char *format, char *arglist)
{
  char full[CRT_FORMAT_BUFFER];
  int length;
  size_t copy;
  size_t i;

  length = pd_vsnprintf(full, sizeof(full), format, arglist);
  if (length < 0)
    return -1;
  copy = (size_t)length < count ? (size_t)length : count;
  if (copy > sizeof(full) - 1)
    copy = sizeof(full) - 1;
  for (i = 0; i < copy; i++)
    buffer[i] = full[i];
  if ((size_t)length < count) {
    buffer[length] = '\0';
    return length;
  }
  return (size_t)length == count ? length : -1;
}

char *crt_strncat(char *dest, const char *src, size_t count)
{
  char *p;

  p = dest;
  while (*p)
    p++;
  while (count != 0 && *src) {
    *p++ = *src++;
    count--;
  }
  *p = '\0';
  return dest;
}

char *crt_strtok(char *string, const char *delimiters)
{
  static char *next;
  const char *d;
  char *token;

  if (string == 0)
    string = next;
  if (string == 0)
    return 0;
  for (; *string; string++) {
    for (d = delimiters; *d && *d != *string; d++) {
    }
    if (*d == '\0')
      break;
  }
  if (*string == '\0') {
    next = 0;
    return 0;
  }
  token = string;
  for (; *string; string++) {
    for (d = delimiters; *d && *d != *string; d++) {
    }
    if (*d != '\0') {
      *string++ = '\0';
      next = string;
      return token;
    }
  }
  next = 0;
  return token;
}

/* 0x1d980b: LIBCMT exit(code). doexit (0x1d9761) runs the atexit table and
 * the C terminators (which flush every stream), then KeBugCheck(0), so it
 * never returns. Halo calls it only on fatal paths: a re-entered
 * halt_and_catch_fire (main.c) and libtiff errors. Returning from it, as the
 * generated stub did, runs past system_exit's __noreturn into unrelated code.
 * Flush pdclib's streams so d:\debug.txt keeps the exception, then stop. */
void FUN_001d980b(int code)
{
  (void)code;
  pd_fflush(0);
  KeBugCheck(0);
}

/* Seconds since 1970-01-01 UTC, as LIBCMT time() returns them. */
unsigned int crt_time(int *timer)
{
  LARGE_INTEGER now;
  unsigned int seconds;

  KeQuerySystemTime(&now);
  seconds = (unsigned int)((now.QuadPart - 116444736000000000LL) / 10000000LL);
  if (timer)
    *timer = (int)seconds;
  return seconds;
}

/* "C" locale classification (LIBCMT _ctype table): nothing above 0x7f. */
int crt_isspace(int c)
{
  return c == ' ' || (c >= '\t' && c <= '\r');
}

int crt_isgraph(int c)
{
  return c > ' ' && c < 0x7f;
}
