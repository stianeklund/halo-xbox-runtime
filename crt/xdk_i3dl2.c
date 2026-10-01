/* Port of the XDK DirectSound I3DL2 reverb parameter builder that is
 * statically linked into Halo's cachebeta.xbe:
 *
 *   FUN_0020ab80(DSI3DL2LISTENER *listener, state)   stdcall, RET 8
 *
 * CMcpxAPU_Commit3dSettings (0x206a5e) reads 0x118 bytes of the I3DL2 reverb
 * DSP state (GetEffectData, offset 0) into a 0x220-byte stack buffer, calls
 * this function, sets bit 2 of the dword at +0x10, and writes back +0x10 and
 * 0x108 bytes from +0x118.
 *
 * Every helper is a separate stdcall function in retail; each one is named
 * after its VA here. Float arguments are passed as f32 (PUSH ECX; FSTP [ESP])
 * and float results come back in ST0, so they are typed `float` below.
 *
 * Precision: retail keeps intermediates on the x87 stack. Halo runs with the
 * default control word 0x037F (64-bit precision, round to nearest), so
 * intermediates use `long double`, which must be the 80-bit x87 format. On
 * i386-pc-win32 (nxdk) long double is 64-bit by default: compile this file
 * with -mlong-double-80. The public interface does not use long double.
 *
 * No libm. The x87 instructions FRNDINT, FSCALE, F2XM1, FCOS and FSQRT are
 * used through inline asm so that results match retail bit for bit. */

#include "xdk_i3dl2.h"

#pragma STDC FP_CONTRACT OFF

typedef long double x87_t;

/* ---- x87 primitives ------------------------------------------------------ */

static x87_t x87_frndint(x87_t x) { __asm__("frndint" : "+t"(x)); return x; }
static x87_t x87_f2xm1(x87_t x)   { __asm__("f2xm1" : "+t"(x)); return x; }
static x87_t x87_fcos(x87_t x)    { __asm__("fcos" : "+t"(x)); return x; }
static x87_t x87_fsqrt(x87_t x)   { __asm__("fsqrt" : "+t"(x)); return x; }
static x87_t x87_fscale(x87_t x, x87_t n)
{
  __asm__("fscale" : "+t"(x) : "u"(n));
  return x;
}

/* _ftol2 (0x1d9068): truncate toward zero to int64, return the low dword.
 * NaN and |x| >= 2^63 give the integer-indefinite value, whose low dword is 0. */
static int32_t ftol2(x87_t x)
{
  if (x != x || x >= 0x1p63L || x <= -0x1p63L) return 0;
  return (int32_t)(uint32_t)(uint64_t)(int64_t)x;
}

/* FSTP to an f32/f64 slot. The volatile store makes the compiler round here:
 * otherwise LLVM may narrow the multiply to a float op and keep the x87
 * register value unrounded. */
static float to_f32(x87_t x) { volatile float f = (float)x; return f; }
static double to_f64(x87_t x) { volatile double d = (double)x; return d; }

/* FILD of a dword, plus 2^32 (0x25fb8c) when negative: unsigned conversion. */
static x87_t u2x(uint32_t v) { return (x87_t)v; }

/* ---- state access -------------------------------------------------------- */

static uint32_t ld(const uint8_t *s, unsigned off)
{
  return (uint32_t)s[off] | (uint32_t)s[off + 1] << 8 |
         (uint32_t)s[off + 2] << 16 | (uint32_t)s[off + 3] << 24;
}

static void st(uint8_t *s, unsigned off, uint32_t v)
{
  s[off] = (uint8_t)v;
  s[off + 1] = (uint8_t)(v >> 8);
  s[off + 2] = (uint8_t)(v >> 16);
  s[off + 3] = (uint8_t)(v >> 24);
}

/* ---- constants (f32 in the XBE unless noted) ----------------------------- */

