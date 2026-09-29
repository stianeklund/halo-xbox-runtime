/*
 * Standalone compat: the LIBCMT wide printf family (vsnwprint.obj & co.).
 *
 *   _vsnwprintf  (0x1dca5f)  unicode_sprintf / usnprintf
 *   FUN_001dcace (0x1dcace)  vswprintf: same body with count 0x7fffffff
 *   FUN_001dd6f5 (0x1dd6f5)  _snwprintf: XAPI's SaveMeta / TitleMeta writers
 *
 * nxdk's pdclib has no wide printf. The originals run LIBCMT's _woutput on
 * a string FILE of count * 2 bytes and then store a wide NUL through two
 * byte puts: output that fills the buffer exactly is left unterminated and
 * its length returned; output that does not fit returns -1 and leaves the
 * first `count` characters.
 *
 * _woutput semantics kept here: %s and %c take WIDE arguments and %S / %C
 * NARROW ones, with h forcing narrow and l / w forcing wide; a NULL string
 * prints "(null)"; '0' pads strings too unless '-' is given; a negative '*'
 * width means left alignment and a negative '*' precision means none; an
 * unknown conversion character is printed as itself. Numbers go through
 * pdclib's narrow printf (crt/msvc_print.c already gives it the
 * MSVC float and I64 behaviour) one conversion at a time, then are widened.
 * %p is eight upper-case hex digits, as in LIBCMT.
 *
 * Halo's wide formats (cachebeta.xbe .rdata, plus the 'ustr' tags) use
 * %s %hs %d %02d %.2f %lc %ls.
 */
#include "xbox.h"

typedef char *wf_va_list;

int __cdecl wf_pd_vsnprintf(char *buffer, unsigned int count, const char *format,
                            wf_va_list args) __asm__("_vsnprintf");

int __cdecl standalone_vswprintf(wchar_t *buffer, const wchar_t *format, char *argptr)
    __asm__("_FUN_001dcace");

#define WF_UNBOUNDED 0x7fffffffu
#define WF_NUMBER_BUFFER 512
#define WF_SPEC_BUFFER 32
#define WF_POINTER_DIGITS 8

#define FL_LEFT 0x01
#define FL_SIGN 0x02
#define FL_SPACE 0x04
#define FL_ALTERNATE 0x08
#define FL_ZERO 0x10

#define SIZE_DEFAULT 0
#define SIZE_SHORT 1
#define SIZE_LONG 2 /* l / w: wide for c/s, 32-bit for integers */
#define SIZE_INT64 3

typedef struct {
  wchar_t *buffer;
  unsigned int count;
  unsigned int length;
  int overflow;
} wf_output;

static const char wf_null_narrow[] = "(null)";

static void wf_put(wf_output *out, wchar_t c)
{
  if (out->overflow) {
    return;
  }
  if (out->length >= out->count) {
    out->overflow = 1;
    return;
  }
  out->buffer[out->length++] = c;
}

static void wf_pad(wf_output *out, wchar_t c, int n)
{
  while (n-- > 0) {
    wf_put(out, c);
  }
}

static int wf_arg_int(char **ap)
{
  int value = *(int *)*ap;

  *ap += sizeof(int);
  return value;
}

static long long wf_arg_int64(char **ap)
{
  long long value = *(long long *)*ap;

  *ap += sizeof(long long);
  return value;
}

static double wf_arg_double(char **ap)
{
  double value = *(double *)*ap;

  *ap += sizeof(double);
  return value;
}

static void *wf_arg_pointer(char **ap)
{
  void *value = *(void **)*ap;

  *ap += sizeof(void *);
  return value;
}

/* pdclib formats one numeric conversion. */
static int wf_narrow(char *buffer, const char *spec, ...)
{
  wf_va_list args;
  int length;

  __builtin_va_start(args, spec);
  length = wf_pd_vsnprintf(buffer, WF_NUMBER_BUFFER, spec, args);
  __builtin_va_end(args);
  if (length < 0) {
    length = 0;
  } else if (length > WF_NUMBER_BUFFER - 1) {
    length = WF_NUMBER_BUFFER - 1;
  }
  return length;
}

