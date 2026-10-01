/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Standalone compat: Bink 1 video decoder (RAD's FUN_00236210/FUN_00238070).
 *
 * Halo's decompiled Bink front end (halo/src/halo/bink) calls these RAD
 * internals once per frame; they decode one compressed frame into the BINK
 * plane buffers and mark changed 16x16 cells in the dirty mask.
 *
 * The bitstream decoding follows FFmpeg's libavcodec/bink.c and binkdsp.c
 * (Copyright (c) 2009 Konstantin Shishkov, (C) 2011 Peter Ross; LGPL-2.1 or
 * later), restricted to the revisions the Xbox SDK writes ('f' through 'i')
 * and restructured around RAD's buffers:
 *
 *   plane buffer = Y (mask_width x mask_height), Cb, Cr (half size each)
 *   bundles      = nine scratch buffers carved by BinkOpen from
 *                  FUN_00236210's sizes; a bundle is rewound whenever all of
 *                  its values were consumed, so a buffer only needs to hold
 *                  one run (1 << len values) instead of a whole plane.
 */
#include "internal.h"
#include "tables.h"

enum {
  SRC_BLOCK_TYPES,
  SRC_SUB_BLOCK_TYPES,
  SRC_COLORS,
  SRC_PATTERN,
  SRC_X_OFF,
  SRC_Y_OFF,
  SRC_INTRA_DC,
  SRC_INTER_DC,
  SRC_RUN
};

enum {
  SKIP_BLOCK,
  SCALED_BLOCK,
  MOTION_BLOCK,
  RUN_BLOCK,
  RESIDUE_BLOCK,
  INTRA_BLOCK,
  FILL_BLOCK,
  INTER_BLOCK,
  PATTERN_BLOCK,
  RAW_BLOCK
};

#define DC_START_BITS 11
#define VLC_BITS 7

struct bitreader {
  const unsigned char *data;
  unsigned long pos;
};

struct tree {
  int vlc_num;
  unsigned char syms[16];
};

struct bundle {
  int len;
  struct tree tree;
  unsigned char *data;
  unsigned char *data_end;
  unsigned char *cur_dec; /* NULL once the plane's list is exhausted */
  unsigned char *cur_ptr;
};

struct context {
  struct bitreader gb;
  struct bundle bundle[BINK_BUNDLE_COUNT];
  struct tree col_high[16];
  int col_lastval;
  int version_i; /* revision 'i' (unsigned colours, 32-bit plane headers) */
};

/* Per-tree lookup: low byte symbol index, high byte code length. */
static unsigned short vlc_tables[16][1 << VLC_BITS];
static int vlc_ready;

static void init_vlcs(void)
{
  int t, s, i;

  for (t = 0; t < 16; t++) {
    for (s = 0; s < 16; s++) {
      int len = bink_tree_lens[t][s];
      for (i = bink_tree_bits[t][s]; i < (1 << VLC_BITS); i += 1 << len) {
        vlc_tables[t][i] = (unsigned short)(s | (len << 8));
      }
    }
  }
  vlc_ready = 1;
}

static unsigned long peek_bits(const struct bitreader *gb)
{
  const unsigned char *p = gb->data + (gb->pos >> 3);
  return *(const bink_u32 *)p >> (gb->pos & 7);
}

/* n <= 25 */
static unsigned long get_bits(struct bitreader *gb, int n)
{
  unsigned long v;

  if (n == 0) {
    return 0;
  }
  v = peek_bits(gb) & ((1ul << n) - 1);
  gb->pos += n;
  return v;
}

static int get_bit(struct bitreader *gb)
{
  int v = (gb->data[gb->pos >> 3] >> (gb->pos & 7)) & 1;
  gb->pos++;
  return v;
}

static int get_huff(struct bitreader *gb, const struct tree *tree)
{
  unsigned short e = vlc_tables[tree->vlc_num][peek_bits(gb) & ((1 << VLC_BITS) - 1)];
  gb->pos += e >> 8;
  return tree->syms[e & 0xff];
}

static int get_signed(struct bitreader *gb, int v)
{
  if (v && get_bit(gb)) {
    return -v;
  }
  return v;
}

static void merge(struct bitreader *gb, unsigned char *dst, const unsigned char *src, int size)
{
  const unsigned char *src2 = src + size;
  int size2 = size;

  do {
    if (!get_bit(gb)) {
      *dst++ = *src++;
      size--;
    } else {
      *dst++ = *src2++;
      size2--;
    }
  } while (size && size2);
  while (size--) {
    *dst++ = *src++;
  }
  while (size2--) {
    *dst++ = *src2++;
  }
}

static void read_tree(struct bitreader *gb, struct tree *tree)
{
  unsigned char tmp1[16], tmp2[16];
  unsigned char *in = tmp1, *out = tmp2, *swap;
  int i, t, len;

  tree->vlc_num = (int)get_bits(gb, 4);
  if (!tree->vlc_num) {
    for (i = 0; i < 16; i++) {
      tree->syms[i] = (unsigned char)i;
    }
    return;
  }
  if (get_bit(gb)) {
    for (i = 0; i < 16; i++) {
      tmp1[i] = 0;
    }
    len = (int)get_bits(gb, 3);
    for (i = 0; i <= len; i++) {
      tree->syms[i] = (unsigned char)get_bits(gb, 4);
      tmp1[tree->syms[i]] = 1;
    }
    for (i = 0; i < 16 && len < 15; i++) {
      if (!tmp1[i]) {
        tree->syms[++len] = (unsigned char)i;
      }
    }
  } else {
    len = (int)get_bits(gb, 2);
    for (i = 0; i < 16; i++) {
      in[i] = (unsigned char)i;
    }
    for (i = 0; i <= len; i++) {
      int size = 1 << i;
      for (t = 0; t < 16; t += size << 1) {
        merge(gb, out + t, in + t, size);
      }
      swap = in;
      in = out;
      out = swap;
    }
    for (i = 0; i < 16; i++) {
      tree->syms[i] = in[i];
    }
  }
}

static int log2_floor(unsigned long v)
{
  int n = 0;
  while (v >>= 1) {
    n++;
  }
  return n;
}

/* init_lengths: bit width of each bundle's per-run element count. */
static void bundle_lengths(int *lens, unsigned long width, unsigned long bw)
{
  width = (width + 7) & ~7ul;
  lens[SRC_BLOCK_TYPES] = log2_floor((width >> 3) + 511) + 1;
  lens[SRC_SUB_BLOCK_TYPES] = log2_floor((width >> 4) + 511) + 1;
  lens[SRC_COLORS] = log2_floor(bw * 64 + 511) + 1;
  lens[SRC_INTRA_DC] = lens[SRC_INTER_DC] = lens[SRC_X_OFF] = lens[SRC_Y_OFF] =
    log2_floor((width >> 3) + 511) + 1;
  lens[SRC_PATTERN] = log2_floor((bw << 3) + 511) + 1;
  lens[SRC_RUN] = log2_floor(bw * 48 + 511) + 1;
}

/* 0x236210. Scratch sizes for the nine bundles, sized for the luma plane
 * (the widest); one run of values plus slack for a raw block's 64 bytes. */
void __stdcall FUN_00236210(unsigned long *sizes, unsigned long width)
{
  int lens[BINK_BUNDLE_COUNT];
  int i;

  bundle_lengths(lens, width < 8 ? 8 : width, (width + 7) >> 3);
  for (i = 0; i < BINK_BUNDLE_COUNT; i++) {
    unsigned long count = 1ul << lens[i];
    if (i == SRC_INTRA_DC || i == SRC_INTER_DC) {
      count *= 2;
    }
    sizes[i] = (count + 64 + 15) & ~15ul;
  }
}

static void read_bundle(struct context *c, int n)
{
  struct bundle *b = &c->bundle[n];
  int i;

  if (n == SRC_COLORS) {
    for (i = 0; i < 16; i++) {
      read_tree(&c->gb, &c->col_high[i]);
    }
    c->col_lastval = 0;
  }
  if (n != SRC_INTRA_DC && n != SRC_INTER_DC) {
    read_tree(&c->gb, &b->tree);
  }
  b->cur_dec = b->cur_ptr = b->data;
}

/* CHECK_READ_VAL plus the rewind: returns the element count to decode, 0 when
 * values are still pending or the list has ended. */
static unsigned long begin_read(struct context *c, struct bundle *b, unsigned long elem)
{
  unsigned long t;

  if (!b->cur_dec || b->cur_dec > b->cur_ptr) {
    return 0;
  }
  t = get_bits(&c->gb, b->len);
  if (!t) {
    b->cur_dec = 0;
    return 0;
  }
  b->cur_dec = b->cur_ptr = b->data;
  if (t * elem > (unsigned long)(b->data_end - b->data)) {
    t = (unsigned long)(b->data_end - b->data) / elem;
  }
  return t;
}

static void read_runs(struct context *c, struct bundle *b)
{
  unsigned long t = begin_read(c, b, 1);
  unsigned char *end;

  if (!t) {
    return;
  }
  end = b->cur_dec + t;
  if (get_bit(&c->gb)) {
    csmemset(b->cur_dec, (int)get_bits(&c->gb, 4), t);
    b->cur_dec = end;
  } else {
    while (b->cur_dec < end) {
      *b->cur_dec++ = (unsigned char)get_huff(&c->gb, &b->tree);
    }
  }
}

static void read_motion_values(struct context *c, struct bundle *b)
{
  unsigned long t = begin_read(c, b, 1);
  unsigned char *end;
  int v;

  if (!t) {
    return;
  }
  end = b->cur_dec + t;
  if (get_bit(&c->gb)) {
    v = get_signed(&c->gb, (int)get_bits(&c->gb, 4));
    csmemset(b->cur_dec, v, t);
    b->cur_dec = end;
  } else {
    while (b->cur_dec < end) {
      v = get_signed(&c->gb, get_huff(&c->gb, &b->tree));
      *b->cur_dec++ = (unsigned char)v;
    }
  }
}

static const unsigned char bink_rlelens[4] = { 4, 8, 12, 32 };

static void read_block_types(struct context *c, struct bundle *b)
{
  unsigned long t = begin_read(c, b, 1);
  unsigned char *end;
  int v, last = 0;

  if (!t) {
    return;
  }
  end = b->cur_dec + t;
  if (get_bit(&c->gb)) {
    csmemset(b->cur_dec, (int)get_bits(&c->gb, 4), t);
    b->cur_dec = end;
  } else {
    while (b->cur_dec < end) {
      v = get_huff(&c->gb, &b->tree);
      if (v < 12) {
        last = v;
        *b->cur_dec++ = (unsigned char)v;
      } else {
        unsigned long run = bink_rlelens[v - 12];
        if ((unsigned long)(end - b->cur_dec) < run) {
          run = (unsigned long)(end - b->cur_dec);
        }
        csmemset(b->cur_dec, last, run);
        b->cur_dec += run;
      }
    }
  }
}

static void read_patterns(struct context *c, struct bundle *b)
{
  unsigned long t = begin_read(c, b, 1);
  unsigned char *end;
  int v;

  if (!t) {
    return;
  }
  end = b->cur_dec + t;
  while (b->cur_dec < end) {
    v = get_huff(&c->gb, &b->tree);
    v |= get_huff(&c->gb, &b->tree) << 4;
    *b->cur_dec++ = (unsigned char)v;
  }
}

static int read_color(struct context *c, struct bundle *b)
{
  int v, sign;

  c->col_lastval = get_huff(&c->gb, &c->col_high[c->col_lastval]);
  v = (c->col_lastval << 4) | get_huff(&c->gb, &b->tree);
  if (!c->version_i) {
    sign = ((signed char)v) >> 7;
    v = ((v & 0x7f) ^ sign) - sign;
    v += 0x80;
  }
  return v;
}

static void read_colors(struct context *c, struct bundle *b)
{
  unsigned long t = begin_read(c, b, 1);
  unsigned char *end;

  if (!t) {
    return;
  }
  end = b->cur_dec + t;
  if (get_bit(&c->gb)) {
    csmemset(b->cur_dec, read_color(c, b), t);
    b->cur_dec = end;
  } else {
    while (b->cur_dec < end) {
      *b->cur_dec++ = (unsigned char)read_color(c, b);
    }
  }
}

static void read_dcs(struct context *c, struct bundle *b, int has_sign)
{
  unsigned long len = begin_read(c, b, 2);
  bink_s16 *dst = (bink_s16 *)b->cur_dec;
  unsigned long i, j, len2;
  int v, v2, bsize;

  if (!len) {
    return;
  }
  v = (int)get_bits(&c->gb, DC_START_BITS - has_sign);
  if (has_sign) {
    v = get_signed(&c->gb, v);
  }
  *dst++ = (short)v;
  len--;
  for (i = 0; i < len; i += 8) {
    len2 = len - i < 8 ? len - i : 8;
    bsize = (int)get_bits(&c->gb, 4);
    if (bsize) {
      for (j = 0; j < len2; j++) {
        v2 = get_signed(&c->gb, (int)get_bits(&c->gb, bsize));
        v += v2;
        *dst++ = (short)v;
      }
    } else {
      for (j = 0; j < len2; j++) {
        *dst++ = (short)v;
      }
    }
  }
  b->cur_dec = (unsigned char *)dst;
}

static int get_value(struct context *c, int n)
{
  struct bundle *b = &c->bundle[n];
  int v;

  if (n < SRC_X_OFF || n == SRC_RUN) {
    return *b->cur_ptr++;
  }
  if (n == SRC_X_OFF || n == SRC_Y_OFF) {
    return (signed char)*b->cur_ptr++;
  }
  v = *(const bink_s16 *)b->cur_ptr;
  b->cur_ptr += 2;
  return v;
}

static int read_coef(struct bitreader *gb, int bits)
{
  int t;

  if (!bits) {
    return 1 - (get_bit(gb) << 1);
  }
  t = (int)get_bits(gb, bits) | 1 << bits;
  return get_bit(gb) ? -t : t;
}

/* Returns the quantizer index; coefficients land in block[] at scan order. */
static int read_dct_coeffs(struct bitreader *gb, int block[64], int *coef_count_, int coef_idx[64])
{
  int coef_list[128];
  int mode_list[128];
  int i, bits, ccoef, mode;
  int list_start = 64, list_end = 64, list_pos;
  int coef_count = 0;

  coef_list[list_end] = 4;  mode_list[list_end++] = 0;
  coef_list[list_end] = 24; mode_list[list_end++] = 0;
  coef_list[list_end] = 44; mode_list[list_end++] = 0;
  coef_list[list_end] = 1;  mode_list[list_end++] = 3;
  coef_list[list_end] = 2;  mode_list[list_end++] = 3;
  coef_list[list_end] = 3;  mode_list[list_end++] = 3;

  for (bits = (int)get_bits(gb, 4) - 1; bits >= 0; bits--) {
    list_pos = list_start;
    while (list_pos < list_end) {
      if (!(mode_list[list_pos] | coef_list[list_pos]) || !get_bit(gb)) {
        list_pos++;
        continue;
      }
      ccoef = coef_list[list_pos];
      mode = mode_list[list_pos];
      switch (mode) {
      case 0:
      case 2:
        if (mode == 0) {
          coef_list[list_pos] = ccoef + 4;
          mode_list[list_pos] = 1;
        } else {
          coef_list[list_pos] = 0;
          mode_list[list_pos++] = 0;
        }
        for (i = 0; i < 4; i++, ccoef++) {
          if (get_bit(gb)) {
            if (list_start > 0) {
              coef_list[--list_start] = ccoef;
              mode_list[list_start] = 3;
            }
          } else {
            block[bink_scan[ccoef]] = read_coef(gb, bits);
            coef_idx[coef_count++] = ccoef;
          }
        }
        break;
      case 1:
        mode_list[list_pos] = 2;
        for (i = 0; i < 3 && list_end < 128; i++) {
          ccoef += 4;
          coef_list[list_end] = ccoef;
          mode_list[list_end++] = 2;
        }
        break;
      case 3:
        block[bink_scan[ccoef]] = read_coef(gb, bits);
        coef_idx[coef_count++] = ccoef;
        coef_list[list_pos] = 0;
        mode_list[list_pos++] = 0;
        break;
      }
    }
  }
  *coef_count_ = coef_count;
  return (int)get_bits(gb, 4);
}

static void unquantize(int block[64], const int quant[64], int coef_count, const int coef_idx[64])
{
  int i;

  block[0] = (int)((unsigned)block[0] * (unsigned)quant[0]) >> 11;
  for (i = 0; i < coef_count; i++) {
    int idx = coef_idx[i];
    int pos = bink_scan[idx];
    block[pos] = (int)((unsigned)block[pos] * (unsigned)quant[idx]) >> 11;
  }
}

static void read_residue(struct bitreader *gb, short block[64], int masks_count)
{
  int coef_list[128];
  int mode_list[128];
  int i, mask, ccoef, mode;
  int list_start = 64, list_end = 64, list_pos;
  int nz_coeff[64];
  int nz_coeff_count = 0;

  coef_list[list_end] = 4;  mode_list[list_end++] = 0;
  coef_list[list_end] = 24; mode_list[list_end++] = 0;
  coef_list[list_end] = 44; mode_list[list_end++] = 0;
  coef_list[list_end] = 0;  mode_list[list_end++] = 2;

  for (mask = 1 << get_bits(gb, 3); mask; mask >>= 1) {
    for (i = 0; i < nz_coeff_count; i++) {
      if (!get_bit(gb)) {
        continue;
      }
      if (block[nz_coeff[i]] < 0) {
        block[nz_coeff[i]] = (short)(block[nz_coeff[i]] - mask);
      } else {
        block[nz_coeff[i]] = (short)(block[nz_coeff[i]] + mask);
      }
      if (--masks_count < 0) {
        return;
      }
    }
    list_pos = list_start;
    while (list_pos < list_end) {
      if (!(coef_list[list_pos] | mode_list[list_pos]) || !get_bit(gb)) {
        list_pos++;
        continue;
      }
      ccoef = coef_list[list_pos];
      mode = mode_list[list_pos];
      switch (mode) {
      case 0:
      case 2:
        if (mode == 0) {
          coef_list[list_pos] = ccoef + 4;
          mode_list[list_pos] = 1;
        } else {
          coef_list[list_pos] = 0;
          mode_list[list_pos++] = 0;
        }
        for (i = 0; i < 4; i++, ccoef++) {
          if (get_bit(gb)) {
            if (list_start > 0) {
              coef_list[--list_start] = ccoef;
              mode_list[list_start] = 3;
            }
          } else {
            if (nz_coeff_count < 64) {
              nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
            }
            block[bink_scan[ccoef]] = (short)(get_bit(gb) ? -mask : mask);
            if (--masks_count < 0) {
              return;
            }
          }
        }
        break;
      case 1:
        mode_list[list_pos] = 2;
        for (i = 0; i < 3 && list_end < 128; i++) {
          ccoef += 4;
          coef_list[list_end] = ccoef;
          mode_list[list_end++] = 2;
        }
        break;
      case 3:
        if (nz_coeff_count < 64) {
          nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
        }
        block[bink_scan[ccoef]] = (short)(get_bit(gb) ? -mask : mask);
        coef_list[list_pos] = 0;
        mode_list[list_pos++] = 0;
        if (--masks_count < 0) {
          return;
        }
        break;
      }
    }
  }
}

#define A1 2896
#define A2 2217
#define A3 3784
#define A4 -5352
#define MUL(X, Y) ((int)((unsigned)(X) * (unsigned)(Y)) >> 11)

#define IDCT_TRANSFORM(dest, s0, s1, s2, s3, s4, s5, s6, s7, d0, d1, d2, d3, d4, d5, d6, d7, munge, src) \
  {                                                                                           \
    const int a0 = (src)[s0] + (src)[s4];                                                     \
    const int a1 = (src)[s0] - (src)[s4];                                                     \
    const int a2 = (src)[s2] + (src)[s6];                                                     \
    const int a3 = MUL(A1, (src)[s2] - (src)[s6]);                                            \
    const int a4 = (src)[s5] + (src)[s3];                                                     \
    const int a5 = (src)[s5] - (src)[s3];                                                     \
    const int a6 = (src)[s1] + (src)[s7];                                                     \
    const int a7 = (src)[s1] - (src)[s7];                                                     \
    const int b0 = a4 + a6;                                                                   \
    const int b1 = MUL(A3, a5 + a7);                                                          \
    const int b2 = MUL(A4, a5) - b0 + b1;                                                     \
    const int b3 = MUL(A1, a6 - a4) - b2;                                                     \
    const int b4 = MUL(A2, a7) + b3 - b1;                                                     \
    (dest)[d0] = munge(a0 + a2 + b0);                                                         \
    (dest)[d1] = munge(a1 + a3 - a2 + b2);                                                    \
    (dest)[d2] = munge(a1 - a3 + a2 + b3);                                                    \
    (dest)[d3] = munge(a0 - a2 - b4);                                                         \
    (dest)[d4] = munge(a0 - a2 + b4);                                                         \
    (dest)[d5] = munge(a1 - a3 + a2 - b3);                                                    \
    (dest)[d6] = munge(a1 + a3 - a2 - b2);                                                    \
    (dest)[d7] = munge(a0 + a2 - b0);                                                         \
  }

#define MUNGE_NONE(x) (x)
#define MUNGE_ROW(x) (((x) + 0x7f) >> 8)

static void idct_col(int *dest, const int *src)
{
  if ((src[8] | src[16] | src[24] | src[32] | src[40] | src[48] | src[56]) == 0) {
    dest[0] = dest[8] = dest[16] = dest[24] = dest[32] = dest[40] = dest[48] = dest[56] = src[0];
  } else {
    IDCT_TRANSFORM(dest, 0, 8, 16, 24, 32, 40, 48, 56, 0, 8, 16, 24, 32, 40, 48, 56, MUNGE_NONE, src)
  }
}

/* Leaves the transformed block in block[] (idct_add) */
static void idct(int *block)
{
  int temp[64];
  int i;

  for (i = 0; i < 8; i++) {
    idct_col(&temp[i], &block[i]);
  }
  for (i = 0; i < 8; i++) {
    int *dest = &block[8 * i];
    const int *src = &temp[8 * i];
    IDCT_TRANSFORM(dest, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7, MUNGE_ROW, src)
  }
}

static void idct_put(unsigned char *dst, long stride, int *block)
{
  int i, j;

  idct(block);
  for (i = 0; i < 8; i++, dst += stride, block += 8) {
    for (j = 0; j < 8; j++) {
      dst[j] = (unsigned char)block[j];
    }
  }
}

static void idct_add(unsigned char *dst, long stride, int *block)
{
  int i, j;

  idct(block);
  for (i = 0; i < 8; i++, dst += stride, block += 8) {
    for (j = 0; j < 8; j++) {
      dst[j] = (unsigned char)(dst[j] + block[j]);
    }
  }
}

static void copy8x8(unsigned char *dst, const unsigned char *src, long stride)
{
  int i;

  if (dst == src) {
    return;
  }
  for (i = 0; i < 8; i++, dst += stride, src += stride) {
    *(bink_u32 *)dst = *(const bink_u32 *)src;
    *(bink_u32 *)(dst + 4) = *(const bink_u32 *)(src + 4);
  }
}

struct plane {
  unsigned char *cur;
  const unsigned char *prev;
  long stride;
  long rows;       /* allocated rows (block writes are clipped to these) */
  int mask_shift;  /* 8x8 block index -> 16x16 dirty cell */
};

/* Motion copy; a reference outside the previous plane degrades to a skip. */
static void put_motion(struct context *c, const struct plane *p, unsigned char *dst,
                       long x, long y)
{
  long rx = x + get_value(c, SRC_X_OFF);
  long ry = y + get_value(c, SRC_Y_OFF);

  if (rx < 0 || ry < 0 || rx + 8 > p->stride || ry + 8 > p->rows) {
    rx = x;
    ry = y;
  }
  copy8x8(dst, p->prev + ry * p->stride + rx, p->stride);
}

static void read_run_block(struct context *c, unsigned char *out, const int *coordmap)
{
  const unsigned char *scan = bink_patterns[get_bits(&c->gb, 4)];
  int i = 0, j, v, run;

  do {
    run = get_value(c, SRC_RUN) + 1;
    if (i + run > 64) {
      run = 64 - i;
    }
    i += run;
    if (get_bit(&c->gb)) {
      v = get_value(c, SRC_COLORS);
      for (j = 0; j < run; j++) {
        out[coordmap[*scan++]] = (unsigned char)v;
      }
    } else {
      for (j = 0; j < run; j++) {
        out[coordmap[*scan++]] = (unsigned char)get_value(c, SRC_COLORS);
      }
    }
  } while (i < 63);
  if (i == 63) {
    out[coordmap[*scan]] = (unsigned char)get_value(c, SRC_COLORS);
  }
}

/* Decodes the 8x8 contents of a 16x16 block and writes it scaled 2x,
 * clipped to the plane. */
static void decode_scaled(struct context *c, const struct plane *p, long x, long y,
                          const int *linear_map)
{
  unsigned char ublock[64];
  int dct[64], coef_idx[64];
  int blk, i, j, v, col[2], coef_count, q;
  long w, h, row;
  unsigned char *dst;

  blk = get_value(c, SRC_SUB_BLOCK_TYPES);
  switch (blk) {
  case RUN_BLOCK:
    read_run_block(c, ublock, linear_map);
    break;
  case INTRA_BLOCK:
    csmemset(dct, 0, sizeof(dct));
    dct[0] = get_value(c, SRC_INTRA_DC);
    q = read_dct_coeffs(&c->gb, dct, &coef_count, coef_idx);
    unquantize(dct, bink_intra_quant[q], coef_count, coef_idx);
    idct(dct);
    for (i = 0; i < 64; i++) {
      ublock[i] = (unsigned char)dct[i];
    }
    break;
  case FILL_BLOCK:
    csmemset(ublock, get_value(c, SRC_COLORS), sizeof(ublock));
    break;
  case PATTERN_BLOCK:
    col[0] = get_value(c, SRC_COLORS);
    col[1] = get_value(c, SRC_COLORS);
    for (j = 0; j < 8; j++) {
      v = get_value(c, SRC_PATTERN);
      for (i = 0; i < 8; i++, v >>= 1) {
        ublock[i + j * 8] = (unsigned char)col[v & 1];
      }
    }
    break;
  case RAW_BLOCK:
    for (i = 0; i < 64; i++) {
      ublock[i] = (unsigned char)get_value(c, SRC_COLORS);
    }
    break;
  default:
    return;
  }

  w = p->stride - x < 16 ? p->stride - x : 16;
  h = p->rows - y < 16 ? p->rows - y : 16;
  for (row = 0; row < h; row++) {
    dst = p->cur + (y + row) * p->stride + x;
    for (i = 0; i < w; i++) {
      dst[i] = ublock[(row >> 1) * 8 + (i >> 1)];
    }
  }
}

static void mark_dirty(unsigned char *mask, unsigned long mask_pitch, unsigned long mask_rows,
                       long bx, long by, int shift)
{
  unsigned long mx = (unsigned long)bx >> shift, my = (unsigned long)by >> shift;

  if (mx < mask_pitch && my < mask_rows) {
    mask[my * mask_pitch + mx] = 1;
  }
}

static void decode_plane(struct context *c, const struct plane *p, unsigned long width,
                         long bw, long bh, unsigned char *mask, unsigned long mask_pitch,
                         unsigned long mask_rows)
{
  int lens[BINK_BUNDLE_COUNT];
  int coordmap[64], linear_map[64];
  int dct[64], coef_idx[64];
  short residue[64];
  long bx, by, x, y;
  int i, blk, v, col[2], coef_count, q;
  unsigned char *dst;

  bundle_lengths(lens, width < 8 ? 8 : width, (unsigned long)bw);
  for (i = 0; i < BINK_BUNDLE_COUNT; i++) {
    c->bundle[i].len = lens[i];
    read_bundle(c, i);
  }
  for (i = 0; i < 64; i++) {
    coordmap[i] = (i & 7) + (i >> 3) * (int)p->stride;
    linear_map[i] = i;
  }

  for (by = 0; by < bh; by++) {
    read_block_types(c, &c->bundle[SRC_BLOCK_TYPES]);
    read_block_types(c, &c->bundle[SRC_SUB_BLOCK_TYPES]);
    read_colors(c, &c->bundle[SRC_COLORS]);
    read_patterns(c, &c->bundle[SRC_PATTERN]);
    read_motion_values(c, &c->bundle[SRC_X_OFF]);
    read_motion_values(c, &c->bundle[SRC_Y_OFF]);
    read_dcs(c, &c->bundle[SRC_INTRA_DC], 0);
    read_dcs(c, &c->bundle[SRC_INTER_DC], 1);
    read_runs(c, &c->bundle[SRC_RUN]);

    y = by * 8;
    if (y + 8 > p->rows) {
      /* Unreachable with BinkOpen's 16-aligned planes; stop rather than
       * write past the buffer. */
      return;
    }
    for (bx = 0; bx < bw; bx++) {
      x = bx * 8;
      dst = p->cur + y * p->stride + x;
      blk = get_value(c, SRC_BLOCK_TYPES);
      /* A 16x16 block type on an odd row/column belongs to a block that
       * was already decoded. */
      if ((by & 1 || bx & 1) && blk == SCALED_BLOCK) {
        bx++;
        continue;
      }
      if (blk != SKIP_BLOCK) {
        mark_dirty(mask, mask_pitch, mask_rows, bx, by, p->mask_shift);
      }
      switch (blk) {
      case SKIP_BLOCK:
        copy8x8(dst, p->prev + y * p->stride + x, p->stride);
        break;
      case SCALED_BLOCK:
        decode_scaled(c, p, x, y, linear_map);
        mark_dirty(mask, mask_pitch, mask_rows, bx + 1, by, p->mask_shift);
        mark_dirty(mask, mask_pitch, mask_rows, bx, by + 1, p->mask_shift);
        mark_dirty(mask, mask_pitch, mask_rows, bx + 1, by + 1, p->mask_shift);
        bx++;
        break;
      case MOTION_BLOCK:
        put_motion(c, p, dst, x, y);
        break;
      case RUN_BLOCK:
        read_run_block(c, dst, coordmap);
        break;
      case RESIDUE_BLOCK:
        put_motion(c, p, dst, x, y);
        csmemset(residue, 0, sizeof(residue));
        v = (int)get_bits(&c->gb, 7);
        read_residue(&c->gb, residue, v);
        for (i = 0; i < 64; i++) {
          unsigned char *px = dst + (i >> 3) * p->stride + (i & 7);
          *px = (unsigned char)(*px + residue[i]);
        }
        break;
      case INTRA_BLOCK:
        csmemset(dct, 0, sizeof(dct));
        dct[0] = get_value(c, SRC_INTRA_DC);
        q = read_dct_coeffs(&c->gb, dct, &coef_count, coef_idx);
        unquantize(dct, bink_intra_quant[q], coef_count, coef_idx);
        idct_put(dst, p->stride, dct);
        break;
      case FILL_BLOCK:
        v = get_value(c, SRC_COLORS);
        for (i = 0; i < 8; i++) {
          csmemset(dst + i * p->stride, v, 8);
        }
        break;
      case INTER_BLOCK:
        put_motion(c, p, dst, x, y);
        csmemset(dct, 0, sizeof(dct));
        dct[0] = get_value(c, SRC_INTER_DC);
        q = read_dct_coeffs(&c->gb, dct, &coef_count, coef_idx);
        unquantize(dct, bink_inter_quant[q], coef_count, coef_idx);
        idct_add(dst, p->stride, dct);
        break;
      case PATTERN_BLOCK:
        col[0] = get_value(c, SRC_COLORS);
        col[1] = get_value(c, SRC_COLORS);
        for (i = 0; i < 8; i++) {
          int j;
          v = get_value(c, SRC_PATTERN);
          for (j = 0; j < 8; j++, v >>= 1) {
            dst[i * p->stride + j] = (unsigned char)col[v & 1];
          }
        }
        break;
      case RAW_BLOCK:
        for (i = 0; i < 64; i++) {
          dst[(i >> 3) * p->stride + (i & 7)] = (unsigned char)get_value(c, SRC_COLORS);
        }
        break;
      default:
        /* Corrupt block type: keep the previous contents. */
        copy8x8(dst, p->prev + y * p->stride + x, p->stride);
        break;
      }
    }
  }

  /* next plane starts on a 32-bit boundary */
  c->gb.pos = (c->gb.pos + 31) & ~31ul;
}

static void set_plane(struct plane *p, void *cur, const void *prev, unsigned long stride,
                      unsigned long rows, int mask_shift)
{
  p->cur = (unsigned char *)cur;
  p->prev = (const unsigned char *)prev;
  p->stride = (long)stride;
  p->rows = (long)rows;
  p->mask_shift = mask_shift;
}

/* 0x238070. Decodes one frame's video data into plane_prev (the buffer that
 * becomes current once BinkDoFrame flips field_b8); plane_cur holds the last
 * decoded frame. mask_width/mask_height are the 16-aligned plane sizes. */
void __stdcall FUN_00238070(void *plane_prev, void *plane_cur, void *aplane_prev, void *aplane_cur,
                            unsigned char *mask, unsigned long width, unsigned long height,
                            unsigned long mask_width, unsigned long mask_height,
                            const unsigned char *input, unsigned long keyframe, void *field_3ac,
                            unsigned long open_flags, unsigned long bink_type)
{
  struct context c;
  const unsigned long *scratch = (const unsigned long *)field_3ac;
  unsigned long sizes[BINK_BUNDLE_COUNT];
  unsigned long luma_size = mask_width * mask_height;
  unsigned long chroma_w = mask_width >> 1, chroma_h = mask_height >> 1;
  unsigned long chroma_size = chroma_w * chroma_h;
  unsigned long mask_pitch = mask_width >> 4, mask_rows = mask_height >> 4;
  long luma_bw = (long)((width + 7) >> 3), luma_bh = (long)((height + 7) >> 3);
  long chroma_bw = (long)((width + 15) >> 4), chroma_bh = (long)((height + 15) >> 4);
  struct plane p;
  int i, swap_planes;

  if (!vlc_ready) {
    init_vlcs();
  }
  FUN_00236210(sizes, mask_width);
  for (i = 0; i < BINK_BUNDLE_COUNT; i++) {
    c.bundle[i].data = (unsigned char *)scratch[i];
    c.bundle[i].data_end = c.bundle[i].data + sizes[i] - 64;
  }
  c.version_i = (open_flags & BINK_OPEN_PRE_I) == 0;
  swap_planes = (open_flags & BINK_OPEN_PRE_H) == 0;
  c.gb.data = input;
  c.gb.pos = 0;

  if (bink_type & BINK_TYPE_ALPHA) {
    if (c.version_i) {
      c.gb.pos += 32;
    }
    if (aplane_prev && aplane_cur) {
      set_plane(&p, aplane_prev, aplane_cur, mask_width, mask_height, 1);
      decode_plane(&c, &p, width, luma_bw, luma_bh, mask, mask_pitch, mask_rows);
    } else {
      /* BinkOpen only drops the alpha buffers when the caller did not ask
       * for alpha; the plane still has to be parsed to reach the colour.
       * The luma plane is overwritten next, so it serves as scratch. */
      static unsigned char dummy_mask;
      set_plane(&p, plane_prev, plane_cur, mask_width, mask_height, 1);
      decode_plane(&c, &p, width, luma_bw, luma_bh, &dummy_mask, 0, 0);
    }
  }
  if (c.version_i) {
    c.gb.pos += 32;
  }

  set_plane(&p, plane_prev, plane_cur, mask_width, mask_height, 1);
  decode_plane(&c, &p, width, luma_bw, luma_bh, mask, mask_pitch, mask_rows);
  if (bink_type & BINK_TYPE_GRAY) {
    csmemset((unsigned char *)plane_prev + luma_size, 0x80, chroma_size * 2);
  } else {
    /* Chroma is stored Cb, Cr; revision 'h' and later code Cr first. */
    for (i = 0; i < 2; i++) {
      unsigned long offset = luma_size + (unsigned long)(swap_planes ? i ^ 1 : i) * chroma_size;
      set_plane(&p, (unsigned char *)plane_prev + offset,
                (const unsigned char *)plane_cur + offset, chroma_w, chroma_h, 0);
      decode_plane(&c, &p, width >> 1, chroma_bw, chroma_bh, mask, mask_pitch, mask_rows);
    }
  }

  if (keyframe) {
    csmemset(mask, 1, mask_pitch * mask_rows);
  }
}