#define K_0_01      0x1.47ae14p-7f   /* 0x25bb10 */
#define K_ONE       1.0f             /* 0x2533c8 */
#define K_FIVE      5.0f             /* 0x254cc4 */
#define K_TWO       2.0f             /* 0x253f40 */
#define K_SIXTY     60.0f            /* 0x2b4ee4 */
#define K_48000     48000.0f         /* 0x2c26b0 */
#define K_Q23       8388608.0f       /* 0x20f1a8 */
#define K_MB_TO_DEC 0x1.0624dep-11f  /* 0x2a39e0, 0.0005 (mB -> log10) */
#define K_Q16       65536.0f         /* 0x20f1ac */
#define K_24000     24000.0f         /* 0x2c26dc */
#define K_2_5       2.5f             /* 0x254e04 */
#define K_0_8       0x1.99999ap-1f   /* 0x2533f0 */
#define K_1_OVER_17 0x1.2d2d2cp-1f   /* 0x2c26e0, 1/1.7 */
#define K_0_2       0x1.99999ap-3f   /* 0x2549d4 */
#define K_PI        0x1.921fb6p+1f   /* 0x222620 */
#define K_1_24000   0x1.5d867cp-15f  /* 0x2c26e8 */
#define K_M6000     -6000.0f         /* 0x2c26e4 */
#define K_ONE_D     1.0              /* 0x2573d8, f64 */
#define K_LOG2_10   0x1.a934f0p+1f   /* immediate 0x40549a78 in 0x20a26f */

/* 0x20f1b0: 4 rows of 5 floats (stride 0x14). Column 1 (+4) is not used here. */
static const float tap_gain[4]  = { 0x1.23d70ap-1f, 0x1.0a3d70p-1f,   /* +0x00 */
                                    0x1.147ae2p-1f, 0x1.333334p-1f };
static const float tap_frac[4]  = { 0x1.aee632p-4f, 0x1.13a92ap-4f,   /* +0x08 */
                                    0x1.63bcd4p-3f, 0x1.ec56d6p-3f };
static const float dly_base[4]  = { 0x1.de8b3cp-9f, 0x1.ccb7d4p-9f,   /* +0x0c */
                                    0x1.e16d6ep-9f, 0x1.029ae4p-8f };
static const float dly_scale[4] = { 0x1.f9a6b6p-3f, 0x1.c46738p-3f,   /* +0x10 */
                                    0x1.d94af4p-3f, 0x1.0b0f28p-2f };
/* 0x20f200: signs */
static const float diff_sign[8] = { 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f };
/* 0x20f230: 2 rows of 4 (stride 0x10) */
static const float pair_frac[8] = { 0x1.47de50p-5f, 0x1.271a3cp-2f, 0x1.35208ep-1f, 0x1.b42ce4p-1f,
                                    0x1.5bbbe8p-6f, 0x1.806e1ap-2f, 0x1.3f4646p-1f, 0x1.84b9aap-1f };

/* ---- helpers ------------------------------------------------------------- */

/* 0x20a26f: 10^x via FRNDINT/FSCALE/F2XM1. */
static float pow10_20a26f(float x)
{
  x87_t t = (x87_t)K_LOG2_10 * x;
  x87_t n = x87_frndint(t);
  float nf = to_f32(n);
  x87_t p = x87_fscale(1.0L, n);
  x87_t f = (x87_t)K_LOG2_10 * x - nf;
  return to_f32((x87_f2xm1(f) + 1.0f) * p);
}

/* 0x20a2b1: mB -> linear amplitude, 10^(mB/2000). */
static float mb_to_gain_20a2b1(int32_t mb)
{
  return pow10_20a26f(to_f32((x87_t)mb * K_MB_TO_DEC));
}

/* 0x20a734: one-pole low-pass coefficients. level and hf in mB; freq and
 * rate in Hz. a1 = pole, b0 = (1 - a1) * gain. */
