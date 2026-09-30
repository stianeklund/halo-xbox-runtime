/*
 * Standalone compat: Bink YUV -> RGB blitters (RAD's FUN_00234110 and the
 * twelve per-surface copy routines BinkCopyToBuffer dispatches to).
 *
 * The source is compat/bink/video.c's plane buffer: Y (plane_width x
 * plane_height) followed by the Cb and Cr quarter planes, plus an optional
 * alpha plane with the luma stride. Colours are BT.601 limited range, as for
 * every Bink 1 revision before 'k'. Surface types follow PAL binkxbox.c's
 * conv_strs (enum surface).
 * The masked variants copy only the 16x16 cells the decoder marked dirty.
 * Doubling copy modes (flags 0x70000000) are not used by Halo and copy 1:1.
 */
#include "internal.h"

#define FIX_SHIFT 16
#define SURFACE_MASK 0xful

/* BinkCopyToBuffer surface types in the low flag bits. */
enum surface {
  SURFACE_32 = 3,    /* X8R8G8B8, Halo's movie texture */
  SURFACE_32A = 5,   /* A8R8G8B8 */
  SURFACE_4444 = 7,  /* A4R4G4B4 */
  SURFACE_5551 = 8,  /* A1R5G5B5 */
  SURFACE_555 = 9,
  SURFACE_565 = 10,
  SURFACE_655 = 11,
  SURFACE_664 = 12
};

static long y_table[256];
static long cr_r[256], cr_g[256], cb_g[256], cb_b[256];
static int tables_ready;

static void init_tables(void)
{
  long i;

  for (i = 0; i < 256; i++) {
    /* 1.164383, 1.596027, 0.812968, 0.391762, 2.017232 in 16.16 */
    y_table[i] = (i - 16) * 76309 + (1l << (FIX_SHIFT - 1));
    cr_r[i] = (i - 128) * 104597;
    cr_g[i] = (i - 128) * 53279;
    cb_g[i] = (i - 128) * 25675;
    cb_b[i] = (i - 128) * 132201;
  }
  tables_ready = 1;
}

/* 0x234110. RAD loads the converter section for the surface here; the
 * tables are shared by every format. */
void __stdcall FUN_00234110(unsigned long surface_type)
{
  (void)surface_type;
  if (!tables_ready) {
    init_tables();
  }
}

static unsigned int clamp8(long v)
{
  v >>= FIX_SHIFT;
  return v < 0 ? 0u : v > 255 ? 255u : (unsigned int)v;
}

