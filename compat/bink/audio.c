/*
 * Standalone compat: Bink audio decoder (RAD's FUN_0023cd10/FUN_0023ce50/
 * FUN_0023ec50), the RDFT variant every Bink 1 file carries.
 *
 * Bitstream parsing follows FFmpeg's libavcodec/binkaudio.c (Copyright (c)
 * 2007-2011 Peter Ross, 2009 Daniel Verkamp; LGPL-2.1 or later). The inverse
 * real FFT and the libm-free helpers are local: the title links against
 * pdclib only and builds with -mno-sse, so everything runs on the x87.
 *
 * One FUN_0023ec50 call decodes one transform block (the input stays 32-bit
 * aligned relative to the chunk start BinkDoFrame hands in) and returns
 * interleaved signed 16-bit PCM owned by the decoder.
 */
#include "internal.h"
#include "tables.h"

#define MAX_FRAME_LEN_BITS 12
#define MAX_FRAME_LEN (1 << MAX_FRAME_LEN_BITS)
#define NUM_QUANT 96
#define TWO_PI 6.28318530717958647692

struct bink_audio {
  int frame_len;
  int frame_len_bits;
  int overlap_len;
  int block_size;
  int num_bands;
  int first;
  float root;
  unsigned int bands[26];
  float quant_table[NUM_QUANT];
  float previous[MAX_FRAME_LEN / 16];
  float cos_table[MAX_FRAME_LEN / 4 + 1]; /* cos(2*pi*k/frame_len), quarter wave */
  float coeffs[MAX_FRAME_LEN + 2];
  short output[MAX_FRAME_LEN];
};

struct bitreader {
  const unsigned char *data;
  unsigned long pos;
};

static unsigned long get_bits(struct bitreader *gb, int n)
{
  const unsigned char *p = gb->data + (gb->pos >> 3);
  unsigned long v = *(const bink_u32 *)p >> (gb->pos & 7);

  if (n == 0) {
    return 0;
  }
  gb->pos += n;
  return v & ((1ul << n) - 1);
}

static double exp_series(double x)
{
  double sum = 1.0, term = 1.0;
  int i, halvings = 0;

  while (x > 0.5) {
    x *= 0.5;
    halvings++;
  }
  for (i = 1; i < 16; i++) {
    term *= x / i;
    sum += term;
  }
  while (halvings--) {
    sum *= sum;
  }
  return sum;
}

static double sqrt_newton(double x)
{
  double r = x > 1.0 ? x : 1.0;
  int i;

  for (i = 0; i < 64; i++) {
    r = 0.5 * (r + x / r);
  }
  return r;
}

static float pow2f(int e)
{
  float f = 1.0f;

  while (e > 0) {
    f *= 2.0f;
    e--;
  }
  while (e < 0) {
    f *= 0.5f;
    e++;
  }
  return f;
}

/* ldexpf(mantissa, power - 23) with a sign bit */
static float get_float(struct bitreader *gb)
{
  int power = (int)get_bits(gb, 5);
  float f = (float)get_bits(gb, 23) * pow2f(power - 23);

  if (get_bits(gb, 1)) {
    f = -f;
  }
  return f;
}

static void build_cos_table(struct bink_audio *s)
{
  double step = TWO_PI / s->frame_len;
  double c, sn, term, cr = 1.0, ci = 0.0, t;
  int i;

  /* cos/sin of one step by series, then rotate */
  term = 1.0;
  c = 1.0;
  for (i = 1; i < 12; i++) {
    term *= -step * step / ((2 * i - 1) * (2 * i));
    c += term;
  }
  term = step;
  sn = step;
  for (i = 1; i < 12; i++) {
    term *= -step * step / ((2 * i) * (2 * i + 1));
    sn += term;
  }
  for (i = 0; i <= s->frame_len / 4; i++) {
    s->cos_table[i] = (float)cr;
    t = cr * c - ci * sn;
    ci = cr * sn + ci * c;
    cr = t;
  }
  s->cos_table[s->frame_len / 4] = 0.0f;
}