/* Appends "%<flags><width>.<precision>" to spec; returns the end. */
static char *wf_spec_prefix(char *spec, int flags, int width, int precision)
{
  char digits[12];
  int n;

  *spec++ = '%';
  if (flags & FL_LEFT) *spec++ = '-';
  if (flags & FL_SIGN) *spec++ = '+';
  if (flags & FL_SPACE) *spec++ = ' ';
  if (flags & FL_ALTERNATE) *spec++ = '#';
  if (flags & FL_ZERO) *spec++ = '0';
  if (width > 0) {
    n = 0;
    do {
      digits[n++] = (char)('0' + width % 10);
      width /= 10;
    } while (width != 0);
    while (n > 0) *spec++ = digits[--n];
  }
  if (precision >= 0) {
    *spec++ = '.';
    n = 0;
    do {
      digits[n++] = (char)('0' + precision % 10);
      precision /= 10;
    } while (precision != 0);
    while (n > 0) *spec++ = digits[--n];
  }
  return spec;
}

/* Text of a string or character conversion, padded to width. */
static void wf_put_text(wf_output *out, const void *text, int wide, int length, int flags,
                        int width)
{
  int i;

  if (!(flags & FL_LEFT)) {
    wf_pad(out, (flags & FL_ZERO) ? L'0' : L' ', width - length);
  }
  for (i = 0; i < length; i++) {
    wf_put(out, wide ? ((const wchar_t *)text)[i]
                     : (wchar_t)((const unsigned char *)text)[i]);
  }
  if (flags & FL_LEFT) {
    wf_pad(out, L' ', width - length);
  }
}