static unsigned long pack(unsigned long surface, unsigned int r, unsigned int g, unsigned int b,
                          unsigned int a)
{
  switch (surface) {
  case SURFACE_32:
    return 0xff000000ul | r << 16 | g << 8 | b;
  case SURFACE_32A:
    return (unsigned long)a << 24 | r << 16 | g << 8 | b;
  case SURFACE_4444:
    return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
  case SURFACE_5551:
    return (a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
  case SURFACE_555:
    return (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
  case SURFACE_655:
    return (r >> 2) << 10 | (g >> 3) << 5 | b >> 3;
  case SURFACE_664:
    return (r >> 2) << 10 | (g >> 2) << 4 | b >> 4;
  case SURFACE_565:
  default:
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
  }
}

struct source {
  const unsigned char *y, *cb, *cr, *a;
  long stride;        /* luma/alpha */
  long chroma_stride;
};

static void set_source(struct source *s, void *y_plane, long plane_width, long plane_height,
                       void *a_plane)
{
  long chroma_size = (plane_width >> 1) * (plane_height >> 1);

  s->y = (const unsigned char *)y_plane;
  s->cb = s->y + plane_width * plane_height;
  s->cr = s->cb + chroma_size;
  s->a = (const unsigned char *)a_plane;
  s->stride = plane_width;
  s->chroma_stride = plane_width >> 1;
}

/* Converts the source rectangle (sx, sy, w, h) to dest + (dx, dy). */
static void blit_rect(unsigned char *dest, long pitch, long dx, long dy, const struct source *s,
                      long sx, long sy, long w, long h, unsigned long surface)
{
  int bytes = (surface == SURFACE_32 || surface == SURFACE_32A) ? 4 : 2;
  long row, col;

  if (!tables_ready) {
    init_tables();
  }
  if (surface == SURFACE_32 && !(sx & 1)) {
    /* Halo's movie texture: pairs of pixels share one chroma sample */
    for (row = 0; row < h; row++) {
      long yy = sy + row;
      const unsigned char *ys = s->y + yy * s->stride + sx;
      const unsigned char *cbs = s->cb + (yy >> 1) * s->chroma_stride + (sx >> 1);
      const unsigned char *crs = s->cr + (yy >> 1) * s->chroma_stride + (sx >> 1);
      bink_u32 *out = (bink_u32 *)(dest + (dy + row) * pitch + dx * 4);
      for (col = 0; col < w; col += 2) {
        long rc = cr_r[*crs], gc = cr_g[*crs] + cb_g[*cbs], bc = cb_b[*cbs];
        long yv = y_table[ys[0]];
        out[0] = 0xff000000u | clamp8(yv + rc) << 16 | clamp8(yv - gc) << 8 | clamp8(yv + bc);
        if (col + 1 < w) {
          yv = y_table[ys[1]];
          out[1] = 0xff000000u | clamp8(yv + rc) << 16 | clamp8(yv - gc) << 8 | clamp8(yv + bc);
        }
        ys += 2;
        cbs++;
        crs++;
        out += 2;
      }
    }
    return;
  }
  for (row = 0; row < h; row++) {
    long yy = sy + row;
    const unsigned char *ys = s->y + yy * s->stride + sx;
    const unsigned char *as = s->a ? s->a + yy * s->stride + sx : 0;
    const unsigned char *cbs = s->cb + (yy >> 1) * s->chroma_stride;
    const unsigned char *crs = s->cr + (yy >> 1) * s->chroma_stride;
    unsigned char *out = dest + (dy + row) * pitch + (dx * bytes);

    for (col = 0; col < w; col++) {
      long cx = (sx + col) >> 1;
      long yv = y_table[ys[col]];
      unsigned int cb = cbs[cx], cr = crs[cx];
      unsigned int r = clamp8(yv + cr_r[cr]);
      unsigned int g = clamp8(yv - cr_g[cr] - cb_g[cb]);
      unsigned int b = clamp8(yv + cb_b[cb]);
      unsigned long px = pack(surface, r, g, b, as ? as[col] : 255u);
      if (bytes == 4) {
        *(bink_u32 *)out = (unsigned int)px;
        out += 4;
      } else {
        *(bink_u16 *)out = (unsigned short)px;
        out += 2;
      }
    }
  }
}

static void blit_full(void *dest, int x, int y, int pitch, void *y_plane, int sx, int sy,
                      unsigned long width, unsigned long height, long plane_width,
                      long plane_height, void *a_plane, unsigned long flags)
{
  struct source s;
  long w = (long)width, h = (long)height;

  if (!dest || !y_plane) {
    return;
  }
  if (sx + w > plane_width) {
    w = plane_width - sx;
  }
  if (sy + h > plane_height) {
    h = plane_height - sy;
  }
  set_source(&s, y_plane, plane_width, plane_height, a_plane);
  blit_rect((unsigned char *)dest, pitch, x, y, &s, sx, sy, w, h, flags & SURFACE_MASK);
}

/* Copies each horizontal run of dirty 16x16 cells. */
static void blit_masked(void *dest, int x, int y, int pitch, const unsigned char *mask,
                        long mask_pitch, void *y_plane, unsigned long width, unsigned long height,
                        long plane_width, long plane_height, void *a_plane, unsigned long flags)
{
  struct source s;
  long cells_x = ((long)width + 15) >> 4, cells_y = ((long)height + 15) >> 4;
  long cy, cx, run;

  if (!dest || !y_plane || !mask) {
    return;
  }
  if (cells_x > mask_pitch) {
    cells_x = mask_pitch;
  }
  set_source(&s, y_plane, plane_width, plane_height, a_plane);
  for (cy = 0; cy < cells_y; cy++) {
    const unsigned char *m = mask + cy * mask_pitch;
    long top = cy * 16, h = (long)height - top < 16 ? (long)height - top : 16;
    for (cx = 0; cx < cells_x; cx = run) {
      long left, w;
      if (!m[cx]) {
        run = cx + 1;
        continue;
      }
      for (run = cx + 1; run < cells_x && m[run]; run++) {
      }
      left = cx * 16;
      w = run * 16 > (long)width ? (long)width - left : (run - cx) * 16;
      blit_rect((unsigned char *)dest, pitch, x + left, y + top, &s, left, top, w, h, flags & SURFACE_MASK);
    }
  }
}

static unsigned long with_surface(unsigned long flags, enum surface surface)
{
  return (flags & ~SURFACE_MASK) | surface;
}

/* 0x244b40: 32-bit X8R8G8B8. */
void __stdcall FUN_00244b40(void *dest, int x, int y, int pitch, void *y_plane, int src_x, int src_y,
                            unsigned long width, unsigned long height, long plane_width,
                            long plane_height, unsigned long flags)
{
  blit_full(dest, x, y, pitch, y_plane, src_x, src_y, width, height, plane_width, plane_height, 0,
            with_surface(flags, SURFACE_32));
}

/* 0x244b90 */
void __stdcall FUN_00244b90(void *dest, int x, int y, int pitch, const unsigned char *mask,
                            long mask_pitch, void *y_plane, unsigned long width, unsigned long height,
                            long plane_width, long plane_height, unsigned long flags)
{
  blit_masked(dest, x, y, pitch, mask, mask_pitch, y_plane, width, height, plane_width,
              plane_height, 0, with_surface(flags, SURFACE_32));
}

/* 0x246030: 32-bit A8R8G8B8 with the alpha plane. */
void __stdcall FUN_00246030(void *dest, int x, int y, int pitch, void *y_plane, int src_x, int src_y,
                            unsigned long width, unsigned long height, long plane_width,
                            long plane_height, void *a_plane, unsigned long flags)
{
  blit_full(dest, x, y, pitch, y_plane, src_x, src_y, width, height, plane_width, plane_height,
            a_plane, with_surface(flags, SURFACE_32A));
}

/* 0x246080 */
void __stdcall FUN_00246080(void *dest, int x, int y, int pitch, const unsigned char *mask,
                            long mask_pitch, void *y_plane, unsigned long width, unsigned long height,
                            long plane_width, long plane_height, void *a_plane,
                            unsigned long flags)
{
  blit_masked(dest, x, y, pitch, mask, mask_pitch, y_plane, width, height, plane_width,
              plane_height, a_plane, with_surface(flags, SURFACE_32A));
}

/* 0x2473f0: 16-bit 555/565/655/664, picked by the surface in flags. */
void __stdcall FUN_002473f0(void *dest, int x, int y, int pitch, void *y_plane, int src_x, int src_y,
                            unsigned long width, unsigned long height, long plane_width,
                            long plane_height, unsigned long flags)
{
  unsigned long surface = flags & SURFACE_MASK;

  if (surface < SURFACE_555 || surface > SURFACE_664) {
    surface = SURFACE_565;
  }
  blit_full(dest, x, y, pitch, y_plane, src_x, src_y, width, height, plane_width, plane_height, 0,
            with_surface(flags, surface));
}

/* 0x247440 */
void __stdcall FUN_00247440(void *dest, int x, int y, int pitch, const unsigned char *mask,
                            long mask_pitch, void *y_plane, unsigned long width, unsigned long height,
                            long plane_width, long plane_height, unsigned long flags)
{
  unsigned long surface = flags & SURFACE_MASK;

  if (surface < SURFACE_555 || surface > SURFACE_664) {
    surface = SURFACE_565;
  }
  blit_masked(dest, x, y, pitch, mask, mask_pitch, y_plane, width, height, plane_width,
              plane_height, 0, with_surface(flags, surface));
}

/* 0x248980: 16-bit A4R4G4B4. */
void __stdcall FUN_00248980(void *dest, int x, int y, int pitch, void *y_plane, int src_x, int src_y,
                            unsigned long width, unsigned long height, long plane_width,
                            long plane_height, void *a_plane, unsigned long flags)
{
  blit_full(dest, x, y, pitch, y_plane, src_x, src_y, width, height, plane_width, plane_height,
            a_plane, with_surface(flags, SURFACE_4444));
}

/* 0x2489d0 */
void __stdcall FUN_002489d0(void *dest, int x, int y, int pitch, const unsigned char *mask,
                            long mask_pitch, void *y_plane, unsigned long width, unsigned long height,
                            long plane_width, long plane_height, void *a_plane,
                            unsigned long flags)
{
  blit_masked(dest, x, y, pitch, mask, mask_pitch, y_plane, width, height, plane_width,
              plane_height, a_plane, with_surface(flags, SURFACE_4444));
}

/* 0x249ab0: 16-bit A1R5G5B5. */
void __stdcall FUN_00249ab0(void *dest, int x, int y, int pitch, void *y_plane, int src_x, int src_y,
                            unsigned long width, unsigned long height, long plane_width,
                            long plane_height, void *a_plane, unsigned long flags)
{
  blit_full(dest, x, y, pitch, y_plane, src_x, src_y, width, height, plane_width, plane_height,
            a_plane, with_surface(flags, SURFACE_5551));
}

/* 0x249b00 */
void __stdcall FUN_00249b00(void *dest, int x, int y, int pitch, const unsigned char *mask,
                            long mask_pitch, void *y_plane, unsigned long width, unsigned long height,
                            long plane_width, long plane_height, void *a_plane,
                            unsigned long flags)
{
  blit_masked(dest, x, y, pitch, mask, mask_pitch, y_plane, width, height, plane_width,
              plane_height, a_plane, with_surface(flags, SURFACE_5551));
}