/* e^(2*pi*i*m/frame_len) for 0 <= m < frame_len / 2 */
static void twiddle(const struct bink_audio *s, int m, float *re, float *im)
{
  int quarter = s->frame_len / 4;

  if (m <= quarter) {
    *re = s->cos_table[m];
    *im = s->cos_table[quarter - m];
  } else {
    *re = -s->cos_table[s->frame_len / 2 - m];
    *im = s->cos_table[m - quarter];
  }
}

/* In-place inverse complex FFT (positive exponent, unscaled) of n points
 * stored as interleaved re/im; n = frame_len / 2. */
static void inverse_fft(const struct bink_audio *s, float *z, int n)
{
  int i, j, bit, len, k;

  for (i = 1, j = 0; i < n; i++) {
    for (bit = n >> 1; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j |= bit;
    if (i < j) {
      float tr = z[2 * i], ti = z[2 * i + 1];
      z[2 * i] = z[2 * j];
      z[2 * i + 1] = z[2 * j + 1];
      z[2 * j] = tr;
      z[2 * j + 1] = ti;
    }
  }
  for (len = 2; len <= n; len <<= 1) {
    int half = len >> 1;
    int stride = s->frame_len / len; /* e^(2*pi*i*k/len) = twiddle(k * stride) */
    for (k = 0; k < half; k++) {
      float wr, wi;
      twiddle(s, k * stride, &wr, &wi);
      for (i = k; i < n; i += len) {
        float *a = z + 2 * i, *b = z + 2 * (i + half);
        float tr = b[0] * wr - b[1] * wi;
        float ti = b[0] * wi + b[1] * wr;
        b[0] = a[0] - tr;
        b[1] = a[1] - ti;
        a[0] += tr;
        a[1] += ti;
      }
    }
  }
}

/* Inverse real DFT of frame_len points in place. On entry c[2k], c[2k+1] hold
 * Re/Im of X[k] for 0 <= k < N/2 with c[1] = X[N/2] (both real bins packed);
 * on exit c[n] = sum over the Hermitian spectrum of X[k] e^(2*pi*i*k*n/N). */
static void inverse_rdft(const struct bink_audio *s, float *c)
{
  int half = s->frame_len / 2, k;
  float x0 = c[0], xn = c[1];

  /* z[m] = x[2m] + i*x[2m+1] is the half-size inverse DFT of
   * Z[k] = E[k] + i*O[k], E[k] = X[k] + conj(X[N/2-k]),
   * O[k] = (X[k] - conj(X[N/2-k])) * e^(2*pi*i*k/N). */
  c[0] = x0 + xn;
  c[1] = x0 - xn;
  for (k = 1; k <= half / 2; k++) {
    int m = half - k;
    float kr = c[2 * k], ki = c[2 * k + 1];
    float mr = c[2 * m], mi = c[2 * m + 1];
    float wr, wi, dr, di;

    twiddle(s, k, &wr, &wi);
    dr = kr - mr;
    di = ki + mi;
    c[2 * k] = kr + mr - (dr * wi + di * wr);
    c[2 * k + 1] = ki - mi + (dr * wr - di * wi);
    if (m != k) {
      twiddle(s, m, &wr, &wi);
      dr = mr - kr;
      di = mi + ki;
      c[2 * m] = mr + kr - (dr * wi + di * wr);
      c[2 * m + 1] = mi - ki + (dr * wr - di * wi);
    }
  }
  inverse_fft(s, c, half);
}

/* 0x23cd10. */
void *__stdcall FUN_0023cd10(unsigned long frequency, unsigned long channels, unsigned long flags)
{
  struct bink_audio *s;
  unsigned long sample_rate, sample_rate_half;
  int i, bits;

  (void)flags;
  if (channels < 1 || channels > 2 || frequency == 0) {
    return 0;
  }
  s = (struct bink_audio *)FUN_002314b0(sizeof(*s));
  if (!s) {
    return 0;
  }
  bits = frequency < 22050 ? 9 : frequency < 44100 ? 10 : 11;
  /* the RDFT variant codes both channels interleaved in one transform */
  sample_rate = frequency * channels;
  if (channels == 2) {
    bits++;
  }
  s->frame_len_bits = bits;
  s->frame_len = 1 << bits;
  s->overlap_len = s->frame_len / 16;
  s->block_size = s->frame_len - s->overlap_len;
  sample_rate_half = (sample_rate + 1) / 2;
  s->root = (float)(2.0 / (sqrt_newton((double)s->frame_len) * 32768.0));
  for (i = 0; i < NUM_QUANT; i++) {
    s->quant_table[i] = (float)exp_series(i * 0.15289164787221953823) * s->root;
  }
  for (s->num_bands = 1; s->num_bands < 25; s->num_bands++) {
    if (sample_rate_half <= bink_critical_freqs[s->num_bands - 1]) {
      break;
    }
  }
  s->bands[0] = 2;
  for (i = 1; i < s->num_bands; i++) {
    s->bands[i] = (unsigned int)((bink_critical_freqs[i - 1] * (unsigned long)s->frame_len /
                                  sample_rate_half) & ~1ul);
  }
  s->bands[s->num_bands] = (unsigned int)s->frame_len;
  s->first = 1;
  build_cos_table(s);
  return s;
}

/* 0x23ce50. */
void __stdcall FUN_0023ce50(void *decoder)
{
  if (decoder) {
    FUN_00231520(decoder);
  }
}

static const unsigned char rle_length_tab[16] = {
  2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64
};

/* 0x23ec50. */
void __stdcall FUN_0023ec50(void *decoder, void **output, unsigned long *output_bytes,
                            const unsigned char *input, const unsigned char **next_input)
{
  struct bink_audio *s = (struct bink_audio *)decoder;
  struct bitreader gb;
  float *coeffs = s->coeffs;
  float quant[25], q;
  int i, j, k, width, coeff, count;

  gb.data = input;
  gb.pos = 0;

  coeffs[0] = get_float(&gb) * s->root;
  coeffs[1] = get_float(&gb) * s->root;
  for (i = 0; i < s->num_bands; i++) {
    int value = (int)get_bits(&gb, 8);
    quant[i] = s->quant_table[value < NUM_QUANT ? value : NUM_QUANT - 1];
  }

  k = 0;
  q = quant[0];
  i = 2;
  while (i < s->frame_len) {
    if (get_bits(&gb, 1)) {
      j = i + rle_length_tab[get_bits(&gb, 4)] * 8;
    } else {
      j = i + 8;
    }
    if (j > s->frame_len) {
      j = s->frame_len;
    }
    width = (int)get_bits(&gb, 4);
    if (width == 0) {
      for (; i < j; i++) {
        coeffs[i] = 0.0f;
      }
      while (s->bands[k] < (unsigned int)i) {
        q = quant[k++];
      }
    } else {
      while (i < j) {
        if (s->bands[k] == (unsigned int)i) {
          q = quant[k++];
        }
        coeff = (int)get_bits(&gb, width);
        if (coeff) {
          coeffs[i] = get_bits(&gb, 1) ? -q * coeff : q * coeff;
        } else {
          coeffs[i] = 0.0f;
        }
        i++;
      }
    }
  }

  /* FFmpeg feeds conj() of the coded bins to a C2R transform scaled by 0.5 */
  for (i = 3; i < s->frame_len; i += 2) {
    coeffs[i] = -coeffs[i];
  }
  inverse_rdft(s, coeffs);

  count = s->overlap_len;
  if (!s->first) {
    for (i = 0; i < s->overlap_len; i++) {
      coeffs[i] = (s->previous[i] * (count - i) + coeffs[i] * i) / count;
    }
  }
  for (i = 0; i < s->overlap_len; i++) {
    s->previous[i] = coeffs[s->frame_len - s->overlap_len + i];
  }
  s->first = 0;

  for (i = 0; i < s->block_size; i++) {
    float f = coeffs[i] * 0.5f * 32768.0f;
    long v = (long)(f < 0.0f ? f - 0.5f : f + 0.5f);
    if (v > 32767) {
      v = 32767;
    } else if (v < -32768) {
      v = -32768;
    }
    s->output[i] = (short)v;
  }

  *output = s->output;
  *output_bytes = (unsigned long)s->block_size * 2;
  *next_input = input + ((gb.pos + 31) >> 5) * 4;
}
