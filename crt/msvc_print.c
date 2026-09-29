/*
 * Standalone CRT: pdclib's printf core with the MSVC behaviours Halo relies
 * on.
 *
 * cachebeta.xbe links the MSVC LIBCMT printf family, which formats a NULL
 * `%s` argument as "(null)". pdclib's stringformat() calls strlen() on it and
 * faults. This TU compiles nxdk's own _PDCLIB_print.c with that single strlen
 * call site redirected, and the object's _PDCLIB_print takes precedence over
 * the libpdclib.lib member at link time.
 *
 * MSVC also takes the length modifiers I64 (64-bit), I32 and I (32-bit on
 * x86), which pdclib does not parse: main_rasterizer_throttle's
 * "%6I64d(targ%6I64d %s%2d)" then consumed the 64-bit arguments as ints and
 * handed a stray word to %s. The _PDCLIB_print wrapper below rewrites those
 * modifiers in a copy of the conversion specification before pdclib sees it.
 *
 * pdclib parses %f/%e/%g but formats nothing and never consumes the double,
 * so every later argument shifts by eight bytes. msvc_print_float formats
 * them the way the MSVC CRT does (three-digit exponent, 1.#INF / 1.#QNAN);
 * digits past double precision are approximate.
 *
 * Only the unbounded path (no precision) is covered; stringformat's precision
 * loop still indexes s directly. Built with nxdk's pdclib flags, not the game
 * flags (crt/CMakeLists.txt).
 */
#include <string.h>

#define MSVC_NULL_STRING "(null)"

/* In stringformat() `s` is the (non-const) parameter; substituting it here
 * makes the rest of the function emit the MSVC text. */
#define strlen(s) ((s) ? (strlen)(s) : ((s) = MSVC_NULL_STRING, sizeof(MSVC_NULL_STRING) - 1))

#define _PDCLIB_print pdclib_print_spec
#include "functions/_PDCLIB/_PDCLIB_print.c"
#undef _PDCLIB_print
#undef strlen

#define MSVC_SPEC_BUFFER_SIZE 64
#define MSVC_SPEC_PREFIX_CHARS "-+ #0123456789.*"

#define MSVC_FLOAT_BUFFER_SIZE 400
#define MSVC_FLOAT_DEFAULT_PRECISION 6
#define MSVC_FLOAT_MAX_PRECISION 60
#define MSVC_FLOAT_EXPONENT_DIGITS 3
#define MSVC_FLOAT_FIXED_INTEGER_LIMIT 9.0e18
#define MSVC_FLOAT_G_MIN_EXPONENT -4
#define MSVC_FLOAT_SIGN_BIT 63

/* Normalizes v > 0 to [1, 10) and returns the decimal exponent. */
static int msvc_float_normalize(double *v)
{
    int exponent = 0;

    while (*v >= 10.0) {
        *v /= 10.0;
        ++exponent;
    }
    while (*v < 1.0) {
        *v *= 10.0;
        --exponent;
    }
    return exponent;
}

/* count >= 1 significant digits of v >= 0, rounded; returns the exponent of
 * the first digit. */
static int msvc_float_digits(double v, int count, char *digits)
{
    double half = 0.5;
    int exponent;
    int i;

    if (v == 0.0) {
        memset(digits, '0', (size_t)count);
        return 0;
    }
    exponent = msvc_float_normalize(&v);
    for (i = 1; i < count; ++i) {
        half /= 10.0;
    }
    v += half;
    if (v >= 10.0) {
        v /= 10.0;
        ++exponent;
    }
    for (i = 0; i < count; ++i) {
        int digit = (int)v;

        if (digit > 9) {
            digit = 9;
        }
        digits[i] = (char)('0' + digit);
        v = (v - digit) * 10.0;
    }
    return exponent;
}