static int wf_format(wf_output *out, const wchar_t *format, char *ap)
{
  while (*format) {
    int flags;
    int width;
    int precision;
    int size;
    wchar_t c;

    c = *format++;
    if (c != L'%') {
      wf_put(out, c);
      continue;
    }

    flags = 0;
    for (;; format++) {
      if (*format == L'-') flags |= FL_LEFT;
      else if (*format == L'+') flags |= FL_SIGN;
      else if (*format == L' ') flags |= FL_SPACE;
      else if (*format == L'#') flags |= FL_ALTERNATE;
      else if (*format == L'0') flags |= FL_ZERO;
      else break;
    }

    width = 0;
    if (*format == L'*') {
      format++;
      width = wf_arg_int(&ap);
      if (width < 0) {
        flags |= FL_LEFT;
        width = -width;
      }
    } else {
      while (*format >= L'0' && *format <= L'9') {
        width = width * 10 + (*format++ - L'0');
      }
    }

    precision = -1;
    if (*format == L'.') {
      format++;
      precision = 0;
      if (*format == L'*') {
        format++;
        precision = wf_arg_int(&ap);
        if (precision < 0) {
          precision = -1;
        }
      } else {
        while (*format >= L'0' && *format <= L'9') {
          precision = precision * 10 + (*format++ - L'0');
        }
      }
    }

    size = SIZE_DEFAULT;
    if (*format == L'h') {
      size = SIZE_SHORT;
      format++;
    } else if (*format == L'l' || *format == L'w') {
      size = SIZE_LONG;
      format++;
      if (*format == L'l') { /* not LIBCMT 7.x; harmless as 64-bit */
        size = SIZE_INT64;
        format++;
      }
    } else if (*format == L'L') {
      format++;
    } else if (*format == L'I') {
      if (format[1] == L'6' && format[2] == L'4') {
        size = SIZE_INT64;
        format += 3;
      } else if (format[1] == L'3' && format[2] == L'2') {
        format += 3;
      } else {
        format++;
      }
    }

    c = *format;
    if (c == 0) {
      break;
    }
    format++;

    switch (c) {
    case L'c':
    case L'C': {
      int wide = (c == L'c') ? (size != SIZE_SHORT) : (size == SIZE_LONG);
      wchar_t wch;
      unsigned char ch;

      if (wide) {
        wch = (wchar_t)wf_arg_int(&ap);
        wf_put_text(out, &wch, 1, 1, flags, width);
      } else {
        ch = (unsigned char)wf_arg_int(&ap);
        wf_put_text(out, &ch, 0, 1, flags, width);
      }
      break;
    }
    case L's':
    case L'S': {
      int wide = (c == L's') ? (size != SIZE_SHORT) : (size == SIZE_LONG);
      const void *text = wf_arg_pointer(&ap);
      int length = 0;

      if (text == 0) {
        text = wf_null_narrow;
        wide = 0;
      }
      if (wide) {
        while ((precision < 0 || length < precision) && ((const wchar_t *)text)[length]) {
          length++;
        }
      } else {
        while ((precision < 0 || length < precision) && ((const char *)text)[length]) {
          length++;
        }
      }
      wf_put_text(out, text, wide, length, flags, width);
      break;
    }
    case L'd':
    case L'i':
    case L'u':
    case L'o':
    case L'x':
    case L'X': {
      char spec[WF_SPEC_BUFFER];
      char number[WF_NUMBER_BUFFER];
      char *end;
      int length;
      int i;

      end = wf_spec_prefix(spec, flags, width, precision);
      if (size == SIZE_INT64) {
        *end++ = 'l';
        *end++ = 'l';
        *end++ = (char)c;
        *end = 0;
        length = wf_narrow(number, spec, wf_arg_int64(&ap));
      } else {
        int value = wf_arg_int(&ap);

        if (size == SIZE_SHORT) {
          value = (c == L'd' || c == L'i') ? (int)(short)value : (int)(unsigned short)value;
        }
        *end++ = (char)c;
        *end = 0;
        length = wf_narrow(number, spec, value);
      }
      for (i = 0; i < length; i++) {
        wf_put(out, (wchar_t)(unsigned char)number[i]);
      }
      break;
    }
    case L'p': {
      char spec[WF_SPEC_BUFFER];
      char number[WF_NUMBER_BUFFER];
      char *end;
      int length;
      int i;

      end = wf_spec_prefix(spec, flags & ~FL_ALTERNATE, width, WF_POINTER_DIGITS);
      *end++ = 'X';
      *end = 0;
      length = wf_narrow(number, spec, (unsigned int)wf_arg_pointer(&ap));
      for (i = 0; i < length; i++) {
        wf_put(out, (wchar_t)(unsigned char)number[i]);
      }
      break;
    }
    case L'e':
    case L'E':
    case L'f':
    case L'g':
    case L'G': {
      char spec[WF_SPEC_BUFFER];
      char number[WF_NUMBER_BUFFER];
      char *end;
      int length;
      int i;

      end = wf_spec_prefix(spec, flags, width, precision);
      *end++ = (char)c;
      *end = 0;
      length = wf_narrow(number, spec, wf_arg_double(&ap));
      for (i = 0; i < length; i++) {
        wf_put(out, (wchar_t)(unsigned char)number[i]);
      }
      break;
    }
    case L'n': {
      void *target = wf_arg_pointer(&ap);

      if (size == SIZE_SHORT) {
        *(short *)target = (short)out->length;
      } else {
        *(int *)target = (int)out->length;
      }
      break;
    }
    default:
      /* '%%' and any unknown conversion character print as themselves. */
      wf_put(out, c);
      break;
    }
  }
  return out->overflow ? -1 : (int)out->length;
}

static int wf_bounded(wchar_t *buffer, unsigned int count, const wchar_t *format, char *ap)
{
  wf_output out;
  int result;

  out.buffer = buffer;
  out.count = count;
  out.length = 0;
  out.overflow = 0;
  result = wf_format(&out, format, ap);
  if (result >= 0 && out.length < count) {
    buffer[out.length] = 0;
  }
  return result;
}

/* 0x1dca5f */
int _vsnwprintf(wchar_t *buffer, size_t count, const wchar_t *format, char *argptr)
{
  return wf_bounded(buffer, (unsigned int)count, format, argptr);
}

/* 0x1dcace: vswprintf, the string FILE sized 0x7fffffff. */
int __cdecl standalone_vswprintf(wchar_t *buffer, const wchar_t *format, char *argptr)
{
  return wf_bounded(buffer, WF_UNBOUNDED, format, argptr);
}

/* 0x1dd6f5: _snwprintf. */
int FUN_001dd6f5(unsigned short *buffer, int count, const unsigned short *format, ...)
{
  wf_va_list args;
  int result;

  __builtin_va_start(args, format);
  result = wf_bounded((wchar_t *)buffer, (unsigned int)count, (const wchar_t *)format, args);
  __builtin_va_end(args);
  return result;
}