static void onepole_20a734(int32_t level, int32_t hf, float freq, float rate,
                           float *a1, float *b0)
{
  double w = to_f64((x87_t)K_PI / rate * freq * 2);
  double g;
  x87_t h, c, A, B, C, a;

  if (level <= -10000) { *a1 = 0.0f; *b0 = 0.0f; return; }
  if (hf == 0) {
    *a1 = 0.0f;
    *b0 = level == 0 ? 1.0f : mb_to_gain_20a2b1(level);
    return;
  }
  g = mb_to_gain_20a2b1(level);
  h = mb_to_gain_20a2b1((int32_t)((uint32_t)hf * 2u)); /* power gain at freq */
  c = x87_fcos((x87_t)w);
  A = K_ONE_D - c * h;
  B = (1.0L - c) * h * 2;
  C = (K_ONE_D - c * c) * h * h;
  a = (A - x87_fsqrt(B - C)) / (1.0L - h);
  *a1 = to_f32(a);
  *b0 = to_f32((1.0L - a) * g);
}

/* 0x20a6f8: convert the 14 delay-line sizes at 0xac+8k, set 0x1a4 = 4 and
 * clear the first dword of the 10 filter entries at 0x1a8+12k. */
static void prep_20a6f8(uint8_t *s)
{
  int k;
  for (k = 0; k < 14; k++)
    st(s, 0xac + 8 * k, ((ld(s, 0xac + 8 * k) - 1) >> 2) + 1);
  st(s, 0x1a4, 4);
  for (k = 0; k < 10; k++) st(s, 0x1a8 + 12 * k, 0);
}

/* 0x20a8cc: room HF filter (48 kHz) -> 0x1ac/0x1b0 (Q23). */
static void room_hf_20a8cc(uint8_t *s, int32_t room_hf, float hf_ref)
{
  float a1, b0;
  onepole_20a734(0, room_hf, hf_ref, K_48000, &a1, &b0);
  st(s, 0x1ac, (uint32_t)ftol2((x87_t)K_Q23 * a1));
  st(s, 0x1b0, (uint32_t)ftol2((x87_t)K_Q23 * b0));
}

/* 0x20a558: reflections tap gains -> 0x18c..0x198 (Q16). */
static void refl_gain_20a558(uint8_t *s, float gain)
{
  int i;
  for (i = 0; i < 4; i++)
    st(s, 0x18c + 4 * i, (uint32_t)ftol2((x87_t)K_Q16 * gain * tap_gain[i]));
}

/* 0x20a58f: reflections delay in samples -> 0x118, rounded down to 4. */
static void refl_delay_20a58f(uint8_t *s, float delay)
{
  st(s, 0x118, (uint32_t)ftol2((x87_t)delay * K_24000) & ~3u);
}

/* 0x20a5b8: reverb gain -> 0x19c and 0x1a0 (Q16). */
static void reverb_gain_20a5b8(uint8_t *s, float gain)
{
  uint32_t v = (uint32_t)ftol2((x87_t)K_Q16 * gain); /* retail computes it twice */
  st(s, 0x19c, v);
  st(s, 0x1a0, v);
}

/* 0x20a5eb: reverb delay -> 0x12c; 4 taps at fractions of it -> 0x11c..0x128;
 * 4 delays (base + scale * reverb delay) -> 0x150..0x15c. */
static void reverb_delay_20a5eb(uint8_t *s, float delay)
{
  int i;
  st(s, 0x12c, (uint32_t)ftol2((x87_t)delay * K_24000) & ~3u);
  for (i = 0; i < 4; i++)
    st(s, 0x11c + 4 * i, (uint32_t)ftol2(u2x(ld(s, 0x12c)) * tap_frac[i]));
  for (i = 0; i < 4; i++)
    st(s, 0x150 + 4 * i,
       (uint32_t)ftol2(((x87_t)delay * dly_scale[i] + dly_base[i]) * K_24000));
}

/* 0x20a4c1: size factor from the clamped decay value -> 8 delays at
 * 0x130..0x14c and one at 0x160, each rounded down to 4. */
static void size_20a4c1(uint8_t *s, float x)
{
  x87_t f = x < K_2_5 ? ((x87_t)x - K_0_8) * K_1_OVER_17 : 1.0L;
  int k, j;
  for (k = 0; k < 4; k++)
    for (j = 0; j < 2; j++)
      st(s, 0x130 + 4 * (2 * k + j),
         (uint32_t)ftol2(f * pair_frac[k + 4 * j] * K_24000) & ~3u);
  st(s, 0x160, (uint32_t)ftol2(f * K_24000) & ~3u);
}