static char *msvc_float_fixed(char *out, double v, int precision, int alternate)
{
    double half = 0.5;
    int i;

    for (i = 0; i < precision; ++i) {
        half /= 10.0;
    }
    v += half;
    if (v < MSVC_FLOAT_FIXED_INTEGER_LIMIT) {
        long long integer = (long long)v;
        char reversed[20];
        int count = 0;

        v -= (double)integer;
        do {
            reversed[count++] = (char)('0' + (int)(integer % 10));
            integer /= 10;
        } while (integer != 0);
        while (count > 0) {
            *out++ = reversed[--count];
        }
    } else {
        double scaled = v;
        int exponent = msvc_float_normalize(&scaled);

        for (i = 0; i <= exponent; ++i) {
            int digit = (int)scaled;

            *out++ = (char)('0' + (digit > 9 ? 9 : digit));
            scaled = (scaled - digit) * 10.0;
        }
        v = 0.0;
    }
    if (precision > 0 || alternate) {
        *out++ = '.';
    }
    for (i = 0; i < precision; ++i) {
        int digit;

        v *= 10.0;
        digit = (int)v;
        if (digit > 9) {
            digit = 9;
        }
        *out++ = (char)('0' + digit);
        v -= digit;
    }
    return out;
}

static char *msvc_float_exponential(char *out, double v, int precision, int alternate,
                                    char marker)
{
    char digits[MSVC_FLOAT_MAX_PRECISION + 1];
    int exponent = msvc_float_digits(v, precision + 1, digits);
    int divisor;
    int i;

    *out++ = digits[0];
    if (precision > 0 || alternate) {
        *out++ = '.';
    }
    for (i = 1; i <= precision; ++i) {
        *out++ = digits[i];
    }
    *out++ = marker;
    *out++ = exponent < 0 ? '-' : '+';
    if (exponent < 0) {
        exponent = -exponent;
    }
    for (divisor = 100; divisor > 0; divisor /= 10) {
        *out++ = (char)('0' + (exponent / divisor) % 10);
    }
    return out;
}

/* Removes trailing fractional zeros (and a bare point) from the mantissa in
 * [start, end), keeping any exponent suffix. */
static char *msvc_float_strip_zeros(char *start, char *end)
{
    char *point = memchr(start, '.', (size_t)(end - start));
    char *mantissa_end;
    char *exponent;

    if (point == NULL) {
        return end;
    }
    exponent = point;
    while (exponent < end && *exponent != 'e' && *exponent != 'E') {
        ++exponent;
    }
    mantissa_end = exponent;
    while (mantissa_end > point + 1 && mantissa_end[-1] == '0') {
        --mantissa_end;
    }
    if (mantissa_end == point + 1) {
        --mantissa_end;
    }
    memmove(mantissa_end, exponent, (size_t)(end - exponent));
    return mantissa_end + (end - exponent);
}

static const char *msvc_print_float(const char *spec, struct _PDCLIB_status_t *status)
{
    char buffer[MSVC_FLOAT_BUFFER_SIZE];
    const char *p = spec + 1;
    char *body;
    char *end;
    char conversion;
    int minus = 0, plus = 0, space = 0, alternate = 0, zero = 0;
    int width = 0;
    int precision = -1;
    double v;
    unsigned long long bits;

    for (;; ++p) {
        if (*p == '-') {
            minus = 1;
        } else if (*p == '+') {
            plus = 1;
        } else if (*p == ' ') {
            space = 1;
        } else if (*p == '#') {
            alternate = 1;
        } else if (*p == '0') {
            zero = 1;
        } else {
            break;
        }
    }
    if (*p == '*') {
        width = va_arg(status->arg, int);
        if (width < 0) {
            minus = 1;
            width = -width;
        }
        ++p;
    } else {
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p++ - '0');
        }
    }
    if (*p == '.') {
        ++p;
        precision = 0;
        if (*p == '*') {
            precision = va_arg(status->arg, int);
            ++p;
        } else {
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p++ - '0');
            }
        }
    }
    if (precision < 0) {
        precision = MSVC_FLOAT_DEFAULT_PRECISION;
    }
    if (precision > MSVC_FLOAT_MAX_PRECISION) {
        precision = MSVC_FLOAT_MAX_PRECISION;
    }
    if (*p == 'l' || *p == 'L') {
        ++p;
    }
    conversion = *p;
    v = va_arg(status->arg, double);

    body = buffer;
    memcpy(&bits, &v, sizeof(bits));
    if (bits >> MSVC_FLOAT_SIGN_BIT) {
        *body++ = '-';
        v = -v;
    } else if (plus) {
        *body++ = '+';
    } else if (space) {
        *body++ = ' ';
    }
    if (v != v) {
        strcpy(body, "1.#QNAN");
        end = body + strlen(body);
        zero = 0;
    } else if (v > 1.7976931348623157e308) {
        strcpy(body, "1.#INF");
        end = body + strlen(body);
        zero = 0;
    } else if (conversion == 'f' || conversion == 'F') {
        end = msvc_float_fixed(body, v, precision, alternate);
    } else if (conversion == 'e' || conversion == 'E') {
        end = msvc_float_exponential(body, v, precision, alternate, conversion);
    } else {
        char digits[MSVC_FLOAT_MAX_PRECISION + 1];
        int significant = precision == 0 ? 1 : precision;
        int exponent = msvc_float_digits(v, significant, digits);

        if (exponent >= MSVC_FLOAT_G_MIN_EXPONENT && exponent < significant) {
            end = msvc_float_fixed(body, v, significant - 1 - exponent, alternate);
        } else {
            end = msvc_float_exponential(body, v, significant - 1, alternate,
                                         conversion == 'G' ? 'E' : 'e');
        }
        if (!alternate) {
            end = msvc_float_strip_zeros(body, end);
        }
    }
    *end = '\0';
    if (zero && !minus && width > end - buffer) {
        size_t pad = (size_t)width - (size_t)(end - buffer);

        memmove(body + pad, body, (size_t)(end - body) + 1);
        memset(body, '0', pad);
    }

    status->flags = minus ? E_minus : 0;
    status->width = (_PDCLIB_size_t)width;
    status->prec = EOF;
    status->current = 0;
    stringformat(buffer, status);
    return ++p;
}

const char *_PDCLIB_print(const char *spec, struct _PDCLIB_status_t *status)
{
    char buffer[MSVC_SPEC_BUFFER_SIZE];
    const char *modifier = spec + 1;
    const char *replacement;
    const char *rc;
    char conversion;
    size_t head;
    size_t skipped;
    size_t replaced;

    while (*modifier != '\0' && strchr(MSVC_SPEC_PREFIX_CHARS, *modifier) != NULL) {
        ++modifier;
    }
    conversion = modifier[*modifier == 'l' || *modifier == 'L'];
    if (conversion != '\0' && strchr("fFeEgG", conversion) != NULL) {
        return msvc_print_float(spec, status);
    }
    if (*modifier != 'I') {
        return pdclib_print_spec(spec, status);
    }
    if (modifier[1] == '6' && modifier[2] == '4') {
        replacement = "ll";
        skipped = 3;
    } else if (modifier[1] == '3' && modifier[2] == '2') {
        replacement = "";
        skipped = 3;
    } else {
        replacement = "";
        skipped = 1;
    }
    head = (size_t)(modifier - spec);
    replaced = strlen(replacement);
    /* prefix, replacement, conversion character, terminator */
    if (head + replaced + 2 > sizeof(buffer) || modifier[skipped] == '\0') {
        return pdclib_print_spec(spec, status);
    }
    memcpy(buffer, spec, head);
    memcpy(buffer + head, replacement, replaced);
    buffer[head + replaced] = modifier[skipped];
    buffer[head + replaced + 1] = '\0';

    rc = pdclib_print_spec(buffer, status);
    if (rc == NULL) {
        return NULL;
    }
    if (rc == buffer) {
        return spec;
    }
    return spec + (size_t)(rc - buffer) - replaced + skipped;
}