/* 0x20a924: decay filter for a delay of n samples. out[1] = a1, out[2] = b0
 * (Q23); out[0] is not written. */
static void decay_20a924(uint8_t *s, unsigned off, uint32_t n, float decay,
                         float hf_ratio, float hf_ref)
{
  x87_t x = u2x(n) * K_1_24000 / decay * K_M6000; /* mB lost over n samples */
  int32_t lf = ftol2(x);
  float a1, b0;
  if (hf_ratio < K_ONE) x = x / hf_ratio;
  onepole_20a734(lf, (int32_t)((uint32_t)ftol2(x) - (uint32_t)lf), hf_ref,
                 K_24000, &a1, &b0);
  st(s, off + 4, (uint32_t)ftol2((x87_t)K_Q23 * a1));
  st(s, off + 8, (uint32_t)ftol2((x87_t)K_Q23 * b0));
}

/* 0x20aab2: decay filters. Entries 1..3 (0x1b4) share the filter for
 * delay 0x160/4; entries 8..9 (0x208) the filter for delay 0x160; entries
 * 4..7 (0x1d8) use the delay-line sizes at 0xec+8i. */
static void decay_filters_20aab2(uint8_t *s, float decay, float hf_ratio, float hf_ref)
{
  uint8_t tmp[12];
  int i;
  decay_20a924(tmp, 0, ld(s, 0x160) >> 2, decay, hf_ratio, hf_ref);
  for (i = 0; i < 3; i++) {
    st(s, 0x1b8 + 12 * i, ld(tmp, 4));
    st(s, 0x1bc + 12 * i, ld(tmp, 8));
  }
  decay_20a924(tmp, 0, ld(s, 0x160), decay, hf_ratio, hf_ref);
  for (i = 0; i < 2; i++) {
    st(s, 0x20c + 12 * i, ld(tmp, 4));
    st(s, 0x210 + 12 * i, ld(tmp, 8));
  }
  for (i = 0; i < 4; i++)
    decay_20a924(s, 0x1d8 + 12 * i, ld(s, 0xec + 8 * i), decay, hf_ratio, hf_ref);
}

/* 0x20a684: diffusion -> 8 signed coefficients at 0x164..0x180,
 * 0x188 = 0.2 * diffusion, 0x184 = 1 - diffusion (Q23). */
static void diffusion_20a684(uint8_t *s, float diff)
{
  int i;
  for (i = 0; i < 8; i++)
    st(s, 0x164 + 4 * i, (uint32_t)ftol2((x87_t)K_Q23 * diff_sign[i] * diff));
  st(s, 0x188, (uint32_t)ftol2((x87_t)K_Q23 * diff * K_0_2));
  st(s, 0x184, (uint32_t)ftol2((1.0L - diff) * K_Q23));
}

static uint32_t up4(uint32_t v) { return (v + 1) & ~3u; }

static float f32(int32_t bits)
{
  union { int32_t i; float f; } u;
  u.i = bits;
  return u.f;
}

/* ---- FUN_0020ab80 -------------------------------------------------------- */

void xdk_i3dl2_reverb_parameters(const int32_t listener[12], uint8_t state[0x220])
{
  uint8_t *s = state;
  const int32_t room = listener[0], room_hf = listener[1];
  const float decay = f32(listener[3]), decay_hf = f32(listener[4]);
  const int32_t reflections = listener[5];
  const float refl_delay = f32(listener[6]);
  const int32_t reverb = listener[7];
  const float reverb_delay = f32(listener[8]);
  const float diffusion = f32(listener[9]), density = f32(listener[10]);
  const float hf_ref = f32(listener[11]);
  /* listener[2] (flRoomRolloffFactor) is not read. */

  float diff = to_f32((x87_t)K_0_01 * diffusion);
  x87_t size = (K_TWO - (x87_t)K_0_01 * density) * (((x87_t)decay - K_ONE) * K_FIVE);
  float size_f = to_f32(size);
  uint32_t prev;
  int i, k;

  if (size < K_ONE) { size = 1.0L; size_f = 1.0f; }
  if (size > K_SIXTY) size_f = K_SIXTY;

  prep_20a6f8(s);
  room_hf_20a8cc(s, room_hf, hf_ref);
  refl_gain_20a558(s, mb_to_gain_20a2b1((int32_t)((uint32_t)reflections + (uint32_t)room)));
  refl_delay_20a58f(s, refl_delay);
  reverb_gain_20a5b8(s, mb_to_gain_20a2b1((int32_t)((uint32_t)reverb + (uint32_t)room)));
  reverb_delay_20a5eb(s, reverb_delay);
  size_20a4c1(s, size_f);
  decay_filters_20aab2(s, decay, decay_hf, hf_ref);
  diffusion_20a684(s, diff);

  /* Round up to a multiple of 4, then subtract 1 from the 14 sizes. */
  for (k = 0; k < 14; k++) st(s, 0xac + 8 * k, up4(ld(s, 0xac + 8 * k)));
  for (i = 0; i < 5; i++) st(s, 0x118 + 4 * i, up4(ld(s, 0x118 + 4 * i)));
  for (i = 0; i < 8; i++) st(s, 0x130 + 4 * i, up4(ld(s, 0x130 + 4 * i)));
  st(s, 0x12c, up4(ld(s, 0x12c)));
  for (i = 0; i < 4; i++) st(s, 0x150 + 4 * i, up4(ld(s, 0x150 + 4 * i)));
  st(s, 0x160, up4(ld(s, 0x160)));
  for (k = 0; k < 14; k++) st(s, 0xac + 8 * k, ld(s, 0xac + 8 * k) - 1);

  /* Reverb taps and reverb delay are relative to the reflections delay. */
  for (i = 0; i < 4; i++) st(s, 0x11c + 4 * i, ld(s, 0x11c + 4 * i) + ld(s, 0x118));
  st(s, 0x12c, ld(s, 0x12c) + ld(s, 0x118));

  /* 4 pairs at 0x130: make each pair relative to the previous pair's second
   * value; scale both down when the larger one exceeds size 0xac+8k. */
  prev = 0;
  for (k = 0; k < 4; k++) {
    unsigned o = 0x130 + 8 * k;
    uint32_t a = ld(s, o), b = ld(s, o + 4);
    uint32_t span = (a > b ? a : b) - prev;
    uint32_t lim = ld(s, 0xac + 8 * k);
    if (span > lim) {
      x87_t r = u2x(lim) / u2x(span);
      st(s, o, up4((uint32_t)ftol2(u2x(a - prev) * r)));
      st(s, o + 4, up4((uint32_t)ftol2(u2x(b - prev) * r)));
    } else {
      st(s, o, a - prev);
      st(s, o + 4, b - prev);
    }
    prev = b;
  }

  for (i = 0; i < 4; i++)
    if (ld(s, 0x150 + 4 * i) > ld(s, 0xcc + 8 * i)) st(s, 0x150 + 4 * i, ld(s, 0xcc + 8 * i));
  if (ld(s, 0x160) > ld(s, 0x10c)) st(s, 0x160, ld(s, 0x10c));

  /* Clamp to the DSP's 24-bit signed range. */
  for (i = 0; i < 8; i++) {
    int32_t v = (int32_t)ld(s, 0x164 + 4 * i);
    if (v > 0x7fffff) v = 0x7fffff;
    else if (v < -0x7fffff) v = -0x7fffff;
    st(s, 0x164 + 4 * i, (uint32_t)v);
  }

  /* Delays to DSP byte units. */
  for (i = 0; i < 5; i++) st(s, 0x118 + 4 * i, ld(s, 0x118 + 4 * i) << 2);
  st(s, 0x12c, ld(s, 0x12c) << 2);
  for (i = 0; i < 8; i++) st(s, 0x130 + 4 * i, ld(s, 0x130 + 4 * i) << 2);
  for (i = 0; i < 4; i++) st(s, 0x150 + 4 * i, ld(s, 0x150 + 4 * i) << 2);
  st(s, 0x160, ld(s, 0x160) << 2);
}
