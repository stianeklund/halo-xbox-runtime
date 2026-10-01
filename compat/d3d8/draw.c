/*
 * Standalone compat: Direct3D 8 (Xbox) vertex shaders, pixel shaders,
 * textures, vertex streams, the lazy state flush and the draw calls.
 *
 * Same approach as state.c: each routine is transcribed from the XDK
 * code in cachebeta.xbe (VA in the comment above it). The same NV2A methods
 * and values are pushed, and the same mirror, dirty-flag and device-field
 * stores are made. Only the XDK's own push-buffer management (MakeSpace
 * 0x1efe10 / 0x1efe30 and the put pointer at device+0) is replaced by pbkit.
 *
 * The XDK's constant tables live in the legacy .data range, which is
 * zero-filled in the standalone image, so they are copied here as const
 * arrays with their source VA. Its mutable state (the FVF shader object,
 * the stream table, the texture-stage mirror) stays at the legacy address.
 */
#include "internal.h"

int pb_busy(void);

/* Non-incrementing method (every dword goes to the same register). */
#define METHOD_NONINC(method, count) (0x40000000ul | METHOD(method, count))

#define DEVICE_VERTEX_SHADER 0x470
#define DEVICE_VERTEX_SHADER_HANDLE 0x474
#define DEVICE_TRANSFORM_FLAGS 0x4fc
#define DEVICE_VIEW_MATRIX 0x880
#define DEVICE_WORLD_MATRIX 0xa00
#define DEVICE_BLEND_MATRIX_1 0xa40
#define DEVICE_VS_CONSTANT_SHADOW 0xcc  /* 16-byte slot index */
#define DEVICE_VS_PROGRAM_SHADOW 0x18c  /* 16-byte slot index */

/* device+0xc bits. */
#define STATE_FLAG_W_BUFFER 0x2
#define STATE_FLAG_NO_SHADOW 0x10
#define STATE_FLAG_SKIP_VIEWPORT_TRANSFORM 0x200

/* D3D__RenderState entries these routines read. */
#define RS_FOGTABLEMODE_ADDRESS ((unsigned long *)0x1fb7e4)
#define RS_ZENABLE_ADDRESS ((unsigned long *)0x1fb884)
#define RS_MULTISAMPLEANTIALIAS_ADDRESS ((unsigned long *)0x1fb8a8)
#define RS_MULTISAMPLETYPE_ADDRESS ((unsigned long *)0x1fb8b0)
#define MULTISAMPLE_SUPERSAMPLE_BIT 0x1000

/* Vertex shader object (CreateVertexShader 0x1eaf70 / FVF build 0x1eaa20),
 * as dword indices. 16 input slots plus 4 fixed-function texcoord
 * templates, 16 bytes each from +0x28: {stream, offset, type, bytes}. */
#define VS_REFCOUNT 0
#define VS_FLAGS 1
#define VS_SLOT_COUNT 2
#define VS_TEXCOORD_COUNT 3
#define VS_INSTRUCTION_COUNT 4
#define VS_PROGRAM_LENGTH 5
#define VS_TEXCOORD_SIZE 6
#define VS_SLOT(i) (10 + (i) * 4)
#define VS_SLOT_STREAM 0
#define VS_SLOT_OFFSET 1
#define VS_SLOT_TYPE 2
#define VS_SLOT_BYTES 3
#define VS_PROGRAM 0x5a
#define VS_OBJECT_DWORDS 0x5b
#define VS_SLOT_TOTAL 20
#define VS_INPUT_SLOTS 16
#define VS_TEXCOORD_TEMPLATE_SLOT 16
#define VS_SLOT_DISABLED 2 /* NV097 array format type 2 = size 0 */

#define VS_FLAG_CONTEXT_WRITE 0x1
#define VS_FLAG_PASSTHROUGH 0x2
#define VS_FLAG_PROGRAM 0x4
#define VS_FLAG_STATE_SHADER 0x8
#define VS_FLAG_DIFFUSE 0x400
#define VS_FLAG_SPECULAR 0x800
#define VS_FLAG_BACK_DIFFUSE 0x1000
#define VS_FLAG_BACK_SPECULAR 0x2000

/* The XDK's FVF shader object (0x1eb812). */
#define FVF_VERTEX_SHADER ((unsigned long *)0x1fb998)

#define SHADER_TYPE_VERTEX 0x2078
#define SHADER_TYPE_STATE 0x7378

/* Transform execution mode (0x1eb883): program, priv. */
#define TRANSFORM_EXECUTION_MODE_PROGRAM 6
/* Fixed-function execution mode (0x1eb8b6). */
#define TRANSFORM_EXECUTION_MODE_FIXED 4

/* Declaration tokens. */
#define VSD_TOKEN_MASK 0xe0000000ul
#define VSD_TOKEN_NOP 0x00000000ul
#define VSD_TOKEN_STREAM 0x20000000ul
#define VSD_TOKEN_STREAMDATA 0x40000000ul
#define VSD_TOKEN_TESSELLATOR 0x60000000ul
#define VSD_TOKEN_CONSTMEM 0x80000000ul
#define VSD_TOKEN_END 0xe0000000ul
#define VSD_END 0xfffffffful
#define VSD_SKIP 0x10000000ul
#define VSD_SKIP_BYTES 0x08000000ul
#define VSD_REG_TEXCOORD0 9
#define VSD_REG_TEXCOORD3 12
#define VSD_REG_DIFFUSE 3
#define VSD_REG_SPECULAR 4
#define VSD_REG_BACK_DIFFUSE 7
#define VSD_REG_BACK_SPECULAR 8

/* 0x1f9f88: bytes per component (indexed by type >> 4) followed at
 * 0x1f9f90 by the component count (indexed by type & 0xf). Both indices are
 * 4 bits and the original does not bound them, so the 24 bytes up to
 * 0x1f9fa0 are kept as one table. */
static const unsigned char vertex_type_size_table[24] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x03, /* 0x1f9f88 */
  0x01, 0x02, 0x04, 0x00, 0x01, 0x02, 0x04, 0x00, /* 0x1f9f90 */
  0x00, 0x00, 0x08, 0x3f, 0x00, 0x00, 0x00, 0x00, /* 0x1f9f98 (0.53125f) */
};
#define VERTEX_TYPE_COMPONENT_BYTES(type) (vertex_type_size_table[((type) >> 4) & 0xf])
#define VERTEX_TYPE_COMPONENT_COUNT(type) (vertex_type_size_table[8 + ((type) & 0xf)])

/* 0x1fa200: D3DFVF_TEXCOORDSIZE code -> float count. */
static const unsigned long fvf_texcoord_sizes[4] = { 2, 3, 4, 1 };

/* 0x1f9fd0, 0x1fa090, 0x1fa140: the passthrough (D3DFVF_XYZRHW) vertex
 * programs 0x1eb6f0 loads at address 0. */
static const unsigned long passthrough_program_1f9fd0[0x30] = {
  0x00000000, 0x0020001b, 0x0836106c, 0x2f100ff8, 0x00000000, 0x0420061b, 0x083613fc, 0x5011f818,
  0x00000000, 0x0400001b, 0x083613fc, 0x2070f82c, 0x00000000, 0x0240081b, 0x1436186c, 0x2f20f824,
  0x00000000, 0x0060201b, 0x2436106c, 0x3070f800, 0x00000000, 0x00200200, 0x0836106c, 0x2070f830,
  0x00000000, 0x00200e1b, 0x0836106c, 0x2070f838, 0x00000000, 0x0020101b, 0x0836106c, 0x2070f840,
  0x00000000, 0x0020121b, 0x0836106c, 0x2070f848, 0x00000000, 0x0020141b, 0x0836106c, 0x2070f850,
  0x00000000, 0x0020161b, 0x0836106c, 0x2070f858, 0x00000000, 0x0020181b, 0x0836106c, 0x2070f861,
};
static const unsigned long passthrough_program_1fa090[0x2c] = {
  0x00000000, 0x022000aa, 0x0836086c, 0x201ff828, 0x00000000, 0x0020081b, 0x0836106c, 0x2070f820,
  0x00000000, 0x0420061b, 0x083613fc, 0x5011f818, 0x00000000, 0x00200e1b, 0x0836106c, 0x2070f838,
  0x00000000, 0x0240021b, 0x14361800, 0x2f20f834, 0x00000000, 0x0060201b, 0x2436106c, 0x3070f800,
  0x00000000, 0x0020101b, 0x0836106c, 0x2070f840, 0x00000000, 0x0020121b, 0x0836106c, 0x2070f848,
  0x00000000, 0x0020141b, 0x0836106c, 0x2070f850, 0x00000000, 0x0020161b, 0x0836106c, 0x2070f858,
  0x00000000, 0x0020181b, 0x0836106c, 0x2070f861,
};
static const unsigned long passthrough_program_1fa140[0x30] = {
  0x00000000, 0x0020001b, 0x0836106c, 0x2f100ff8, 0x00000000, 0x0420061b, 0x083613fc, 0x5011f818,
  0x00000000, 0x002008ff, 0x0836106c, 0x2070f828, 0x00000000, 0x0240081b, 0x1436186c, 0x2f20f824,
  0x00000000, 0x0060201b, 0x2436106c, 0x3070f800, 0x00000000, 0x00200200, 0x0836106c, 0x2070f830,
  0x00000000, 0x00200e1b, 0x0836106c, 0x2070f838, 0x00000000, 0x0020101b, 0x0836106c, 0x2070f840,
  0x00000000, 0x0020121b, 0x0836106c, 0x2070f848, 0x00000000, 0x0020141b, 0x0836106c, 0x2070f850,
  0x00000000, 0x0020161b, 0x0836106c, 0x2070f858, 0x00000000, 0x0020181b, 0x0836106c, 0x2070f861,
};

/* Float constants the originals read from .rdata. */
#define FLOAT_PIXEL_CENTER 0.53125f     /* 0x1f9e0c, 0x1f9f98 */
#define FLOAT_HALF 0.5f                 /* 0x253398 */
#define FLOAT_MINUS_HALF (-0.5f)        /* 0x255964 */
#define FLOAT_ONE 1.0f                  /* 0x2533c8 */
#define FLOAT_TWO_POW_32 4294967296.0f  /* 0x25fb8c */

/* Push buffer.
 *
 * pbkit never wraps: it only rewinds to its head on pb_reset. The XDK wraps
 * on its own (MakeSpace), so a long frame can outgrow pbkit's buffer. A
 * push reserves the dword count the XDK reserves and rewinds first when the
 * frame would run into the tail; pb_reset waits until the GPU has fetched
 * everything and jumped back, so this only costs a stall. */
#define PUSH_BUFFER_BYTES (4ul * 1024 * 1024)
#define PUSH_REWIND_MARGIN_DWORDS (64ul * 1024 / 4)
#define PUSH_SCRATCH_DWORDS 0x2000
/* The XDK writes at its own put pointer and starts DMA from CDevice_KickOff
 * (0x1ef930): Present, EndVisibilityTest, SetFence, and MakeSpace when the
 * written span passes the threshold. Kick in the same places, with a fixed
 * span standing in for MakeSpace, instead of on every push: each pb_end
 * flushes the write-combine cache and spins on its MMIO status. */
#define PUSH_KICK_DWORDS (8ul * 1024 / 4)

static uint32_t push_scratch[PUSH_SCRATCH_DWORDS];
static uint32_t *push_head;
static uint32_t *push_cursor;
static uint32_t *push_kicked;
static uint32_t *push_limit;
static int push_dropped;

/* Size handed to pb_size before pb_init (device.c). */
unsigned long compat_d3d_push_buffer_bytes(void)
{
  return PUSH_BUFFER_BYTES;
}

/* Hand everything written since the last kick to the GPU. Waits for the GPU
 * and anything that reads what it writes must follow a kick. */
void compat_d3d_push_kick(void)
{
  if (push_cursor != push_kicked) {
    pb_begin();
    pb_end(push_cursor);
    push_kicked = push_cursor;
  }
}

/* Kick, then wait until the GPU has fetched and executed everything. */
void compat_d3d_push_drain(void)
{
  compat_d3d_push_kick();
  while (pb_busy()) {
  }
}

/* pb_reset and remember where the buffer starts again. Called after pb_init
 * and from Present instead of a bare pb_reset.
 *
 * pb_reset gives up waiting for the GPU to reach the head after 100 ticks.
 * A slower GPU (xemu compiling a shader) is then still executing the
 * previous frame from the head while the next frame overwrites it, and it
 * reads a mix of both (unpaired BEGIN_END, stale texture stage state). Drain
 * the GPU first so the jump is always taken before anything is rewritten. */
void compat_d3d_push_rewind(void)
{
  uint32_t *p;

  compat_d3d_push_drain();
  pb_reset();
  p = pb_begin();
  pb_end(p);
  push_head = p;
  push_cursor = p;
  push_kicked = p;
}

/* Not XDK behavior: bring-up diagnostic. pbkit (like the XDK) is not thread
 * safe; report the first pushes from a thread other than the first one. */
unsigned long compat_d3d_foreign_thread_pushes;
static void *push_thread;

static void check_push_thread(unsigned long caller)
{
  void *thread;

  thread = KeGetCurrentThread();
  if (push_thread == 0) {
    push_thread = thread;
  } else if (thread != push_thread && compat_d3d_foreign_thread_pushes++ < 4) {
    DbgPrint("[d3d] push from thread %p (first %p) caller=%08lx\n", thread, push_thread,
             caller);
  }
}

static uint32_t *push_reserve(unsigned long dwords, unsigned long caller)
{
  uint32_t *p;

  check_push_thread(caller);
  if (!push_cursor) {
    push_cursor = pb_begin();
    push_kicked = push_cursor;
  }
  p = push_cursor;
  if (push_head &&
      (unsigned long)(p - push_head) + dwords + PUSH_REWIND_MARGIN_DWORDS >
        PUSH_BUFFER_BYTES / 4) {
    compat_d3d_push_rewind();
    p = push_cursor;
  }
  push_limit = p + dwords;
  push_dropped = 0;
  return p;
}

uint32_t *compat_d3d_push_reserve(unsigned long dwords)
{
  return push_reserve(dwords, (unsigned long)__builtin_return_address(0));
}

uint32_t *compat_d3d_push_begin(unsigned long dwords)
{
  if (gpu_state_ready()) {
    return push_reserve(dwords, (unsigned long)__builtin_return_address(0));
  }
  if (dwords > PUSH_SCRATCH_DWORDS) {
    halo_standalone_missing("d3d8_draw: push larger than the scratch buffer");
  }
  push_limit = push_scratch + dwords;
  push_dropped = 1;
  return push_scratch;
}

void compat_d3d_push_end(uint32_t *end)
{
  if (end > push_limit) {
    halo_standalone_missing("d3d8_draw: push ran past its reservation");
  }
  if (!push_dropped) {
    push_cursor = end;
    if ((unsigned long)(push_cursor - push_kicked) >= PUSH_KICK_DWORDS) {
      compat_d3d_push_kick();
    }
  }
}

static float device_float(unsigned long offset)
{
  dword_float v;

  v.u = *device_field(offset);
  return v.f;
}

static unsigned long float_bits(float f)
{
  dword_float v;

  v.f = f;
  return v.u;
}

static unsigned long *current_vertex_shader(void)
{
  return (unsigned long *)*device_field(DEVICE_VERTEX_SHADER);
}

/* (float) of an unsigned device dword, as the originals do it (fild, then
 * + 2^32 when the sign bit is set). */
static float unsigned_to_float(unsigned long value)
{
  float f;

  f = (float)(long)value;
  if ((long)value < 0) {
    f += FLOAT_TWO_POW_32;
  }
  return f;
}

/* The -0.5 pixel-center shift of 0x1e9e0c / 0x1eb789: super-sampling on
 * (D3DRS_MULTISAMPLETYPE bit 12), multi-sample anti-aliasing on, and the
 * render target is the back buffer. */
static int super_sample_center_shift(void)
{
  return (*RS_MULTISAMPLETYPE_ADDRESS & MULTISAMPLE_SUPERSAMPLE_BIT) &&
         *RS_MULTISAMPLEANTIALIAS_ADDRESS != 0 &&
         *device_field(DEVICE_RENDER_TARGET) ==
           (unsigned long)device_field(DEVICE_BUFFER_SURFACES);
}

/* 0x1e9db0 (ecx = device, edx = push pointer, returns the new pointer):
 * viewport offset/scale and the clip range. */
static uint32_t *push_viewport(uint32_t *p)
{
  const unsigned long *vs;
  float x;
  float y;
  float half_width;
  float half_height;
  float z_scale;

  x = unsigned_to_float(*device_field(DEVICE_VIEWPORT + 0x0)) *
      device_float(DEVICE_SUPER_SAMPLE_SCALE_X) + FLOAT_PIXEL_CENTER;
  y = unsigned_to_float(*device_field(DEVICE_VIEWPORT + 0x4)) *
      device_float(DEVICE_SUPER_SAMPLE_SCALE_Y) + FLOAT_PIXEL_CENTER;
  if (super_sample_center_shift()) {
    x -= FLOAT_HALF;
    y -= FLOAT_HALF;
  }
  z_scale = device_float(DEVICE_Z_SCALE);
  vs = current_vertex_shader();
  if (vs[VS_FLAGS] & (VS_FLAG_PASSTHROUGH | VS_FLAG_PROGRAM)) {
    if (!(*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_SKIP_VIEWPORT_TRANSFORM)) {
      half_width = unsigned_to_float(*device_field(DEVICE_VIEWPORT + 0x8)) *
                   device_float(DEVICE_SUPER_SAMPLE_SCALE_X) * FLOAT_HALF;
      half_height = unsigned_to_float(*device_field(DEVICE_VIEWPORT + 0xc)) *
                    device_float(DEVICE_SUPER_SAMPLE_SCALE_Y) * FLOAT_MINUS_HALF;
      p[0] = METHOD(NV097_SET_VIEWPORT_OFFSET, 4);
      p[1] = float_bits(half_width + x);
      p[2] = float_bits(-half_height + y);
      p[3] = float_bits(device_float(DEVICE_VIEWPORT + 0x10) * z_scale);
      p[4] = 0;
      p[5] = METHOD(NV097_SET_VIEWPORT_SCALE, 4);
      p[6] = float_bits(half_width);
      p[7] = float_bits(half_height);
      p[8] = float_bits((device_float(DEVICE_VIEWPORT + 0x14) -
                         device_float(DEVICE_VIEWPORT + 0x10)) * z_scale);
      p[9] = 0;
      p += 10;
    }
    p[0] = METHOD(NV097_SET_CLIP_MIN, 2);
    p[1] = 0;
    p[2] = float_bits(z_scale);
    return p + 3;
  }
  p[0] = METHOD(NV097_SET_VIEWPORT_OFFSET, 4);
  p[1] = float_bits(x);
  p[2] = float_bits(y);
  p[3] = 0;
  p[4] = 0;
  p[5] = METHOD(NV097_SET_CLIP_MIN, 2);
  p[6] = float_bits(device_float(DEVICE_VIEWPORT + 0x10) * z_scale);
  p[7] = float_bits(device_float(DEVICE_VIEWPORT + 0x14) * z_scale);
  return p + 8;
}

/* The viewport push as its own block, for the setters in state.c that
 * call 0x1e9db0 right after MakeSpace. */
void compat_d3d_push_viewport(void)
{
  uint32_t *p;

  p = compat_d3d_push_begin(16);
  compat_d3d_push_end(push_viewport(p));
}

/* 0x1eac20(dst, src, dwords): NV097_SET_TRANSFORM_PROGRAM in chunks of up
 * to 32 dwords; returns the end. */
static uint32_t *copy_transform_program(uint32_t *dst, const unsigned long *src,
                                        unsigned long dwords)
{
  unsigned long n;
  unsigned long i;

  do {
    n = dwords < 0x20 ? dwords : 0x20;
    dst[0] = METHOD(NV097_SET_TRANSFORM_PROGRAM, n);
    for (i = 0; i < n; i++) {
      dst[1 + i] = src[i];
    }
    src += n;
    dst += n + 1;
    dwords -= n;
  } while (dwords != 0);
  return dst;
}

/* 0x1eb6f0 (ecx = device): for the passthrough (XYZRHW) shader, the
 * viewport constants c0-c1 and the passthrough program at address 0. */
void compat_d3d_push_passthrough_program(void)
{
  const unsigned long *program;
  unsigned long dwords;
  uint32_t *p;
  float center;

  if (!(current_vertex_shader()[VS_FLAGS] & VS_FLAG_PASSTHROUGH)) {
    return;
  }
  if (*RS_FOGTABLEMODE_ADDRESS == 0) {
    program = passthrough_program_1fa140;
    dwords = 0x30;
  } else if (*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_W_BUFFER) {
    program = passthrough_program_1fa090;
    dwords = 0x2c;
  } else {
    program = passthrough_program_1f9fd0;
    dwords = 0x30;
  }
  p = compat_d3d_push_begin(dwords + 0x1e);
  p[0] = METHOD(NV097_SET_TRANSFORM_CONSTANT_LOAD, 1);
  p[1] = 0;
  p[2] = METHOD(NV097_SET_TRANSFORM_CONSTANT, 8);
  p[3] = *device_field(DEVICE_SUPER_SAMPLE_SCALE_X);
  p[4] = *device_field(DEVICE_SUPER_SAMPLE_SCALE_Y);
  p[5] = *device_field(DEVICE_Z_SCALE);
  if (*RS_ZENABLE_ADDRESS == D3DZB_USEW) {
    p[6] = float_bits(device_float(DEVICE_Z_SCALE) * device_float(DEVICE_Z_SCALE_W));
  } else {
    p[6] = float_bits(FLOAT_ONE);
  }
  if (super_sample_center_shift()) {
    center = FLOAT_PIXEL_CENTER - FLOAT_HALF;
    p[7] = float_bits(center);
    p[8] = float_bits(center);
  } else {
    p[7] = float_bits(FLOAT_PIXEL_CENTER);
    p[8] = float_bits(FLOAT_PIXEL_CENTER);
  }
  p[9] = 0;
  p[10] = 0;
  p[11] = METHOD(NV097_SET_TRANSFORM_PROGRAM_LOAD, 1);
  p[12] = 0;
  compat_d3d_push_end(copy_transform_program(p + 13, program, dwords));
}

/* 0x1eaa20(shader, fvf): build the FVF shader object. */
static void build_fvf_vertex_shader(unsigned long *vs, unsigned long fvf)
{
  unsigned long position;
  unsigned long offset;
  unsigned long weights;
  unsigned long count;
  unsigned long sizes;
  unsigned long size;
  unsigned long i;

  for (i = 0; i < VS_OBJECT_DWORDS; i++) {
    vs[i] = 0;
  }
  for (i = 0; i < VS_INPUT_SLOTS; i++) {
    vs[VS_SLOT(i) + VS_SLOT_TYPE] = VS_SLOT_DISABLED;
  }
  position = fvf & 0xe;
  offset = 0;
  if (position == 4) { /* D3DFVF_XYZRHW */
    vs[VS_FLAGS] |= VS_FLAG_PASSTHROUGH;
    vs[VS_SLOT(0) + VS_SLOT_TYPE] = 0x42;
    offset = 0x10;
  } else if (position != 0) {
    vs[VS_SLOT(0) + VS_SLOT_TYPE] = 0x32;
    offset = 0xc;
    if (position >= 6) { /* D3DFVF_XYZB1.. */
      weights = ((position - 6) >> 1) + 1;
      vs[VS_SLOT(1) + VS_SLOT_OFFSET] = 0xc;
      vs[VS_SLOT(1) + VS_SLOT_TYPE] = (weights << 4) | 2;
      offset = weights * 4 + 0xc;
    }
  }
  if (fvf & 0x10) { /* D3DFVF_NORMAL */
    vs[VS_SLOT(2) + VS_SLOT_OFFSET] = offset;
    vs[VS_SLOT(2) + VS_SLOT_TYPE] = 0x32;
    offset += 0xc;
  }
  if (fvf & 0x40) { /* D3DFVF_DIFFUSE */
    vs[VS_FLAGS] |= VS_FLAG_DIFFUSE;
    vs[VS_SLOT(3) + VS_SLOT_OFFSET] = offset;
    vs[VS_SLOT(3) + VS_SLOT_TYPE] = 0x40;
    offset += 4;
  }
  if (fvf & 0x80) { /* D3DFVF_SPECULAR */
    vs[VS_FLAGS] |= VS_FLAG_SPECULAR;
    vs[VS_SLOT(4) + VS_SLOT_OFFSET] = offset;
    vs[VS_SLOT(4) + VS_SLOT_TYPE] = 0x40;
    offset += 4;
  }
  count = (fvf >> 8) & 0xf;
  sizes = fvf >> 16;
  vs[VS_TEXCOORD_COUNT] = count;
  for (i = 0; i < count; i++) {
    size = fvf_texcoord_sizes[sizes & 3];
    vs[VS_SLOT(VS_TEXCOORD_TEMPLATE_SLOT + i) + VS_SLOT_OFFSET] = offset;
    vs[VS_SLOT(VS_TEXCOORD_TEMPLATE_SLOT + i) + VS_SLOT_TYPE] = (size << 4) | 2;
    vs[VS_TEXCOORD_SIZE + i] = size;
    sizes >>= 2;
    offset += size * 4;
  }
}

/* 0x1eacb0(shader, declaration, fixed_function): parse the stream and
 * register tokens into the input slots. */
static void parse_vertex_declaration(unsigned long *vs, const unsigned long *declaration,
                                     int fixed_function)
{
  unsigned long token;
  unsigned long stream;
  unsigned long offset;
  unsigned long reg;
  unsigned long slot;
  unsigned long type;
  unsigned long size;

  stream = 0;
  offset = 0;
  for (;;) {
    token = *declaration++;
    switch (token & VSD_TOKEN_MASK) {
    case VSD_TOKEN_STREAM:
      stream = token & 0xf;
      offset = 0;
      break;
    case VSD_TOKEN_STREAMDATA:
      if (token & VSD_SKIP) {
        size = (token >> 16) & 0xf;
        if (!(token & VSD_SKIP_BYTES)) {
          size <<= 2;
        }
        offset += size;
        break;
      }
      reg = token & 0x1f;
      type = (token >> 16) & 0xff;
      slot = reg;
      if (fixed_function && reg >= VSD_REG_TEXCOORD0 && reg <= VSD_REG_TEXCOORD3) {
        slot = reg + 7;
      }
      vs[VS_SLOT(slot) + VS_SLOT_STREAM] = stream;
      vs[VS_SLOT(slot) + VS_SLOT_OFFSET] = offset;
      ((unsigned char *)&vs[VS_SLOT(slot) + VS_SLOT_BYTES])[0] = 0;
      vs[VS_SLOT(slot) + VS_SLOT_TYPE] = type;
      if (fixed_function) {
        if (reg >= VSD_REG_TEXCOORD0 && reg <= VSD_REG_TEXCOORD3) {
          size = (type >> 4) & 0xf;
          if (size == 7) {
            size = 4;
          } else if (type == 0x16) {
            size = 3;
          }
          vs[VS_TEXCOORD_COUNT]++;
          vs[VS_TEXCOORD_SIZE + reg - VSD_REG_TEXCOORD0] = size;
        }
        if (reg == VSD_REG_DIFFUSE) {
          vs[VS_FLAGS] |= VS_FLAG_DIFFUSE;
        } else if (reg == VSD_REG_SPECULAR) {
          vs[VS_FLAGS] |= VS_FLAG_SPECULAR;
        } else if (reg == VSD_REG_BACK_DIFFUSE) {
          vs[VS_FLAGS] |= VS_FLAG_BACK_DIFFUSE;
        } else if (reg == VSD_REG_BACK_SPECULAR) {
          vs[VS_FLAGS] |= VS_FLAG_BACK_SPECULAR;
        }
      }
      offset += VERTEX_TYPE_COMPONENT_BYTES(type) * VERTEX_TYPE_COMPONENT_COUNT(type);
      break;
    case VSD_TOKEN_TESSELLATOR:
      slot = token & 0x1f;
      vs[VS_SLOT(slot) + VS_SLOT_TYPE] = VS_SLOT_DISABLED;
      vs[VS_SLOT(slot) + VS_SLOT_OFFSET] = 0;
      vs[VS_SLOT(slot) + VS_SLOT_STREAM] = 0;
      ((unsigned char *)&vs[VS_SLOT(slot) + VS_SLOT_BYTES])[1] =
        (unsigned char)((token >> 20) & 0xf);
      ((unsigned char *)&vs[VS_SLOT(slot) + VS_SLOT_BYTES])[0] =
        (unsigned char)((token & VSD_SKIP) ? 2 : 1);
      break;
    case VSD_TOKEN_CONSTMEM:
      declaration += ((token >> 25) & 0xf) * 4;
      break;
    case VSD_TOKEN_END:
      return;
    default:
      break;
    }
  }
}

/* 0x1eae70(declaration, out): the declaration's constant loads as pushes
 * (out may be NULL to count). Returns the dword count. */
static unsigned long emit_declaration_constants(const unsigned long *declaration, uint32_t *out)
{
  unsigned long token;
  unsigned long last;
  unsigned long reg;
  unsigned long count;
  unsigned long n;
  unsigned long dwords;
  unsigned long i;

  dwords = 0;
  last = 0xdeadbeef;
  token = *declaration++;
  while (token != VSD_END) {
    if ((token & VSD_TOKEN_MASK) == VSD_TOKEN_CONSTMEM) {
      count = (token >> 25) & 0xf;
      reg = token & 0xff;
      /* The original compares against the previous token's first register
       * plus one, not its last. */
      if (reg != last + 1) {
        if (out) {
          out[0] = METHOD(NV097_SET_TRANSFORM_CONSTANT_LOAD, 1);
          out[1] = reg;
          out += 2;
        }
        dwords += 2;
      }
      do {
        n = count < 8 ? count : 8;
        if (out) {
          out[0] = METHOD(NV097_SET_TRANSFORM_CONSTANT, n * 4);
          for (i = 0; i < n * 4; i++) {
            out[1 + i] = declaration[i];
          }
          out += n * 4 + 1;
        }
        declaration += n * 4;
        dwords += n * 4 + 1;
        count -= n;
      } while (count != 0);
      last = reg;
    }
    token = *declaration++;
  }
  return dwords;
}

/* 0x1eaf70 */
int __stdcall D3DDevice_CreateVertexShader(unsigned long *declaration, unsigned long *code,
                                           unsigned long *handle, unsigned long flags)
{
  unsigned long type;
  unsigned long instructions;
  unsigned long program_bytes;
  unsigned long bytes;
  unsigned long length;
  unsigned long *vs;
  unsigned long i;

  type = 0;
  instructions = 0;
  if (code) {
    type = ((const unsigned short *)code)[0];
    instructions = ((const unsigned short *)code)[1];
  }
  program_bytes = 0;
  if (code) {
    program_bytes = instructions * 16 + ((instructions * 16 + 0x7f) >> 7) * 4;
  }
  bytes = program_bytes;
  if (declaration) {
    bytes += emit_declaration_constants(declaration, 0) * 4;
  }
  vs = (unsigned long *)compat_malloc(bytes + 0x16c);
  if (!vs) {
    return (int)E_OUTOFMEMORY;
  }
  for (i = 0; i < VS_OBJECT_DWORDS; i++) {
    vs[i] = 0;
  }
  vs[VS_FLAGS] = flags;
  vs[VS_REFCOUNT] = 1;
  for (i = 0; i < VS_SLOT_TOTAL; i++) {
    vs[VS_SLOT(i) + VS_SLOT_TYPE] = VS_SLOT_DISABLED;
  }
  if (declaration) {
    parse_vertex_declaration(vs, declaration, code == 0);
  }
  length = 0;
  if (code) {
    vs[VS_FLAGS] |= VS_FLAG_PROGRAM;
    if (type != SHADER_TYPE_VERTEX) {
      vs[VS_FLAGS] |= VS_FLAG_CONTEXT_WRITE;
    }
    if (type == SHADER_TYPE_STATE) {
      vs[VS_FLAGS] |= VS_FLAG_STATE_SHADER;
    }
    vs[VS_INSTRUCTION_COUNT] = instructions;
    length = (unsigned long)(copy_transform_program((uint32_t *)&vs[VS_PROGRAM], code + 1,
                                                    instructions * 4) -
                             (uint32_t *)&vs[VS_PROGRAM]);
  }
  if (declaration) {
    emit_declaration_constants(declaration, (uint32_t *)&vs[VS_PROGRAM + length]);
  }
  vs[VS_PROGRAM_LENGTH] = bytes >> 2;
  vs[VS_PROGRAM + (bytes >> 2)] = 0;
  *handle = (unsigned long)vs | 1;
  return D3D_OK;
}

/* 0x1eb6d0 */
void __stdcall D3DDevice_DeleteVertexShader(unsigned long handle)
{
  unsigned long *vs;

  vs = (unsigned long *)(handle - 1);
  if (--vs[VS_REFCOUNT] == 0) {
    compat_free(vs);
  }
}

/* 0x1eb540 */
void __stdcall D3DDevice_GetVertexShaderSize(uint32_t handle, uint32_t *size)
{
  if (size) {
    *size = ((const unsigned long *)(handle - 1))[VS_INSTRUCTION_COUNT];
  }
}

/* 0x1eab60(shader, address): keep the device's copy of the program and
 * constants the shader loads. */
static void shadow_vertex_shader(const unsigned long *vs, unsigned long address)
{
  const unsigned long *command;
  unsigned char *program;
  unsigned char *constants;
  unsigned long header;
  unsigned long count;

  program = (unsigned char *)device_field((DEVICE_VS_PROGRAM_SHADOW + address) << 4);
  constants = program;
  command = &vs[VS_PROGRAM];
  do {
    header = *command++;
    count = header >> 18;
    if (count == 1) {
      constants = (unsigned char *)device_field((command[0] + DEVICE_VS_CONSTANT_SHADOW) << 4);
    } else if ((header & 0xffff) == NV097_SET_TRANSFORM_PROGRAM) {
      compat_memcpy(program, command, count * 4);
      program += count * 4;
    } else {
      compat_memcpy(constants, command, count * 4);
      constants += count * 4;
    }
    command += count;
  } while (*command != 0);
}

/* 0x1eb400 */
void __stdcall D3DDevice_LoadVertexShader(uint32_t handle, uint32_t offset)
{
  const unsigned long *vs;
  unsigned long length;
  unsigned long i;
  uint32_t *p;

  vs = (const unsigned long *)(handle - 1);
  if (!(*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_NO_SHADOW)) {
    shadow_vertex_shader(vs, offset);
  }
  length = vs[VS_PROGRAM_LENGTH];
  p = compat_d3d_push_begin(length + 2);
  p[0] = METHOD(NV097_SET_TRANSFORM_PROGRAM_LOAD, 1);
  p[1] = offset;
  for (i = 0; i < length; i++) {
    p[2 + i] = vs[VS_PROGRAM + i];
  }
  compat_d3d_push_end(p + length + 2);
}

/* 0x1eb4d0 */
void __stdcall D3DDevice_SelectVertexShader(uint32_t handle, uint32_t offset)
{
  unsigned long *vs;
  uint32_t *p;

  if (handle) {
    vs = (unsigned long *)(handle - 1);
    *device_field(DEVICE_VERTEX_SHADER) = (unsigned long)vs;
    *device_field(DEVICE_VERTEX_SHADER_HANDLE) = handle;
    *device_field(DEVICE_DIRTY) |= 0x3a0;
    p = compat_d3d_push_begin(32);
    p[0] = METHOD(NV097_SET_TRANSFORM_EXECUTION_MODE, 2);
    p[1] = TRANSFORM_EXECUTION_MODE_PROGRAM;
    p[2] = vs[VS_FLAGS] & VS_FLAG_CONTEXT_WRITE;
    compat_d3d_push_end(push_viewport(p + 3));
  }
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_TRANSFORM_PROGRAM_START, 1);
  p[1] = offset;
  compat_d3d_push_end(p + 2);
}

/* 0x1eb7f0 */
void __stdcall D3DDevice_SetVertexShader(uint32_t shader)
{
  unsigned long *vs;
  unsigned long old_flags;
  uint32_t *p;

  old_flags = current_vertex_shader()[VS_FLAGS];
  if (shader & 1) {
    vs = (unsigned long *)(shader - 1);
  } else {
    vs = FVF_VERTEX_SHADER;
    build_fvf_vertex_shader(vs, shader);
  }
  *device_field(DEVICE_DIRTY) |= 0x3a4;
  if (old_flags != vs[VS_FLAGS]) {
    *device_field(DEVICE_DIRTY) |= 2;
  }
  *device_field(DEVICE_VERTEX_SHADER) = (unsigned long)vs;
  *device_field(DEVICE_VERTEX_SHADER_HANDLE) = shader;
  if (vs[VS_FLAGS] & VS_FLAG_PROGRAM) {
    D3DDevice_LoadVertexShader(shader, 0);
    D3DDevice_SelectVertexShader(shader, 0);
    return;
  }
  p = compat_d3d_push_begin(32);
  if (vs[VS_FLAGS] & VS_FLAG_PASSTHROUGH) {
    p[0] = METHOD(NV097_SET_TRANSFORM_PROGRAM_START, 1);
    p[1] = 0;
    p[2] = METHOD(NV097_SET_TRANSFORM_EXECUTION_MODE, 2);
    p[3] = TRANSFORM_EXECUTION_MODE_PROGRAM;
    p[4] = 0;
    compat_d3d_push_end(push_viewport(p + 5));
    compat_d3d_push_passthrough_program();
    return;
  }
  p[0] = METHOD(NV097_SET_TRANSFORM_EXECUTION_MODE, 1);
  p[1] = TRANSFORM_EXECUTION_MODE_FIXED;
  compat_d3d_push_end(push_viewport(p + 2));
}

/* 0x1eb8d0 */
void __stdcall D3DDevice_SetVertexShaderConstant(int register_index, const void *data,
                                                 uint32_t count)
{
  const unsigned long *src;
  unsigned long reg;
  unsigned long dwords;
  unsigned long n;
  unsigned long i;
  uint32_t *p;

  reg = (unsigned long)register_index + 0x60;
  dwords = count * 4;
  src = (const unsigned long *)data;
  if (!(*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_NO_SHADOW)) {
    compat_memcpy(device_field((reg + DEVICE_VS_CONSTANT_SHADOW) << 4), src, dwords * 4);
  }
  p = compat_d3d_push_begin(dwords + 0x1a);
  p[0] = METHOD(NV097_SET_TRANSFORM_CONSTANT_LOAD, 1);
  p[1] = reg;
  p += 2;
  while ((long)dwords > 0x20) {
    p[0] = METHOD(NV097_SET_TRANSFORM_CONSTANT, 0x20);
    for (i = 0; i < 0x20; i++) {
      p[1 + i] = src[i];
    }
    src += 0x20;
    p += 0x21;
    dwords -= 0x20;
  }
  n = dwords;
  p[0] = METHOD(NV097_SET_TRANSFORM_CONSTANT, n);
  for (i = 0; i < n; i++) {
    p[1 + i] = src[i];
  }
  compat_d3d_push_end(p + n + 1);
}

/* 0x1f3f80(depth, &device+0x4f8), called by SetRenderTarget (0x1e7c15)
 * for a non-NULL depth buffer: the depth range of its format. Other formats
 * leave the scale unchanged. */
void compat_d3d_set_depth_scale(const void *depth_stencil)
{
  unsigned long format;

  if (!depth_stencil) {
    return;
  }
  format = ((const unsigned char *)depth_stencil)[0xd];
  switch (format) {
  case 0x2a: /* D3DFMT_D24S8 */
  case 0x2e: /* D3DFMT_LIN_D24S8 */
    *device_field(DEVICE_Z_SCALE) = 0x4b7fffff; /* 16777215.0f */
    break;
  case 0x2b: /* D3DFMT_F24S8 */
  case 0x2f: /* D3DFMT_LIN_F24S8 */
    *device_field(DEVICE_Z_SCALE) = 0x7149f2ca; /* 1.0e30f */
    break;
  case 0x2c: /* D3DFMT_D16 */
  case 0x30: /* D3DFMT_LIN_D16 */
    *device_field(DEVICE_Z_SCALE) = 0x477fff00; /* 65535.0f */
    break;
  case 0x2d: /* D3DFMT_F16 */
  case 0x31: /* D3DFMT_LIN_F16 */
    *device_field(DEVICE_Z_SCALE) = 0x43fff800; /* 511.9375f */
    break;
  default:
    break;
  }
}

/* ------------------------------------------------------------------------
 * Resources, streams and indices.
 */

/* Resource header: Common, Data, Lock, Format, Size; a surface's parent at
 * +0x14. */
#define RESOURCE_COMMON 0
#define RESOURCE_DATA 1
#define RESOURCE_LOCK 2
#define RESOURCE_FORMAT 3
#define RESOURCE_SIZE 4
#define RESOURCE_PARENT 5
#define D3DCOMMON_INTREFCOUNT_MASK 0x00780000ul
#define D3DCOMMON_INTREFCOUNT_1 0x00080000ul
#define D3DCOMMON_REFCOUNT_MASK 0x0000fffful

void __stdcall D3D_DestroyResource(void *resource);

/* 0x1ed8b0: take a device reference; a surface without one also references
 * its parent. */
static void resource_add_device_ref(unsigned long *resource)
{
  if ((resource[RESOURCE_COMMON] & D3DCOMMON_TYPE_MASK) == D3DCOMMON_TYPE_SURFACE &&
      !(resource[RESOURCE_COMMON] & D3DCOMMON_INTREFCOUNT_MASK) &&
      resource[RESOURCE_PARENT]) {
    resource_add_device_ref((unsigned long *)resource[RESOURCE_PARENT]);
  }
  resource[RESOURCE_COMMON] += D3DCOMMON_INTREFCOUNT_1;
}

/* The release inlined into the setters (0x1eb2fb, 0x1e8954, 0x1e8736):
 * stamp the resource with the current time, drop the device reference, and
 * destroy it when no reference is left. */
static void resource_release_device_ref(unsigned long *resource, int stamp)
{
  if (stamp) {
    resource[RESOURCE_LOCK] = *device_field(DEVICE_CURRENT_TIME);
  }
  resource[RESOURCE_COMMON] -= D3DCOMMON_INTREFCOUNT_1;
  if (!(resource[RESOURCE_COMMON] & (D3DCOMMON_INTREFCOUNT_MASK | D3DCOMMON_REFCOUNT_MASK))) {
    D3D_DestroyResource(resource);
  }
}

/* The XDK's vertex stream table: 16 x {stride, offset, vertex buffer}. */
#define STREAM_TABLE ((unsigned long *)0x1fb8d8)
#define STREAM_STRIDE(s) STREAM_TABLE[(s) * 3 + 0]
#define STREAM_OFFSET(s) STREAM_TABLE[(s) * 3 + 1]
#define STREAM_BUFFER(s) STREAM_TABLE[(s) * 3 + 2]
/* Index buffer data (0x1e8939). */
#define INDEX_DATA_ADDRESS ((unsigned long *)0x1fb494)

#define DEVICE_BASE_VERTEX_INDEX 0x478
#define DEVICE_INDEX_BUFFER 0x47c
#define DEVICE_LAST_BASE_VERTEX_INDEX 0x598

/* 0x1eb2d0 */
void __stdcall D3DDevice_SetStreamSource(uint32_t stream, void *vertex_buffer, uint32_t stride)
{
  unsigned long *old;

  if (vertex_buffer) {
    resource_add_device_ref((unsigned long *)vertex_buffer);
  }
  old = (unsigned long *)STREAM_BUFFER(stream);
  if (old) {
    resource_release_device_ref(old, 1);
  }
  if (stride == STREAM_STRIDE(stream)) {
    *device_field(DEVICE_DIRTY) |= 0x200;
  } else {
    *device_field(DEVICE_DIRTY) |= 0x280;
  }
  STREAM_BUFFER(stream) = (unsigned long)vertex_buffer;
  STREAM_STRIDE(stream) = stride;
}

/* 0x1e8920. The index buffer's release does not stamp it. */
void __stdcall D3DDevice_SetIndices(void *index_data, uint32_t base_vertex_index)
{
  unsigned long *old;

  if (index_data) {
    resource_add_device_ref((unsigned long *)index_data);
    *INDEX_DATA_ADDRESS = ((unsigned long *)index_data)[RESOURCE_DATA];
  } else {
    *INDEX_DATA_ADDRESS = 0;
  }
  old = (unsigned long *)*device_field(DEVICE_INDEX_BUFFER);
  if (old) {
    resource_release_device_ref(old, 0);
  }
  *device_field(DEVICE_INDEX_BUFFER) = (unsigned long)index_data;
  *device_field(DEVICE_BASE_VERTEX_INDEX) = base_vertex_index;
}

/* ------------------------------------------------------------------------
 * Lazy state (0x1f1b50) and its dirty-bit handlers.
 */

#define DIRTY_POINT_PARAMS 0x1
#define DIRTY_COMBINERS 0x8
#define DIRTY_TEXTURE_CONTROL 0x10
#define DIRTY_FOG 0x40
#define DIRTY_VERTEX_FORMAT 0x80
#define DIRTY_VERTEX_OFFSETS 0x200
#define DIRTY_TEXTURE_MODES 0x400
#define DIRTY_SKIP_VERTEX_STATE 0x40000000ul

/* Deferred D3DRS_* indices (mirror dword index). */
#define D3DRS_FOGENABLE 0x52
#define D3DRS_FOGTABLEMODE 0x53
#define D3DRS_FOGSTART 0x54
#define D3DRS_FOGEND 0x55
#define D3DRS_FOGDENSITY 0x56
#define D3DRS_RANGEFOGENABLE 0x57
#define D3DRS_WRAP0 0x58
#define D3DRS_LIGHTING 0x5c
#define D3DRS_SPECULARENABLE 0x5d
#define D3DRS_POINTSIZE 0x6a
#define D3DRS_POINTSIZE_MIN 0x6b
#define D3DRS_POINTSPRITEENABLE 0x6c
#define D3DRS_POINTSCALEENABLE 0x6d
#define D3DRS_POINTSCALE_A 0x6e
#define D3DRS_POINTSCALE_B 0x6f
#define D3DRS_POINTSCALE_C 0x70
#define D3DRS_POINTSIZE_MAX 0x71

/* D3DTSS_* indices (mirror dword index within a stage). */
#define D3DTSS_COLOROP 0x00
#define D3DTSS_COLORARG1 0x02
#define D3DTSS_COLORARG2 0x03
#define D3DTSS_ALPHAOP 0x04
#define D3DTSS_ALPHAARG1 0x06
#define D3DTSS_ALPHAARG2 0x07
#define D3DTOP_DISABLE 1
#define D3DTOP_SELECTARG1 2
#define D3DTOP_SELECTARG2 3
#define D3DTOP_MODULATE 4
#define D3DTA_CURRENT 0x01
#define D3DTA_TEXTURE 0x02
#define D3DTA_TFACTOR 0x03
#define D3DTA_SPECULAR 0x04
#define D3DTA_TEMP 0x05
#define D3DTA_COMPLEMENT 0x10
#define D3DTA_ALPHAREPLICATE 0x20
#define D3DTSS_ADDRESSU 0x0a
#define D3DTSS_ADDRESSV 0x0b
#define D3DTSS_ADDRESSW 0x0c
#define D3DTSS_MAGFILTER 0x0d
#define D3DTSS_MINFILTER 0x0e
#define D3DTSS_MIPFILTER 0x0f
#define D3DTSS_MIPMAPLODBIAS 0x10
#define D3DTSS_MAXMIPLEVEL 0x11
#define D3DTSS_MAXANISOTROPY 0x12
#define D3DTSS_COLORKEYOP 0x13
#define D3DTSS_COLORSIGN 0x14
#define D3DTSS_ALPHAKILL 0x15
#define D3DTOP_BUMPENVMAP 0x19
#define D3DTOP_BUMPENVMAPLUMINANCE 0x1a

#define TEXTURE_STAGE_COUNT 4
#define DEVICE_TEXTURES 0xba0
#define DEVICE_PALETTES 0xbb0
#define DEVICE_TEXTURE_CONTROL0 0x4cc
#define DEVICE_TEXTURE_FORMAT_BITS 0x4dc
#define DEVICE_LOD_BIAS 0x50c
#define DEVICE_PS_COMBINER_COUNT 0x418
#define DEVICE_FF_COMBINER_COUNT 0x594
#define DEVICE_PS_DOTMAP_TEXTURE_MODES 0x41c
#define STATE_FLAG_FORCE_SPECULAR 0x40

#define TEXTURE_STATE(stage, state) D3D_TEXTURE_STATE[(stage) * TEXTURE_STATE_STRIDE + (state)]

/* ------------------------------------------------------------------------
 * Debug knob HALO_STANDALONE_TRACE_TEXTURE_STAGES (compile-time, off by
 * default; not XDK code). It records the last SHADER_STAGE_PROGRAM,
 * TEXTURE_FORMAT and TEXTURE_CONTROL0 values pushed for each stage, and
 * before each draw logs them with the bound texture. The log goes to
 * DbgPrint and to COM1 (0x3f8), because xemu aborts the process on a
 * pixel-shader assert before XBDM can read memory back.
 * xemu's psh.c get_sampler_type asserts dim == 2 for stage modes
 * CUBEMAP (3), DOT_RFLCT_DIFF (0xb), DOT_RFLCT_SPEC (0xc), DOT_STR_CUBE
 * (0xe), DPNDNT_AR (0xf) and DPNDNT_GB (0x10). dim is read only for a
 * stage whose TEXTURE_CONTROL0 ENABLE bit is set, else it stays 0. A draw that
 * would trip that assert is tagged "DIM!" (first TRACE_BAD_LIMIT logged) and
 * redrawn with the offending stages set to PROGRAM_NONE, so xemu survives
 * and XBDM can read the log. Other draws are logged for the first
 * TRACE_DRAW_LIMIT draws only.
 */
#ifdef HALO_STANDALONE_TRACE_TEXTURE_STAGES
#define TRACE_DRAW_LIMIT 2000
#define TRACE_BAD_LIMIT 16
#define TRACE_COM1 ((PUCHAR)0x3f8)
static unsigned long trace_stage_program;
static unsigned long trace_format[TEXTURE_STAGE_COUNT];
static unsigned long trace_control0[TEXTURE_STAGE_COUNT];
static unsigned long trace_draw_count;
static unsigned long trace_bad_count;

static char *trace_hex(char *out, unsigned long value)
{
  int shift;

  for (shift = 28; shift >= 0; shift -= 4) {
    *out++ = "0123456789abcdef"[(value >> shift) & 0xf];
  }
  return out;
}

static char *trace_text(char *out, const char *text)
{
  while (*text) {
    *out++ = *text++;
  }
  return out;
}

/* Give XBDM time to send the line before the GPU runs the draw: when
 * xemu dies on a GPU fault, anything still queued is lost. */
static void trace_flush(void)
{
  LARGE_INTEGER interval;

  interval.QuadPart = -20 * 10000; /* 20 ms, relative */
  KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static void trace_texture_stages(const char *where)
{
  char line[512];
  char *out;
  unsigned long stage;
  unsigned long mode;
  unsigned long dim;
  unsigned long safe;
  uint32_t *p;
  int bad;

  bad = 0;
  safe = trace_stage_program;
  for (stage = 0; stage < TEXTURE_STAGE_COUNT; stage++) {
    mode = (trace_stage_program >> (stage * 5)) & 0x1f;
    dim = (trace_control0[stage] & 0x40000000ul) ? (trace_format[stage] >> 4) & 0xf : 0;
    if ((mode == 3 || mode == 0xb || mode == 0xc || mode == 0xe || mode == 0xf ||
         mode == 0x10) &&
        dim != 2) {
      bad = 1;
      safe &= ~(0x1ful << (stage * 5));
    }
  }
  trace_draw_count++;
  if (bad) {
    /* Not XDK behavior: keep xemu alive so the log reaches XBDM. The
     * offending stages draw as PROGRAM_NONE; the recorded value is kept so
     * the next draw is still checked against what the XDK code pushed. */
    p = compat_d3d_push_begin(2);
    p[0] = METHOD(NV097_SET_SHADER_STAGE_PROGRAM, 1);
    p[1] = safe;
    compat_d3d_push_end(p + 2);
    if (++trace_bad_count > TRACE_BAD_LIMIT) {
      return;
    }
  } else if (trace_draw_count > TRACE_DRAW_LIMIT) {
    return;
  }
  out = line;
  out = trace_text(out, bad ? "[d3d-trace] DIM! " : "[d3d-trace] ");
  out = trace_text(out, where);
  out = trace_text(out, " n=");
  out = trace_hex(out, trace_draw_count);
  out = trace_text(out, " prog=");
  out = trace_hex(out, trace_stage_program);
  for (stage = 0; stage < TEXTURE_STAGE_COUNT; stage++) {
    unsigned long *t;

    t = (unsigned long *)*(unsigned long *)(0x1fbb10 + DEVICE_TEXTURES + stage * 4);
    if (!(trace_control0[stage] & 0x40000000ul) && !t) {
      continue;
    }
    out = trace_text(out, " s");
    *out++ = (char)('0' + stage);
    out = trace_text(out, ":c=");
    out = trace_hex(out, trace_control0[stage]);
    out = trace_text(out, ",f=");
    out = trace_hex(out, trace_format[stage]);
    if (t) {
      out = trace_text(out, ",d=");
      out = trace_hex(out, t[RESOURCE_DATA]);
      out = trace_text(out, ",z=");
      out = trace_hex(out, t[RESOURCE_SIZE]);
    }
  }
  out = trace_text(out, "\n");
  *out = 0;
  DbgPrint("%s", line);
  WRITE_PORT_BUFFER_UCHAR(TRACE_COM1, (PUCHAR)line, (ULONG)(out - line));
  trace_flush();
}

/* Coarse progress markers from device.c (Present, Clear, init). */
static unsigned long trace_mark_count;

void compat_d3d_trace_mark(const char *what)
{
  char line[96];
  char *out;

  if (++trace_mark_count > 100) {
    return;
  }
  out = line;
  out = trace_text(out, "[d3d-trace] mark ");
  out = trace_text(out, what);
  out = trace_text(out, " n=");
  out = trace_hex(out, trace_mark_count);
  out = trace_text(out, " draws=");
  out = trace_hex(out, trace_draw_count);
  out = trace_text(out, "\n");
  *out = 0;
  DbgPrint("%s", line);
  WRITE_PORT_BUFFER_UCHAR(TRACE_COM1, (PUCHAR)line, (ULONG)(out - line));
  trace_flush();
}

/* Not XDK behavior: a texture whose header cannot describe a real NV2A
 * texture (Data outside the 128 MB of physical RAM, dimensionality other
 * than 2 or 3, a log2 size above 12) is logged as "BADTEX" and bound as
 * NULL, so xemu neither uploads garbage nor asserts on it. */
static unsigned long trace_bad_textures;

static void *trace_check_texture(uint32_t stage, void *texture)
{
  char line[160];
  char *out;
  unsigned long *t;
  unsigned long format;
  unsigned long dim;

  t = (unsigned long *)texture;
  if (!t) {
    return texture;
  }
  format = t[RESOURCE_FORMAT];
  dim = (format >> 4) & 0xf;
  if (t[RESOURCE_DATA] != 0 && t[RESOURCE_DATA] < 0x8000000ul &&
      (dim == 2 || dim == 3) && ((format >> 20) & 0xf) <= 12 &&
      ((format >> 24) & 0xf) <= 12 && ((format >> 28) & 0xf) <= 12) {
    return texture;
  }
  if (++trace_bad_textures <= TRACE_BAD_LIMIT) {
    out = line;
    out = trace_text(out, "[d3d-trace] BADTEX stage ");
    *out++ = (char)('0' + stage);
    out = trace_text(out, " tex=");
    out = trace_hex(out, (unsigned long)t);
    out = trace_text(out, " common=");
    out = trace_hex(out, t[0]);
    out = trace_text(out, " data=");
    out = trace_hex(out, t[RESOURCE_DATA]);
    out = trace_text(out, " fmt=");
    out = trace_hex(out, format);
    out = trace_text(out, " size=");
    out = trace_hex(out, t[RESOURCE_SIZE]);
    out = trace_text(out, " ret=");
    out = trace_hex(out, (unsigned long)__builtin_return_address(0));
    out = trace_text(out, "\n");
    *out = 0;
    DbgPrint("%s", line);
    WRITE_PORT_BUFFER_UCHAR(TRACE_COM1, (PUCHAR)line, (ULONG)(out - line));
    trace_flush();
  }
  return 0;
}
#define TRACE_CHECK_TEXTURE(stage, texture) ((texture) = trace_check_texture((stage), (texture)))
#define TRACE_STAGE_PROGRAM(value) (trace_stage_program = (value))
#define TRACE_FORMAT(stage, value) (trace_format[stage] = (value))
#define TRACE_CONTROL0(stage, value) (trace_control0[stage] = (value))
#define TRACE_DRAW(where) trace_texture_stages(where)
#else
#define TRACE_STAGE_PROGRAM(value) ((void)0)
#define TRACE_FORMAT(stage, value) ((void)0)
#define TRACE_CONTROL0(stage, value) ((void)0)
#define TRACE_DRAW(where) ((void)0)
#define TRACE_CHECK_TEXTURE(stage, texture) ((void)0)
void compat_d3d_trace_mark(const char *what)
{
  (void)what;
}
#endif

static unsigned long render_state_float_bits(unsigned long index)
{
  return D3D_RENDER_STATE[index];
}

static float render_state_float(unsigned long index)
{
  dword_float v;

  v.u = D3D_RENDER_STATE[index];
  return v.f;
}

static unsigned long *device_texture(unsigned long stage)
{
  return (unsigned long *)*device_field(DEVICE_TEXTURES + stage * 4);
}

#define FLOAT_POINT_SIZE_LIMIT 64.0f  /* 0x1fa744 */
#define FLOAT_POINT_SIZE_CLAMP 64.0f  /* 0x254df8 */
#define FLOAT_POINT_SIZE_UNITS 8.0f   /* 0x253f78 */
#define POINT_SIZE_MAX 0x1ff

/* 0x1f05e0: point size and point-scale parameters. */
static void lazy_point_params(void)
{
  float size;
  float minimum;
  float maximum;
  float range;
  float t;
  unsigned long scale_enable;
  unsigned long point_size;
  uint32_t *p;

  *device_field(DEVICE_DIRTY) &= ~(unsigned long)DIRTY_POINT_PARAMS;
  p = compat_d3d_push_begin(0x20);
  maximum = render_state_float(D3DRS_POINTSIZE_MAX);
  size = render_state_float(D3DRS_POINTSIZE);
  minimum = render_state_float(D3DRS_POINTSIZE_MIN);
  scale_enable = D3D_RENDER_STATE[D3DRS_POINTSCALEENABLE];
  if (!scale_enable) {
    size *= device_float(DEVICE_SUPER_SAMPLE_SCALE);
    if (size < minimum) {
      size = minimum;
    }
    if (size > maximum) {
      size = maximum;
    }
    if (size > FLOAT_POINT_SIZE_LIMIT) {
      size = FLOAT_POINT_SIZE_CLAMP;
    }
  } else {
    range = maximum - minimum;
    t = range / (unsigned_to_float(*device_field(DEVICE_VIEWPORT + 0xc)) * size);
    p[0] = METHOD(NV097_SET_POINT_PARAMS, 8);
    p[4] = float_bits(range);
    p[5] = float_bits(range);
    p[6] = float_bits(range);
    p[8] = float_bits(minimum);
    t = t * t;
    p[1] = float_bits(render_state_float(D3DRS_POINTSCALE_A) * t);
    p[2] = float_bits(render_state_float(D3DRS_POINTSCALE_B) * t);
    p[3] = float_bits(render_state_float(D3DRS_POINTSCALE_C) * t);
    p[7] = float_bits(-(minimum / range));
    p += 9;
  }
  p[0] = METHOD(NV097_SET_POINT_PARAMS_ENABLE, 2);
  p[1] = scale_enable;
  p[2] = D3D_RENDER_STATE[D3DRS_POINTSPRITEENABLE];
  /* 0x1f3c30 is cvttss2si. */
  point_size = (unsigned long)(long)(size * FLOAT_POINT_SIZE_UNITS + FLOAT_HALF);
  if (point_size > POINT_SIZE_MAX) {
    point_size = POINT_SIZE_MAX;
  }
  p[3] = METHOD(NV097_SET_POINT_SIZE, 1);
  p[4] = point_size;
  compat_d3d_push_end(p + 5);
}

/* NV097_SET_SHADER_STAGE_PROGRAM stage modes (0x1f0750). */
#define STAGE_MODE_NONE 0
#define STAGE_MODE_2D 1
#define STAGE_MODE_3D 2
#define STAGE_MODE_CUBE 3
#define STAGE_MODE_BUMPENVMAP 6
#define STAGE_MODE_BUMPENVMAP_LUMINANCE 7
#define STAGE_MODE_DOT_STR_3D 0xd
#define TEXTURE_FORMAT_CUBEMAP 0x4
#define TEXTURE_FORMAT_DIMENSION_MASK 0xf0
#define TEXTURE_FORMAT_DIMENSION_3 0x30
#define TEXTURE_FORMAT_COLOR_MASK 0xff00
#define TEXTURE_FORMAT_DEPTH_FIRST 0x2a00
#define TEXTURE_FORMAT_DEPTH_LAST 0x3100

/* 0x1f0750: texture modes of the four stages (stage 0 in the low bits). */
static void lazy_texture_modes(void)
{
  const unsigned long *texture;
  unsigned long modes;
  unsigned long ps_modes;
  unsigned long mode;
  unsigned long format;
  int stage;
  int shift;
  uint32_t *p;

  *device_field(DEVICE_DIRTY) &= ~(unsigned long)DIRTY_TEXTURE_MODES;
  modes = 0;
  if (*device_field(DEVICE_PIXEL_SHADER) == 0) {
    for (stage = TEXTURE_STAGE_COUNT - 1; stage >= 0; stage--) {
      texture = device_texture((unsigned long)stage);
      mode = STAGE_MODE_2D;
      if (!texture) {
        mode = STAGE_MODE_NONE;
      } else if (stage != 0 &&
                 TEXTURE_STATE(stage - 1, D3DTSS_COLOROP) == D3DTOP_BUMPENVMAP) {
        mode = STAGE_MODE_BUMPENVMAP;
      } else if (stage != 0 &&
                 TEXTURE_STATE(stage - 1, D3DTSS_COLOROP) == D3DTOP_BUMPENVMAPLUMINANCE) {
        mode = STAGE_MODE_BUMPENVMAP_LUMINANCE;
      } else if (texture[RESOURCE_FORMAT] & TEXTURE_FORMAT_CUBEMAP) {
        mode = STAGE_MODE_CUBE;
      } else if ((texture[RESOURCE_FORMAT] & TEXTURE_FORMAT_DIMENSION_MASK) ==
                 TEXTURE_FORMAT_DIMENSION_3) {
        mode = STAGE_MODE_3D;
      }
      modes = (modes << 5) | mode;
    }
  } else if (*device_field(DEVICE_PS_DOTMAP_TEXTURE_MODES) == 0) {
    modes = *device_field(DEVICE_PS_TEXTURE_MODES);
  } else {
    ps_modes = *device_field(DEVICE_PS_TEXTURE_MODES);
    for (stage = TEXTURE_STAGE_COUNT - 1, shift = 15; shift >= 0; stage--, shift -= 5) {
      texture = device_texture((unsigned long)stage);
      mode = (ps_modes >> shift) & 0x1f;
      if (!texture && mode != 4 && mode != 5 && mode != 0xa && mode != 0x11) {
        mode = STAGE_MODE_NONE;
      } else if (mode >= 1 && mode <= 3) {
        format = texture[RESOURCE_FORMAT];
        if (format & TEXTURE_FORMAT_CUBEMAP) {
          mode = STAGE_MODE_CUBE;
        } else if ((format & TEXTURE_FORMAT_DIMENSION_MASK) == TEXTURE_FORMAT_DIMENSION_3 ||
                   ((format & TEXTURE_FORMAT_COLOR_MASK) >= TEXTURE_FORMAT_DEPTH_FIRST &&
                    (format & TEXTURE_FORMAT_COLOR_MASK) <= TEXTURE_FORMAT_DEPTH_LAST)) {
          mode = STAGE_MODE_3D;
        } else {
          mode = STAGE_MODE_2D;
        }
      } else if (mode == 0xd || mode == 0xe) {
        mode = STAGE_MODE_DOT_STR_3D + ((texture[RESOURCE_FORMAT] & TEXTURE_FORMAT_CUBEMAP) ? 1 : 0);
      }
      modes = (modes << 5) | mode;
    }
  }
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_SHADER_STAGE_PROGRAM, 1);
  p[1] = modes;
  compat_d3d_push_end(p + 2);
  TRACE_STAGE_PROGRAM(modes);
}

/* 0x1f08d0: fixed-function texture-stage combiners. With a pixel shader set
 * (all of Halo's normal rendering) it only clears the dirty bit. The
 * fixed-function path (stage compilers 0x1f7330 / 0x1f68d0 and the combiner
 * push at 0x1f0956) is not transcribed. */
#define COMBINER_SOURCE_CONSTANT0 0x01
#define COMBINER_SOURCE_DIFFUSE 0x04
#define COMBINER_SOURCE_SPECULAR 0x05
#define COMBINER_SOURCE_TEXTURE0 0x08
#define COMBINER_SOURCE_CURRENT 0x0c
#define COMBINER_SOURCE_TEMP 0x0d
#define COMBINER_COMPONENT_ALPHA 0x10
#define COMBINER_MAP_UNSIGNED_INVERT 0x20
#define COMBINER_OUTPUT_CURRENT 0x00000c00

static unsigned long combiner_argument(unsigned long stage, unsigned long argument,
                                       int alpha, int *missing_texture)
{
  unsigned long source;

  switch (argument & 0xf) {
  case D3DTA_CURRENT:
    source = stage == 0 ? COMBINER_SOURCE_DIFFUSE : COMBINER_SOURCE_CURRENT;
    break;
  case D3DTA_TEXTURE:
    if (device_texture(stage) == 0) {
      *missing_texture = 1;
      source = stage == 0 ? COMBINER_SOURCE_DIFFUSE : COMBINER_SOURCE_CURRENT;
    } else {
      source = COMBINER_SOURCE_TEXTURE0 + stage;
    }
    break;
  case D3DTA_TFACTOR:
    source = COMBINER_SOURCE_CONSTANT0;
    break;
  case D3DTA_SPECULAR:
    source = COMBINER_SOURCE_SPECULAR;
    *device_field(DEVICE_STATE_FLAGS) |= STATE_FLAG_FORCE_SPECULAR;
    break;
  case D3DTA_TEMP:
    source = COMBINER_SOURCE_TEMP;
    break;
  default:
    source = COMBINER_SOURCE_DIFFUSE;
    break;
  }
  if (alpha || (argument & D3DTA_ALPHAREPLICATE)) {
    source |= COMBINER_COMPONENT_ALPHA;
  }
  if (argument & D3DTA_COMPLEMENT) {
    source |= COMBINER_MAP_UNSIGNED_INVERT;
  }
  return source;
}

static int build_fixed_combiner(unsigned long stage, unsigned long op,
                                unsigned long arg1, unsigned long arg2,
                                int alpha, unsigned long *input,
                                unsigned long *output)
{
  unsigned long a;
  unsigned long b;
  int missing_texture;

  missing_texture = 0;
  a = combiner_argument(stage, arg1, alpha, &missing_texture);
  b = combiner_argument(stage, arg2, alpha, &missing_texture);
  *input = 0;
  *output = COMBINER_OUTPUT_CURRENT;
  if (missing_texture) {
    a = (stage == 0 ? COMBINER_SOURCE_DIFFUSE : COMBINER_SOURCE_CURRENT) |
        (alpha ? COMBINER_COMPONENT_ALPHA : 0);
    *input = (a << 24) | (COMBINER_MAP_UNSIGNED_INVERT << 16);
    return 1;
  }
  switch (op) {
  case D3DTOP_SELECTARG2:
    *input = (b << 24) | (COMBINER_MAP_UNSIGNED_INVERT << 16);
    break;
  case D3DTOP_MODULATE:
    *input = (a << 24) | (b << 16);
    break;
  default:
    *input = (a << 24) | (COMBINER_MAP_UNSIGNED_INVERT << 16);
    break;
  }
  return missing_texture;
}

static void lazy_combiners(void)
{
  unsigned long color_input[8];
  unsigned long color_output[8];
  unsigned long alpha_input[8];
  unsigned long alpha_output[8];
  unsigned long count;
  unsigned long stage;
  unsigned long color_op;
  unsigned long alpha_op;
  unsigned long i;
  int missing_texture;
  uint32_t *p;

  *device_field(DEVICE_DIRTY) &= ~(unsigned long)DIRTY_COMBINERS;
  if (*device_field(DEVICE_PIXEL_SHADER) != 0) {
    return;
  }
  count = 0;
  for (stage = 0; stage < TEXTURE_STAGE_COUNT; stage++) {
    color_op = TEXTURE_STATE(stage, D3DTSS_COLOROP);
    if (color_op == D3DTOP_DISABLE && stage != 0) {
      break;
    }
    if (color_op != D3DTOP_SELECTARG1 && color_op != D3DTOP_SELECTARG2 &&
        color_op != D3DTOP_MODULATE) {
      halo_standalone_missing("D3D fixed-function color operation");
      color_op = D3DTOP_SELECTARG1;
    }
    alpha_op = TEXTURE_STATE(stage, D3DTSS_ALPHAOP);
    if (alpha_op != D3DTOP_SELECTARG1 && alpha_op != D3DTOP_SELECTARG2 &&
        alpha_op != D3DTOP_MODULATE) {
      halo_standalone_missing("D3D fixed-function alpha operation");
      alpha_op = D3DTOP_SELECTARG1;
    }
    missing_texture = build_fixed_combiner(
      stage, color_op, TEXTURE_STATE(stage, D3DTSS_COLORARG1),
      TEXTURE_STATE(stage, D3DTSS_COLORARG2), 0, &color_input[count],
      &color_output[count]);
    missing_texture |= build_fixed_combiner(
      stage, alpha_op, TEXTURE_STATE(stage, D3DTSS_ALPHAARG1),
      TEXTURE_STATE(stage, D3DTSS_ALPHAARG2), 1, &alpha_input[count],
      &alpha_output[count]);
    count++;
    if (missing_texture) {
      break;
    }
  }
  if (count == 0) {
    count = 1;
    build_fixed_combiner(0, D3DTOP_SELECTARG1, D3DTA_CURRENT,
                         D3DTA_CURRENT, 0, &color_input[0],
                         &color_output[0]);
    build_fixed_combiner(0, D3DTOP_SELECTARG1, D3DTA_CURRENT,
                         D3DTA_CURRENT, 1, &alpha_input[0],
                         &alpha_output[0]);
  }
  for (i = count; i < 8; i++) {
    color_input[i] = 0;
    color_output[i] = 0;
    alpha_input[i] = 0;
    alpha_output[i] = 0;
  }
  *device_field(DEVICE_FF_COMBINER_COUNT) = count;
  p = compat_d3d_push_begin(38);
  p[0] = METHOD(NV097_SET_COMBINER_CONTROL, 1);
  p[1] = count & 0xff;
  p[2] = METHOD(NV097_SET_COMBINER_COLOR_ICW, 8);
  for (i = 0; i < 8; i++) {
    p[3 + i] = color_input[i];
  }
  p[11] = METHOD(NV097_SET_COMBINER_COLOR_OCW, 8);
  for (i = 0; i < 8; i++) {
    p[12 + i] = color_output[i];
  }
  p[20] = METHOD(NV097_SET_COMBINER_ALPHA_ICW, 8);
  for (i = 0; i < 8; i++) {
    p[21 + i] = alpha_input[i];
  }
  p[29] = METHOD(NV097_SET_COMBINER_ALPHA_OCW, 8);
  for (i = 0; i < 8; i++) {
    p[30 + i] = alpha_output[i];
  }
  compat_d3d_push_end(p + 38);
}

/* 0x1fa720: NV097 texture filter MIN field by [min filter][mip filter]. */
static const unsigned long texture_min_filter_table[9] = {
  0x00000080, 0x00000040, 0x00000020, /* min none (unused) */
  0x00010000, 0x00030000, 0x00050000, /* point */
  0x00020000, 0x00040000, 0x00060000, /* linear */
};
#define FLOAT_LOD_BIAS_UNITS 256.0f /* 0x2642a0 */
#define TEXTURE_FILTER_BASE 0x2000
#define TEXTURE_FILTER_GAUSSIAN 0x4000
#define TEXTURE_FILTER_CONVOLUTION 0x70000
#define TEXTURE_CONTROL0_BASE 0x3ffc0
#define TEXTURE_CONTROL0_ENABLE 0x40000000ul
#define TEXTURE_SIGNED_BUMP 0xc0000000ul
#define D3DTEXF_POINT 1
#define D3DTEXF_LINEAR 2
#define D3DTEXF_ANISOTROPIC 3
#define D3DTEXF_GAUSSIANCUBIC 5
#define NV_MAG_CONVOLUTION 4

/* 0x1f0ad0: per-stage address, control0 and filter. */
static void lazy_texture_control(void)
{
  unsigned long stage;
  unsigned long address;
  unsigned long control0;
  unsigned long min_filter;
  unsigned long mag_filter;
  unsigned long filter_bits;
  unsigned long signs;
  unsigned long anisotropy;
  long lod;
  dword_float bias;
  uint32_t *p;

  *device_field(DEVICE_DIRTY) &= ~(unsigned long)DIRTY_TEXTURE_CONTROL;
  for (stage = 0; stage < TEXTURE_STAGE_COUNT; stage++) {
    address = D3D_RENDER_STATE[D3DRS_WRAP0 + (TEXTURE_STATE(stage, D3DTSS_TEXCOORDINDEX) & 0xffff)] |
              ((TEXTURE_STATE(stage, D3DTSS_ADDRESSW) << 8 | TEXTURE_STATE(stage, D3DTSS_ADDRESSV))
               << 8) |
              TEXTURE_STATE(stage, D3DTSS_ADDRESSU);
    control0 = (TEXTURE_STATE(stage, D3DTSS_MAXMIPLEVEL) << 26) |
               TEXTURE_STATE(stage, D3DTSS_ALPHAKILL) |
               TEXTURE_STATE(stage, D3DTSS_COLORKEYOP) | TEXTURE_CONTROL0_BASE;
    if (device_texture(stage)) {
      control0 |= TEXTURE_CONTROL0_ENABLE;
    }
    min_filter = TEXTURE_STATE(stage, D3DTSS_MINFILTER);
    mag_filter = TEXTURE_STATE(stage, D3DTSS_MAGFILTER);
    filter_bits = TEXTURE_FILTER_BASE;
    if (min_filter >= D3DTEXF_ANISOTROPIC || mag_filter >= D3DTEXF_ANISOTROPIC) {
      if (min_filter <= D3DTEXF_ANISOTROPIC && mag_filter <= D3DTEXF_ANISOTROPIC) {
        anisotropy = TEXTURE_STATE(stage, D3DTSS_MAXANISOTROPY);
        if (anisotropy == 0) {
          min_filter = D3DTEXF_POINT;
          mag_filter = D3DTEXF_POINT;
        } else {
          min_filter = D3DTEXF_LINEAR;
          mag_filter = D3DTEXF_LINEAR;
          control0 |= (anisotropy - 1) << 4;
        }
      } else {
        if (min_filter == D3DTEXF_GAUSSIANCUBIC || mag_filter == D3DTEXF_GAUSSIANCUBIC) {
          filter_bits = TEXTURE_FILTER_GAUSSIAN;
        }
        filter_bits |= TEXTURE_FILTER_CONVOLUTION;
        min_filter = D3DTEXF_LINEAR;
        mag_filter = NV_MAG_CONVOLUTION;
      }
    }
    *device_field(DEVICE_TEXTURE_CONTROL0 + stage * 4) = control0 | TEXTURE_CONTROL0_ENABLE;
    bias.u = TEXTURE_STATE(stage, D3DTSS_MIPMAPLODBIAS);
    lod = (long)((bias.f + device_float(DEVICE_LOD_BIAS)) * FLOAT_LOD_BIAS_UNITS + FLOAT_HALF);
    if (lod < -0x1000) {
      lod = -0x1000;
    } else if (lod > 0xfff) {
      lod = 0xfff;
    }
    if (TEXTURE_STATE(stage, D3DTSS_COLOROP) >= D3DTOP_BUMPENVMAP) {
      signs = TEXTURE_SIGNED_BUMP;
    } else {
      signs = TEXTURE_STATE(stage, D3DTSS_COLORSIGN);
    }
    p = compat_d3d_push_begin(5);
    p[0] = METHOD(NV097_SET_TEXTURE_ADDRESS + stage * 0x40, 2);
    p[1] = address;
    p[2] = control0;
    TRACE_CONTROL0(stage, control0);
    p[3] = METHOD(NV097_SET_TEXTURE_FILTER + stage * 0x40, 1);
    p[4] = ((unsigned long)lod & 0x1fff) | (mag_filter << 24) |
           texture_min_filter_table[min_filter * 3 + TEXTURE_STATE(stage, D3DTSS_MIPFILTER)] |
           signs | filter_bits;
    compat_d3d_push_end(p + 5);
  }
}

#define FOG_GEN_MODE_SPEC_ALPHA 0
#define FOG_MODE_LINEAR 0x2601
#define FOG_MODE_EXP 0x800
#define FOG_MODE_EXP2 0x801
#define D3DFOG_NONE 0
#define D3DFOG_EXP 1
#define D3DFOG_LINEAR 3
#define FLOAT_FOG_FLAT_SCALE 8192.0f   /* 0x1fa748 */
#define FLOAT_FOG_EXP_BIAS 0x3fc00000ul /* 1.5f */
#define FLOAT_FOG_EXP_SCALE_BITS 0xbdb8aa0aul  /* 0x2c20fc */
#define FLOAT_FOG_EXP2_SCALE_BITS 0xbe596d0bul /* 0x2c20f8 */
#define SPECULAR_FOG_CW0 0x130c0300ul
#define SPECULAR_FOG_CW0_SPECULAR 0x00020000ul
#define SPECULAR_FOG_CW1 0x1c80

/* 0x1f0cd0: fog, and the final combiner when no pixel shader provides one. */
static void lazy_fog(void)
{
  unsigned long gen_mode;
  unsigned long mode;
  unsigned long bias;
  unsigned long scale;
  float start;
  float end;
  float f;
  dword_float v;
  uint32_t *p;

  *device_field(DEVICE_DIRTY) &= ~(unsigned long)DIRTY_FOG;
  compat_d3d_push_passthrough_program();
  if (D3D_RENDER_STATE[D3DRS_FOGENABLE]) {
    start = render_state_float(D3DRS_FOGSTART);
    end = render_state_float(D3DRS_FOGEND);
    gen_mode = (D3D_RENDER_STATE[D3DRS_RANGEFOGENABLE] == 0) + 1;
    scale = render_state_float_bits(D3DRS_FOGSTART);
    bias = render_state_float_bits(D3DRS_FOGEND);
    if (D3D_RENDER_STATE[D3DRS_FOGTABLEMODE] == D3DFOG_NONE) {
      bias = float_bits(FLOAT_ONE);
      scale = float_bits(FLOAT_ONE);
      mode = FOG_MODE_LINEAR;
      gen_mode = FOG_GEN_MODE_SPEC_ALPHA;
    } else if (D3D_RENDER_STATE[D3DRS_FOGTABLEMODE] == D3DFOG_LINEAR) {
      if (end == start) {
        f = FLOAT_FOG_FLAT_SCALE;
      } else {
        f = FLOAT_ONE / (end - start);
      }
      mode = FOG_MODE_LINEAR;
      bias = float_bits(end * f + FLOAT_ONE);
      scale = float_bits(-f);
    } else {
      bias = FLOAT_FOG_EXP_BIAS;
      if (D3D_RENDER_STATE[D3DRS_FOGTABLEMODE] == D3DFOG_EXP) {
        v.u = FLOAT_FOG_EXP_SCALE_BITS;
        mode = FOG_MODE_EXP;
      } else {
        v.u = FLOAT_FOG_EXP2_SCALE_BITS;
        mode = FOG_MODE_EXP2;
      }
      scale = float_bits(render_state_float(D3DRS_FOGDENSITY) * v.f);
    }
    p = compat_d3d_push_begin(12);
    p[0] = METHOD(NV097_SET_FOG_GEN_MODE, 2);
    p[1] = gen_mode;
    p[2] = 1;
    p[3] = METHOD(NV097_SET_FOG_MODE, 1);
    p[4] = mode;
    p[5] = METHOD(NV097_SET_FOG_PARAMS, 3);
    p[6] = bias;
    p[7] = scale;
    p[8] = 0;
    p += 9;
    if (*device_field(DEVICE_PIXEL_SHADER) != 0 && *device_field(DEVICE_PS_COMBINER_COUNT) != 0) {
      compat_d3d_push_end(p);
      return;
    }
    p[0] = METHOD(NV097_SET_COMBINER_SPECULAR_FOG_CW0, 2);
    p[1] = SPECULAR_FOG_CW0 +
           (D3D_RENDER_STATE[D3DRS_SPECULARENABLE] ? SPECULAR_FOG_CW0_SPECULAR : 0);
    p[2] = SPECULAR_FOG_CW1;
    compat_d3d_push_end(p + 3);
    return;
  }
  p = compat_d3d_push_begin(5);
  p[0] = METHOD(NV097_SET_FOG_ENABLE, 1);
  p[1] = 0;
  p += 2;
  if (*device_field(DEVICE_PIXEL_SHADER) != 0 && *device_field(DEVICE_PS_COMBINER_COUNT) != 0) {
    compat_d3d_push_end(p);
    return;
  }
  p[0] = METHOD(NV097_SET_COMBINER_SPECULAR_FOG_CW0, 2);
  p[1] = D3D_RENDER_STATE[D3DRS_SPECULARENABLE] ? 0xe : 0xc;
  p[2] = SPECULAR_FOG_CW1;
  compat_d3d_push_end(p + 3);
}

#define D3DTSS_TEXTURETRANSFORMFLAGS 0x09
#define D3DTTFF_COUNT_MASK 0xff
#define D3DTTFF_PROJECTED 0x100
#define TEXCOORDINDEX_GENERATION_MASK 0xffff0000ul
#define TEXCOORD_GENERATED_SIZE 3
#define TEXCOORD_DEFAULT_SIZE 2
/* D3DTS_TEXTURE0 (2) in the device's transform array (0x880 + 2 * 0x40). */
#define DEVICE_TEXTURE_MATRIX 0x900
#define TEXTURE_MATRIX_STRIDE 0x40
#define TEXTURE_MATRIX_DWORDS 16
#define TEXTURE_TRANSFORM_PUSH_DWORDS (3 + TEXTURE_MATRIX_DWORDS)

/* Texture-transform matrix key (0x1f0f4c..0x1f0f7b):
 * input size << 8 | output count << 4 | projected. */
#define TEXTURE_TRANSFORM_KEY(in, count, projected) \
  (((in) << 8) | ((count) << 4) | (projected))

/* NV2A row r of a 2-component (u, v, 1) source: D3D's third row carries the
 * translation that NV2A takes from its fourth column. */
static void texture_matrix_row_2d(unsigned long *out, const unsigned long *m,
                                  int r)
{
  out[0] = m[r];
  out[1] = m[4 + r];
  out[2] = 0;
  out[3] = m[8 + r];
}

/* NV2A row r of a 3/4-component source: D3D's column r. */
static void texture_matrix_row(unsigned long *out, const unsigned long *m,
                               int r)
{
  out[0] = m[r];
  out[1] = m[4 + r];
  out[2] = m[8 + r];
  out[3] = m[12 + r];
}

static void texture_matrix_row_zero(unsigned long *out)
{
  out[0] = out[1] = out[2] = out[3] = 0;
}

static void texture_matrix_row_w(unsigned long *out)
{
  out[0] = out[1] = out[2] = 0;
  out[3] = float_bits(1.0f);
}

/* 0x1f0eb0: texture-coordinate transforms for the fixed-function vertex
 * path. Per stage, D3DTSS_TEXTURETRANSFORMFLAGS 0 disables the NV2A texture
 * matrix; otherwise the D3DTS_TEXTUREn matrix is rearranged for the source
 * coordinate size (3 for generated coordinates, else the vertex shader's
 * texcoord size, 2 when the index is out of range) and the output count. */
static void lazy_texture_transform(void)
{
  const unsigned long *m;
  unsigned long *vs;
  unsigned long flags;
  unsigned long index;
  unsigned long in_size;
  unsigned long *out;
  uint32_t *p;
  int stage;

  *device_field(DEVICE_DIRTY) &= ~(unsigned long)DIRTY_TEXTURE_TRANSFORM;
  vs = current_vertex_shader();
  if (vs[VS_FLAGS] & (VS_FLAG_PASSTHROUGH | VS_FLAG_PROGRAM)) {
    return;
  }
  for (stage = 0; stage < TEXTURE_STAGE_COUNT; stage++) {
    flags = TEXTURE_STATE(stage, D3DTSS_TEXTURETRANSFORMFLAGS);
    p = compat_d3d_push_begin(TEXTURE_TRANSFORM_PUSH_DWORDS);
    p[0] = METHOD(NV097_SET_TEXTURE_MATRIX_ENABLE + stage * 4, 1);
    if (flags == 0) {
      p[1] = 0;
      compat_d3d_push_end(p + 2);
      continue;
    }
    index = TEXTURE_STATE(stage, D3DTSS_TEXCOORDINDEX);
    if (index & TEXCOORDINDEX_GENERATION_MASK) {
      in_size = TEXCOORD_GENERATED_SIZE;
    } else if ((index & 0xffff) < vs[VS_TEXCOORD_COUNT]) {
      in_size = vs[VS_TEXCOORD_SIZE + (index & 0xffff)];
    } else {
      in_size = TEXCOORD_DEFAULT_SIZE;
    }
    p[1] = 1;
    p[2] = METHOD(NV097_SET_TEXTURE_MATRIX + stage * TEXTURE_MATRIX_STRIDE,
                  TEXTURE_MATRIX_DWORDS);
    m = device_field(DEVICE_TEXTURE_MATRIX + stage * TEXTURE_MATRIX_STRIDE);
    out = (unsigned long *)(p + 3);
    switch (TEXTURE_TRANSFORM_KEY(in_size, flags & D3DTTFF_COUNT_MASK,
                                  (flags & D3DTTFF_PROJECTED) ? 1 : 0)) {
    case TEXTURE_TRANSFORM_KEY(2, 2, 0):
      texture_matrix_row_2d(out, m, 0);
      texture_matrix_row_2d(out + 4, m, 1);
      texture_matrix_row_zero(out + 8);
      texture_matrix_row_w(out + 12);
      break;
    case TEXTURE_TRANSFORM_KEY(2, 3, 0):
      texture_matrix_row_2d(out, m, 0);
      texture_matrix_row_2d(out + 4, m, 1);
      texture_matrix_row_2d(out + 8, m, 2);
      texture_matrix_row_w(out + 12);
      break;
    case TEXTURE_TRANSFORM_KEY(2, 3, 1):
      texture_matrix_row_2d(out, m, 0);
      texture_matrix_row_2d(out + 4, m, 1);
      texture_matrix_row_zero(out + 8);
      texture_matrix_row_2d(out + 12, m, 2);
      break;
    case TEXTURE_TRANSFORM_KEY(2, 4, 1):
      texture_matrix_row_2d(out, m, 0);
      texture_matrix_row_2d(out + 4, m, 1);
      texture_matrix_row_2d(out + 8, m, 2);
      texture_matrix_row_2d(out + 12, m, 3);
      break;
    case TEXTURE_TRANSFORM_KEY(3, 2, 0):
      texture_matrix_row(out, m, 0);
      texture_matrix_row(out + 4, m, 1);
      texture_matrix_row_zero(out + 8);
      texture_matrix_row_w(out + 12);
      break;
    case TEXTURE_TRANSFORM_KEY(3, 3, 0):
      texture_matrix_row(out, m, 0);
      texture_matrix_row(out + 4, m, 1);
      texture_matrix_row(out + 8, m, 2);
      texture_matrix_row_w(out + 12);
      break;
    case TEXTURE_TRANSFORM_KEY(3, 3, 1):
      texture_matrix_row(out, m, 0);
      texture_matrix_row(out + 4, m, 1);
      texture_matrix_row_zero(out + 8);
      texture_matrix_row(out + 12, m, 3);
      break;
    default:
      texture_matrix_row(out, m, 0);
      texture_matrix_row(out + 4, m, 1);
      texture_matrix_row(out + 8, m, 2);
      texture_matrix_row(out + 12, m, 3);
      break;
    }
    compat_d3d_push_end(p + TEXTURE_TRANSFORM_PUSH_DWORDS);
  }
}

/* 0x1f1640: lighting. The fixed-function light setup (0x1f1684..0x1f1918,
 * a vertex program or D3DRS_LIGHTING on) is not transcribed; the tail with
 * lighting disabled is. */
static void lazy_lighting(void)
{
  unsigned long specular;
  uint32_t *p;

  *device_field(DEVICE_DIRTY) &= ~(unsigned long)DIRTY_LIGHTING;
  specular = (D3D_RENDER_STATE[D3DRS_SPECULARENABLE] != 0 ||
              (*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_FORCE_SPECULAR)) ? 1 : 0;
  if (!(current_vertex_shader()[VS_FLAGS] & (VS_FLAG_PASSTHROUGH | VS_FLAG_PROGRAM)) &&
      D3D_RENDER_STATE[D3DRS_LIGHTING] != 0) {
    halo_standalone_missing("D3D fixed-function lighting (0x1f1640)");
  }
  p = compat_d3d_push_begin(8);
  p[0] = METHOD(NV097_SET_LIGHTING_ENABLE, 1);
  p[1] = 0;
  p[2] = METHOD(NV097_SET_SPECULAR_ENABLE, 1);
  p[3] = specular;
  p[4] = METHOD(NV097_SET_LIGHT_CONTROL, 1);
  p[5] = 0x20001;
  p[6] = METHOD(NV097_SET_LIGHT_TWO_SIDE_ENABLE, 1);
  p[7] = D3D_RENDER_STATE[D3DRS_TWOSIDEDLIGHTING];
  compat_d3d_push_end(p + 8);
}

/* 0x1f9f48 */
const float compat_d3d_identity_matrix[16] = {
  1.0f, 0.0f, 0.0f, 0.0f,
  0.0f, 1.0f, 0.0f, 0.0f,
  0.0f, 0.0f, 1.0f, 0.0f,
  0.0f, 0.0f, 0.0f, 1.0f,
};

void compat_d3d_matrix4x4_multiply(float *out, const float *left, const float *right)
{
  float result[16];
  int row;
  int column;
  int i;

  for (row = 0; row < 4; row++) {
    for (column = 0; column < 4; column++) {
      result[row * 4 + column] = 0.0f;
      for (i = 0; i < 4; i++) {
        result[row * 4 + column] += left[row * 4 + i] * right[i * 4 + column];
      }
    }
  }
  compat_memcpy(out, result, sizeof(result));
}

uint32_t *compat_d3d_push_transposed_matrix(uint32_t *p, unsigned long method,
                                            const float *matrix)
{
  const unsigned long *words;
  int row;
  int column;

  words = (const unsigned long *)matrix;
  p[0] = METHOD(method, 16);
  for (row = 0; row < 4; row++) {
    for (column = 0; column < 4; column++) {
      p[1 + row * 4 + column] = words[column * 4 + row];
    }
  }
  return p + 17;
}

/* 0x1f1960: fixed-function model-view and composite matrices. The optional
 * inverse-model-view payload is still handled separately because 0x1f2650's
 * normalize-normal scaling has not yet been transcribed. */
static void lazy_transforms(void)
{
  float model_view[16];
  float composite[16];
  float blended[16];
  unsigned long dirty;
  unsigned long blend;
  int matrix_index;
  uint32_t *p;

  dirty = *device_field(DEVICE_DIRTY) & ~(unsigned long)DIRTY_TRANSFORMS;
  *device_field(DEVICE_DIRTY) = dirty;
  if ((long)dirty < 0) {
    return;
  }
  if (current_vertex_shader()[VS_FLAGS] & (VS_FLAG_PASSTHROUGH | VS_FLAG_PROGRAM)) {
    return;
  }

  compat_d3d_matrix4x4_multiply(model_view, (const float *)device_field(DEVICE_WORLD_MATRIX),
                     (const float *)device_field(DEVICE_VIEW_MATRIX));
  compat_d3d_matrix4x4_multiply(composite, model_view,
                     (const float *)device_field(DEVICE_DERIVED_PROJECTION_MATRIX));
  blend = D3D_RENDER_STATE[D3DRS_VERTEXBLEND];
  p = compat_d3d_push_begin(17 + 17 + (blend ? 3 * 17 : 0));
  p = compat_d3d_push_transposed_matrix(p, NV097_SET_MODEL_VIEW_MATRIX, model_view);
  p = compat_d3d_push_transposed_matrix(p, NV097_SET_COMPOSITE_MATRIX, composite);
  if (blend) {
    for (matrix_index = 0; matrix_index < 3; matrix_index++) {
      compat_d3d_matrix4x4_multiply(
        blended,
        (const float *)device_field(DEVICE_BLEND_MATRIX_1 + matrix_index * 0x40),
        (const float *)device_field(DEVICE_VIEW_MATRIX));
      p = compat_d3d_push_transposed_matrix(p, NV097_SET_MODEL_VIEW_MATRIX + (matrix_index + 1) * 0x40,
                                 blended);
    }
  }
  compat_d3d_push_end(p);

  if (*device_field(DEVICE_TRANSFORM_FLAGS) != 0 ||
      D3D_RENDER_STATE[D3DRS_NORMALIZENORMALS] != 0) {
    halo_standalone_missing("D3D inverse model-view transform (0x1f2650)");
  }
}

/* 0x1f1b50 */
static void lazy_set_state(void)
{
  unsigned long dirty;

  dirty = *device_field(DEVICE_DIRTY);
  if (dirty == 0) {
    return;
  }
  if (dirty & DIRTY_POINT_PARAMS) {
    lazy_point_params();
  }
  if (dirty & DIRTY_TEXTURE_MODES) {
    lazy_texture_modes();
  }
  if (dirty & DIRTY_COMBINERS) {
    lazy_combiners();
  }
  if (dirty & DIRTY_TEXTURE_CONTROL) {
    lazy_texture_control();
  }
  if (dirty & DIRTY_FOG) {
    lazy_fog();
  }
  if (dirty & DIRTY_TEXTURE_TRANSFORM) {
    lazy_texture_transform();
  }
  if (dirty & DIRTY_LIGHTING) {
    lazy_lighting();
  }
  if (dirty & DIRTY_TRANSFORMS) {
    lazy_transforms();
  }
}

/* ------------------------------------------------------------------------
 * Vertex-buffer state and the draws.
 */

/* Default values of the attributes a fixed-function shader does not read
 * from a stream (0x1f1c65). */
#define DEFAULT_DIFFUSE 0xfffffffful
#define DEFAULT_SPECULAR 0

/* 0x1f1bc0 (ecx = device): lazy state, then the vertex array format and
 * offsets. */
static void set_state_vb(unsigned long base_vertex_index)
{
  unsigned long *vs;
  unsigned long dirty;
  unsigned long index;
  unsigned long last;
  unsigned long slot;
  unsigned long buffer;
  unsigned long address;
  unsigned long i;
  uint32_t *p;

  lazy_set_state();
  TRACE_DRAW("draw");
  dirty = *device_field(DEVICE_DIRTY);
  if (dirty & DIRTY_SKIP_VERTEX_STATE) {
    return;
  }
  if (dirty & DIRTY_VERTEX_FORMAT) {
    vs = current_vertex_shader();
    *device_field(DEVICE_DIRTY) = (dirty & 0xbfffff7ful) | 0x300;
    if (!(vs[VS_FLAGS] & VS_FLAG_PROGRAM)) {
      for (i = 0; i < TEXTURE_STAGE_COUNT; i++) {
        index = TEXTURE_STATE(i, D3DTSS_TEXCOORDINDEX) & 0xffff;
        if (index < vs[VS_TEXCOORD_COUNT]) {
          vs[VS_SLOT(VSD_REG_TEXCOORD0 + i) + 0] = vs[VS_SLOT(VS_TEXCOORD_TEMPLATE_SLOT + index) + 0];
          vs[VS_SLOT(VSD_REG_TEXCOORD0 + i) + 1] = vs[VS_SLOT(VS_TEXCOORD_TEMPLATE_SLOT + index) + 1];
          vs[VS_SLOT(VSD_REG_TEXCOORD0 + i) + 2] = vs[VS_SLOT(VS_TEXCOORD_TEMPLATE_SLOT + index) + 2];
          vs[VS_SLOT(VSD_REG_TEXCOORD0 + i) + 3] = vs[VS_SLOT(VS_TEXCOORD_TEMPLATE_SLOT + index) + 3];
        } else {
          vs[VS_SLOT(VSD_REG_TEXCOORD0 + i) + VS_SLOT_TYPE] = VS_SLOT_DISABLED;
        }
      }
      p = compat_d3d_push_begin(8);
      if (!(vs[VS_FLAGS] & VS_FLAG_DIFFUSE)) {
        p[0] = METHOD(NV097_SET_VERTEX_DATA4UB + VSD_REG_DIFFUSE * 4, 1);
        p[1] = DEFAULT_DIFFUSE;
        p += 2;
      }
      if (!(vs[VS_FLAGS] & VS_FLAG_SPECULAR)) {
        p[0] = METHOD(NV097_SET_VERTEX_DATA4UB + VSD_REG_SPECULAR * 4, 1);
        p[1] = DEFAULT_SPECULAR;
        p += 2;
      }
      if (!(vs[VS_FLAGS] & VS_FLAG_BACK_DIFFUSE)) {
        p[0] = METHOD(NV097_SET_VERTEX_DATA4UB + VSD_REG_BACK_DIFFUSE * 4, 1);
        p[1] = DEFAULT_DIFFUSE;
        p += 2;
      }
      if (!(vs[VS_FLAGS] & VS_FLAG_BACK_SPECULAR)) {
        p[0] = METHOD(NV097_SET_VERTEX_DATA4UB + VSD_REG_BACK_SPECULAR * 4, 1);
        p[1] = DEFAULT_SPECULAR;
        p += 2;
      }
      compat_d3d_push_end(p);
    }
    last = VS_INPUT_SLOTS - 1;
    while (vs[VS_SLOT(last) + VS_SLOT_TYPE] == VS_SLOT_DISABLED) {
      last--;
      if (last == 0) {
        break;
      }
    }
    vs[VS_SLOT_COUNT] = last;
    p = compat_d3d_push_begin(17);
    p[0] = METHOD(NV097_SET_VERTEX_DATA_ARRAY_FORMAT, VS_INPUT_SLOTS);
    for (slot = 0; slot < VS_INPUT_SLOTS; slot++) {
      p[1 + slot] = (STREAM_STRIDE(vs[VS_SLOT(slot) + VS_SLOT_STREAM]) << 8) +
                    vs[VS_SLOT(slot) + VS_SLOT_TYPE];
    }
    compat_d3d_push_end(p + 17);
  }
  dirty = *device_field(DEVICE_DIRTY);
  if (!(dirty & DIRTY_VERTEX_OFFSETS) &&
      *device_field(DEVICE_LAST_BASE_VERTEX_INDEX) == base_vertex_index) {
    return;
  }
  vs = current_vertex_shader();
  *device_field(DEVICE_DIRTY) = dirty & 0xbffffdfful;
  *device_field(DEVICE_LAST_BASE_VERTEX_INDEX) = base_vertex_index;
  p = compat_d3d_push_begin(2 * VS_INPUT_SLOTS);
  last = vs[VS_SLOT_COUNT];
  for (slot = 0; slot <= last; slot++) {
    if (vs[VS_SLOT(slot) + VS_SLOT_TYPE] == VS_SLOT_DISABLED) {
      continue;
    }
    buffer = STREAM_BUFFER(vs[VS_SLOT(slot) + VS_SLOT_STREAM]);
    if (!buffer) {
      continue;
    }
    address = ((const unsigned long *)buffer)[RESOURCE_DATA] + vs[VS_SLOT(slot) + VS_SLOT_OFFSET] +
              STREAM_OFFSET(vs[VS_SLOT(slot) + VS_SLOT_STREAM]);
    if (base_vertex_index != 0) {
      address += STREAM_STRIDE(vs[VS_SLOT(slot) + VS_SLOT_STREAM]) * base_vertex_index;
    }
    p[0] = METHOD(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + slot * 4, 1);
    p[1] = address;
    p += 2;
  }
  compat_d3d_push_end(p);
}

#define DRAW_ARRAYS_BATCH 0x100
#define STATE_FLAG_IN_BEGIN_END 0x800
#define STATE_FLAG_FENCE_PENDING 0x1000

static void check_begin_not_nested(unsigned long where, unsigned long caller);

/* 0x1ecef0 */
void __stdcall D3DDevice_DrawVertices(uint32_t primitive_type, uint32_t start_vertex,
                                      uint32_t vertex_count)
{
  unsigned long batches;
  unsigned long i;
  uint32_t *p;

  check_begin_not_nested(1, (unsigned long)__builtin_return_address(0));
  set_state_vb(0);
  batches = ((vertex_count - 1) >> 8) + 1;
  p = compat_d3d_push_begin(batches + 5);
  p[0] = METHOD(NV097_SET_BEGIN_END, 1);
  p[1] = primitive_type;
  p[2] = METHOD_NONINC(NV097_DRAW_ARRAYS, batches);
  p += 3;
  if (vertex_count > DRAW_ARRAYS_BATCH) {
    for (i = ((vertex_count - 0x101) >> 8) + 1; i != 0; i--) {
      *p++ = start_vertex | 0xff000000ul;
      start_vertex += DRAW_ARRAYS_BATCH;
      vertex_count -= DRAW_ARRAYS_BATCH;
    }
  }
  p[0] = ((vertex_count - 1) << 24) | start_vertex;
  p[1] = METHOD(NV097_SET_BEGIN_END, 1);
  p[2] = 0;
  compat_d3d_push_end(p + 3);
}

#define INDEX_CHUNK_DWORDS 0x1ff
#define INDEX_CHUNK_INDICES 0x3fe
#define INDEX_MIN_CHUNK 0x3e

static uint32_t *push_index_pairs(uint32_t *p, const unsigned long **indices, unsigned long dwords)
{
  unsigned long i;

  p[0] = METHOD_NONINC(NV097_ARRAY_ELEMENT16, dwords);
  for (i = 0; i < dwords; i++) {
    p[1 + i] = (*indices)[i];
  }
  *indices += dwords;
  return p + 1 + dwords;
}

/* 0x1ecf90. Indices go inline, two per dword. The first chunk aligns the
 * push pointer to 32 bytes; the middle chunks are 0x1ff dwords (copied with
 * MMX non-temporal stores in the original); an odd last index goes through
 * NV097_ARRAY_ELEMENT32. */
void __stdcall D3DDevice_DrawIndexedVertices(uint32_t primitive_type, uint32_t vertex_count,
                                             const void *index_data)
{
  const unsigned long *indices;
  unsigned long count;
  unsigned long align;
  unsigned long chunk;
  unsigned long remainder;
  uint32_t *p;

  check_begin_not_nested(2, (unsigned long)__builtin_return_address(0));
  set_state_vb(*device_field(DEVICE_BASE_VERTEX_INDEX));
  *device_field(DEVICE_STATE_FLAGS) |= STATE_FLAG_IN_BEGIN_END;
  indices = (const unsigned long *)index_data;
  count = vertex_count;
  p = compat_d3d_push_begin(0x209);
  p[0] = METHOD(NV097_SET_BEGIN_END, 1);
  p[1] = primitive_type;
  p += 2;
  align = ((0 - (unsigned long)p) >> 2) & 7;
  if (count >= align * 2 + INDEX_MIN_CHUNK) {
    if (align != 0) {
      p = push_index_pairs(p, &indices, align - 1);
      count -= (align - 1) * 2;
    }
    remainder = count % INDEX_CHUNK_INDICES;
    if (remainder >= INDEX_MIN_CHUNK) {
      chunk = (((remainder >> 1) + 1) & ~0xful) - 1;
    } else {
      chunk = INDEX_CHUNK_DWORDS;
    }
    do {
      p = push_index_pairs(p, &indices, chunk);
      count -= chunk * 2;
      compat_d3d_push_end(p);
      p = compat_d3d_push_begin(0x204);
      chunk = INDEX_CHUNK_DWORDS;
    } while (count >= INDEX_CHUNK_INDICES);
  }
  p = push_index_pairs(p, &indices, count >> 1);
  if (count & 1) {
    p[0] = METHOD(NV097_ARRAY_ELEMENT32, 1);
    p[1] = *(const unsigned short *)indices;
    p += 2;
  }
  p[0] = METHOD(NV097_SET_BEGIN_END, 1);
  p[1] = 0;
  compat_d3d_push_end(p + 2);
  if (*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_FENCE_PENDING) {
    /* 0x1ed1df: SetFence(1) (0x1ef9e0), not transcribed. */
    halo_standalone_missing("D3D fence after draw (0x1ef9e0)");
  }
  *device_field(DEVICE_STATE_FLAGS) &= ~(unsigned long)(STATE_FLAG_IN_BEGIN_END |
                                                       STATE_FLAG_FENCE_PENDING);
}

/* ------------------------------------------------------------------------
 * Pixel shaders.
 */

#define DEVICE_PIXEL_SHADER_PROGRAM_HANDLE 0x464
#define D3D_PS_REGISTER_COUNT 0x39
#define BUMP_ENV_DWORDS 6
/* The initial combiner input mode of a NULL pixel shader (0x1ec61a). */
#define OTHER_STAGE_INPUT_DEFAULT 0x210000

/* D3DPIXELSHADERDEF byte offsets. */
#define PSDEF_ALPHA_INPUTS 0x00
#define PSDEF_FINAL_INPUTS_0 0x20
#define PSDEF_FINAL_INPUTS_1 0x24
#define PSDEF_CONSTANT_0 0x28
#define PSDEF_CLIP_PLANE_MODE 0xa8
#define PSDEF_FINAL_CONSTANT_0 0xac
#define PSDEF_FINAL_CONSTANT_1 0xb0
#define PSDEF_RGB_OUTPUTS 0xb4
#define PSDEF_TEXTURE_MODES 0xd8
#define PSDEF_DOT_MAPPING 0xdc
#define PSDEF_INPUT_TEXTURE 0xe0
#define PSDEF_C0_MAPPING 0xec
#define PSDEF_DOTMAP_FLAG 0x100

/* 0x1e9d40 (ecx = device, edx = push pointer): the bump-environment matrix,
 * scale and offset of texture units 1-3. The fixed-function pipe takes a
 * unit's values from the stage before it, a pixel shader from its own. */
static uint32_t *push_bump_env(uint32_t *p)
{
  unsigned long stage_offset;
  unsigned long unit;
  unsigned long i;

  stage_offset = *device_field(DEVICE_PIXEL_SHADER) != 0 ? 1 : 0;
  for (unit = 1; unit < TEXTURE_STAGE_COUNT; unit++) {
    p[0] = METHOD(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + unit * 0x40, BUMP_ENV_DWORDS);
    for (i = 0; i < BUMP_ENV_DWORDS; i++) {
      p[1 + i] = TEXTURE_STATE(unit - 1 + stage_offset, D3DTSS_BUMPENVMAT00 + i);
    }
    p += 1 + BUMP_ENV_DWORDS;
  }
  return p;
}

/* 0x1ec5c0. Its kb decl is a void(void) placeholder and the game only
 * reaches it through SetPixelShaderProgram, so it is local here. */
static void set_pixel_shader(unsigned long handle)
{
  const unsigned long *def;
  unsigned long old;
  unsigned long i;
  uint32_t *p;

  old = *device_field(DEVICE_PIXEL_SHADER);
  *device_field(DEVICE_PIXEL_SHADER) = handle;
  if (handle == 0) {
    *device_field(DEVICE_DIRTY) |= DIRTY_TEXTURE_MODES | DIRTY_COMBINERS;
    if (*device_field(DEVICE_PS_COMBINER_COUNT) != 0) {
      *device_field(DEVICE_DIRTY) |= DIRTY_FOG;
    }
    D3DDevice_SetRenderState_TextureFactor(D3D_RENDER_STATE[D3DRS_TEXTUREFACTOR]);
    p = compat_d3d_push_begin(0x17);
    p = push_bump_env(p);
    p[0] = METHOD(NV097_SET_SHADER_OTHER_STAGE_INPUT, 1);
    p[1] = OTHER_STAGE_INPUT_DEFAULT;
    compat_d3d_push_end(p + 2);
    return;
  }
  def = (const unsigned long *)((const unsigned long *)handle)[2];
  *device_field(DEVICE_PS_COMBINER_COUNT) =
    def[PSDEF_FINAL_INPUTS_1 / 4] | def[PSDEF_FINAL_INPUTS_0 / 4];
  *device_field(DEVICE_PS_DOTMAP_TEXTURE_MODES) = def[PSDEF_C0_MAPPING / 4] & PSDEF_DOTMAP_FLAG;
  *device_field(DEVICE_PS_TEXTURE_MODES) = def[PSDEF_TEXTURE_MODES / 4];
  *device_field(DEVICE_DIRTY) |= DIRTY_TEXTURE_MODES;
  p = compat_d3d_push_begin(0x54);
  if (old == 0) {
    p = push_bump_env(p);
  }
  p[0] = METHOD(NV097_SET_COMBINER_ALPHA_ICW, 8);
  for (i = 0; i < 8; i++) {
    p[1 + i] = def[PSDEF_ALPHA_INPUTS / 4 + i];
  }
  p[9] = METHOD(NV097_SET_COMBINER_FACTOR0, 0x20);
  for (i = 0; i < 0x20; i++) {
    p[10 + i] = def[PSDEF_CONSTANT_0 / 4 + i];
  }
  p[0x2a] = METHOD(NV097_SET_SHADER_CLIP_PLANE_MODE, 1);
  p[0x2b] = def[PSDEF_CLIP_PLANE_MODE / 4];
  p[0x2c] = METHOD(NV097_SET_SPECULAR_FOG_FACTOR, 2);
  p[0x2d] = def[PSDEF_FINAL_CONSTANT_0 / 4];
  p[0x2e] = def[PSDEF_FINAL_CONSTANT_1 / 4];
  p[0x2f] = METHOD(NV097_SET_COMBINER_COLOR_OCW, 9);
  for (i = 0; i < 9; i++) {
    p[0x30 + i] = def[PSDEF_RGB_OUTPUTS / 4 + i];
  }
  p[0x39] = METHOD(NV097_SET_DOT_RGBMAPPING, 2);
  p[0x3a] = def[PSDEF_DOT_MAPPING / 4];
  p[0x3b] = def[PSDEF_INPUT_TEXTURE / 4];
  p += 0x3c;
  if (*device_field(DEVICE_PS_COMBINER_COUNT) != 0) {
    p[0] = METHOD(NV097_SET_COMBINER_SPECULAR_FOG_CW0, 2);
    p[1] = def[PSDEF_FINAL_INPUTS_0 / 4];
    p[2] = def[PSDEF_FINAL_INPUTS_1 / 4];
    p += 3;
  }
  compat_d3d_push_end(p);
  if (!(*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_NO_SHADOW)) {
    for (i = 0; i < D3D_PS_REGISTER_COUNT; i++) {
      D3D_RENDER_STATE[i] = def[i];
    }
    D3D_RENDER_STATE[D3DRS_PSTEXTUREMODES] = def[PSDEF_TEXTURE_MODES / 4];
  }
}

/* 0x1ec120: the handle is a {1, 0, program} block at device+0x464. */
void __stdcall D3DDevice_SetPixelShaderProgram(void *program)
{
  unsigned long *handle;

  if (program) {
    handle = device_field(DEVICE_PIXEL_SHADER_PROGRAM_HANDLE);
    handle[0] = 1;
    handle[1] = 0;
    handle[2] = (unsigned long)program;
    set_pixel_shader((unsigned long)handle);
  } else {
    set_pixel_shader(0);
  }
}

/* ------------------------------------------------------------------------
 * Textures.
 */

#define TEXTURE_FORMAT_MODE_MASK 0xf4
#define D3DCOMMON_PALETTESIZE_SHIFT 28

/* 0x1e8700 */
void __stdcall D3DDevice_SetTexture(uint32_t stage, void *texture)
{
  unsigned long *old;
  unsigned long *t;
  unsigned long format_bits;
  unsigned long width;
  unsigned long height;
  uint32_t *p;

  TRACE_CHECK_TEXTURE(stage, texture);
  old = device_texture(stage);
  if (old) {
    resource_release_device_ref(old, 1);
  }
  t = (unsigned long *)texture;
  *device_field(DEVICE_TEXTURES + stage * 4) = (unsigned long)t;
  if (!t) {
    p = compat_d3d_push_begin(2);
    p[0] = METHOD(NV097_SET_TEXTURE_CONTROL0 + stage * 0x40, 1);
    p[1] = 0;
    compat_d3d_push_end(p + 2);
    TRACE_CONTROL0(stage, 0);
    *device_field(DEVICE_TEXTURE_FORMAT_BITS + stage * 4) = 0xfffffffful;
    *device_field(DEVICE_DIRTY) |= DIRTY_TEXTURE_MODES | DIRTY_COMBINERS;
    return;
  }
  resource_add_device_ref(t);
  p = compat_d3d_push_begin(9);
  p[0] = METHOD(NV097_SET_TEXTURE_OFFSET + stage * 0x40, 2);
  p[1] = t[RESOURCE_DATA];
  p[2] = t[RESOURCE_FORMAT];
  TRACE_FORMAT(stage, t[RESOURCE_FORMAT]);
  p += 3;
  if (t[RESOURCE_SIZE] != 0) {
    width = (t[RESOURCE_SIZE] & SURFACE_SIZE_DIMENSION_MASK) + 1;
    height = ((t[RESOURCE_SIZE] >> 12) & SURFACE_SIZE_DIMENSION_MASK) + 1;
    /* 0x1e77d0 with a non-zero Size: the pitch field. */
    p[0] = METHOD(NV097_SET_TEXTURE_CONTROL1 + stage * 0x40, 1);
    p[1] = (((t[RESOURCE_SIZE] >> SURFACE_SIZE_PITCH_SHIFT) + 1) * SURFACE_PITCH_UNIT) << 16;
    p[2] = METHOD(NV097_SET_TEXTURE_IMAGE_RECT + stage * 0x40, 1);
    p[3] = (width << 16) | height;
    p += 4;
  }
  format_bits = t[RESOURCE_FORMAT] & TEXTURE_FORMAT_MODE_MASK;
  if (*device_field(DEVICE_TEXTURE_FORMAT_BITS + stage * 4) != format_bits) {
    *device_field(DEVICE_TEXTURE_FORMAT_BITS + stage * 4) = format_bits;
    p[0] = METHOD(NV097_SET_TEXTURE_CONTROL0 + stage * 0x40, 1);
    p[1] = *device_field(DEVICE_TEXTURE_CONTROL0 + stage * 4);
    TRACE_CONTROL0(stage, p[1]);
    p += 2;
    *device_field(DEVICE_DIRTY) |= DIRTY_TEXTURE_MODES;
    if (!old) {
      *device_field(DEVICE_DIRTY) |= DIRTY_COMBINERS;
    }
  }
  compat_d3d_push_end(p);
}

/* 0x1e88a0 */
void __stdcall D3DDevice_SetPalette(uint32_t stage, void *palette)
{
  unsigned long *old;
  unsigned long *pal;
  uint32_t *p;

  old = (unsigned long *)*device_field(DEVICE_PALETTES + stage * 4);
  if (old) {
    resource_release_device_ref(old, 1);
  }
  pal = (unsigned long *)palette;
  *device_field(DEVICE_PALETTES + stage * 4) = (unsigned long)pal;
  if (!pal) {
    return;
  }
  resource_add_device_ref(pal);
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_TEXTURE_PALETTE + stage * 0x40, 1);
  p[1] = pal[RESOURCE_DATA] | (pal[RESOURCE_COMMON] >> D3DCOMMON_PALETTESIZE_SHIFT);
  compat_d3d_push_end(p + 2);
}

/* 0x1fb3d8: dirty bits of the deferred texture-stage states 0..0x15. */
#define D3DTSS_DEFERRED_COUNT 0x16
static const unsigned long d3d_deferred_texture_state_dirty[D3DTSS_DEFERRED_COUNT] = {
  0x418, 0x008, 0x008, 0x008, 0x008, 0x008, 0x008, 0x008, /* 0x00 */
  0x008, 0x004, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, /* 0x08 */
  0x010, 0x010, 0x010, 0x010, 0x010, 0x010,               /* 0x10 */
};

/* 0x1e9410 (ecx = stage, edx = state). The original indexes the dirty table
 * unchecked; only the deferred states 0..0x15 are defined. */
void D3DDevice_SetTextureStageState(uint32_t stage, uint32_t state, uint32_t value)
{
  if (state < D3DTSS_DEFERRED_COUNT) {
    *device_field(DEVICE_DIRTY) |= d3d_deferred_texture_state_dirty[state];
  }
  TEXTURE_STATE(stage, state) = value;
}

/* ------------------------------------------------------------------------
 * Immediate mode.
 */

/* The primitive between Begin and End (0x1ed45c). */
#define IMMEDIATE_PRIMITIVE ((unsigned long *)0x1fbb04)

/* Not XDK behavior: bring-up diagnostic. A Begin (immediate or a draw) issued
 * while a Begin/End block is still open is dropped by the NV2A, so the open
 * block's inline vertices meet the next draw's elements. Record who opened
 * and who re-entered, read back over XBDM. */
unsigned long compat_d3d_nested_begin_count;
unsigned long compat_d3d_nested_begin_opener;
unsigned long compat_d3d_nested_begin_caller;
unsigned long compat_d3d_nested_begin_where;
static unsigned long d3d_begin_opener;

/* xemu aborts as soon as the GPU reaches the bad draw, and the XBDM
 * notification carrying the DbgPrint above is sent asynchronously. Hold the
 * push back for two seconds so the report reaches the host first. */
static void diagnostic_flush(void)
{
  LARGE_INTEGER delay;

  delay.QuadPart = -20000000LL;
  KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static void check_begin_not_nested(unsigned long where, unsigned long caller)
{
  if (*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_IN_BEGIN_END) {
    if (compat_d3d_nested_begin_count++ == 0) {
      compat_d3d_nested_begin_opener = d3d_begin_opener;
      compat_d3d_nested_begin_caller = caller;
      compat_d3d_nested_begin_where = where;
      DbgPrint("[d3d] nested Begin: where=%lu opener=%08lx caller=%08lx\n", where,
               d3d_begin_opener, caller);
      diagnostic_flush();
    }
  }
  d3d_begin_opener = caller;
}

/* Not XDK behavior: bring-up diagnostic. Position data (register 0, or -1
 * for SET_VERTEX4F) outside a Begin/End block: xemu appends it to the inline
 * vertex buffer anyway, and the next DrawVertices then trips its
 * inline_buffer_length assert. Report the first few callers. */
unsigned long compat_d3d_stray_vertex_count;

static void check_vertex_inside_begin(unsigned long reg, unsigned long caller)
{
  if ((reg == 0 || reg == 0xfffffffful) &&
      !(*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_IN_BEGIN_END)) {
    if (compat_d3d_stray_vertex_count++ < 4) {
      DbgPrint("[d3d] vertex data outside Begin/End: reg=%ld caller=%08lx\n", (long)reg,
               caller);
      diagnostic_flush();
    }
  }
}

/* 0x1ed450: lazy state only; no vertex arrays. */
void __stdcall D3DDevice_Begin(uint32_t primitive_type)
{
  uint32_t *p;

  check_begin_not_nested(0, (unsigned long)__builtin_return_address(0));
  *IMMEDIATE_PRIMITIVE = primitive_type;
  lazy_set_state();
  TRACE_DRAW("begin");
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_BEGIN_END, 1);
  p[1] = primitive_type;
  compat_d3d_push_end(p + 2);
  *device_field(DEVICE_STATE_FLAGS) |= STATE_FLAG_IN_BEGIN_END;
}

/* 0x1ed490 */
void D3DDevice_End(void)
{
  uint32_t *p;

  *IMMEDIATE_PRIMITIVE = 0;
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_BEGIN_END, 1);
  p[1] = 0;
  compat_d3d_push_end(p + 2);
  if (*device_field(DEVICE_STATE_FLAGS) & STATE_FLAG_FENCE_PENDING) {
    /* 0x1ed4c3: SetFence(1) (0x1ef9e0), not transcribed. */
    halo_standalone_missing("D3D fence after draw (0x1ef9e0)");
  }
  *device_field(DEVICE_STATE_FLAGS) &= ~(unsigned long)(STATE_FLAG_IN_BEGIN_END |
                                                       STATE_FLAG_FENCE_PENDING);
}

/* 0x1ed280 */
void __stdcall D3DDevice_SetVertexData2f(uint32_t reg, float a, float b)
{
  uint32_t *p;

  check_vertex_inside_begin(reg, (unsigned long)__builtin_return_address(0));

  p = compat_d3d_push_begin(3);
  p[0] = METHOD(NV097_SET_VERTEX_DATA2F_M + reg * 8, 2);
  p[1] = float_bits(a);
  p[2] = float_bits(b);
  compat_d3d_push_end(p + 3);
}

/* 0x1ed2c0: register -1 is the vertex itself (NV097_SET_VERTEX4F). */
void __stdcall D3DDevice_SetVertexData4f(uint32_t reg, float a, float b, float c, float d)
{
  uint32_t *p;

  check_vertex_inside_begin(reg, (unsigned long)__builtin_return_address(0));

  p = compat_d3d_push_begin(5);
  if (reg == 0xfffffffful) {
    p[0] = METHOD(NV097_SET_VERTEX4F, 4);
  } else {
    p[0] = METHOD(NV097_SET_VERTEX_DATA4F_M + reg * 16, 4);
  }
  p[1] = float_bits(a);
  p[2] = float_bits(b);
  p[3] = float_bits(c);
  p[4] = float_bits(d);
  compat_d3d_push_end(p + 5);
}

/* 0x1ed320 */
void __stdcall D3DDevice_SetVertexData2s(unsigned int reg, int a, int b)
{
  uint32_t *p;

  check_vertex_inside_begin(reg, (unsigned long)__builtin_return_address(0));

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_VERTEX_DATA2S + reg * 4, 1);
  p[1] = ((unsigned long)(long)(short)b << 16) | ((unsigned long)a & 0xffff);
  compat_d3d_push_end(p + 2);
}

/* 0x1ed3b0 */
void __stdcall D3DDevice_SetVertexData4ub(uint32_t reg, uint32_t r, uint32_t g, uint32_t b,
                                          uint32_t a)
{
  uint32_t *p;

  check_vertex_inside_begin(reg, (unsigned long)__builtin_return_address(0));

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_VERTEX_DATA4UB + reg * 4, 1);
  p[1] = ((a & 0xff) << 24) | ((b & 0xff) << 16) | ((g & 0xff) << 8) | (r & 0xff);
  compat_d3d_push_end(p + 2);
}

/* 0x1ed400: D3DCOLOR (ARGB) to the RGBA byte order of the method. */
void __stdcall D3DDevice_SetVertexDataColor(unsigned int reg, unsigned int color)
{
  uint32_t *p;

  check_vertex_inside_begin(reg, (unsigned long)__builtin_return_address(0));

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_VERTEX_DATA4UB + reg * 4, 1);
  p[1] = (color & 0xff00ff00ul) | ((color & 0xff) << 16) | ((color >> 16) & 0xff);
  compat_d3d_push_end(p + 2);
}

/* ------------------------------------------------------------------------
 * Device initialization.
 */

/* 0x1e9cb0 (D3DDevice_SetTextureStageStateNotInline, whose kb decl is a
 * void(void) placeholder). */
static void set_texture_stage_state_not_inline(unsigned long stage, unsigned long state,
                                               unsigned long value)
{
  if (state < D3DTSS_DEFERRED_COUNT) {
    TEXTURE_STATE(stage, state) = value;
    *device_field(DEVICE_DIRTY) |= d3d_deferred_texture_state_dirty[state];
  } else if (state == D3DTSS_TEXCOORDINDEX) {
    D3DDevice_SetTextureState_TexCoordIndex(stage, value);
  } else if (state == 0x1d) { /* D3DTSS_BORDERCOLOR */
    D3DDevice_SetTextureState_BorderColor(stage, value);
  } else if (state == 0x1e) { /* D3DTSS_COLORKEYCOLOR */
    D3DDevice_SetTextureState_ColorKeyColor(stage, value);
  } else if (state <= 0x1b) { /* D3DTSS_BUMPENVMAT00..BUMPENVLOFFSET */
    D3DDevice_SetTextureState_BumpEnv(stage, state, value);
  }
}

/* 0x1fa880: default values of render states 0x39..0x8f. */
#define DEFAULT_RENDER_STATE_FIRST 0x39
#define DEFAULT_RENDER_STATE_COUNT 0x57
#define DEFAULT_RENDER_STATE_SKIPPED 0x86 /* D3DRS_MULTISAMPLETYPE */
static const unsigned long default_render_states[DEFAULT_RENDER_STATE_COUNT] = {
  0x00000203, 0x00000207, 0x00000000, 0x00000000, 0x00000000, 0x00000001, /* 0x39 */
  0x00000000, 0x00000001, 0x00000000, 0x00001d01, 0x01010101, 0x00001e00, /* 0x3f */
  0x00001e00, 0x00000207, 0x00000000, 0xffffffff, 0xffffffff, 0x00008006, /* 0x45 */
  0x00000000, 0x00000004, 0x00000000, 0x00000000, 0x00000000, 0x00000000, /* 0x4b */
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x3f800000, /* 0x51 */
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, /* 0x57 */
  0x00000000, 0x00000001, 0x00000001, 0x00000002, 0x00000001, 0x00000000, /* 0x5d */
  0x00000000, 0x00000002, 0x00000001, 0x00000000, 0x00000000, 0x00000000, /* 0x63 */
  0x00000000, 0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, /* 0x69 */
  0x00000000, 0x00000000, 0x42800000, 0x00000000, 0x3f800000, 0x00000000, /* 0x6f */
  0x00000000, 0x00000000, 0x00001b02, 0x00001b02, 0x00000000, 0x00000000, /* 0x75 */
  0x00000002, 0x00000000, 0x00001e00, 0x00000900, 0x00000901, 0xffffffff, /* 0x7b */
  0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xffffffff, 0xdeadbeef, /* 0x81 */
  0x00000200, 0x3f800000, 0x00000001, 0x00000000, 0x00000001, 0x00000001, /* 0x87 */
  0x00000000, 0x00000000, 0x00000000, /* 0x8d */
};

/* 0x1fa860: default values of texture-stage states 0..0x1f. */
#define DEFAULT_TEXTURE_STATE_COUNT 0x20
static const unsigned char default_texture_states[DEFAULT_TEXTURE_STATE_COUNT] = {
  0x01, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02, 0x01, /* 0x00 */
  0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, /* 0x08 */
  0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, /* 0x10 */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 0x18 */
};

/* 0x1f3000: the device's default state, starting with identity transforms.
 * D3DRS_ZENABLE follows the depth buffer: the XDK tests its auto
 * depth-stencil (device+0x2198), the compat device the current depth buffer
 * at device+0x410. */
static void set_default_state(void)
{
  unsigned long i;
  unsigned long stage;
  unsigned long state;

  for (i = 0; i < D3DTS_COUNT; i++) {
    D3DDevice_SetTransform(i, (void *)compat_d3d_identity_matrix);
  }
  for (i = 0; i < DEFAULT_RENDER_STATE_COUNT; i++) {
    if (DEFAULT_RENDER_STATE_FIRST + i == DEFAULT_RENDER_STATE_SKIPPED) {
      continue;
    }
    D3DDevice_SetRenderStateNotInline(DEFAULT_RENDER_STATE_FIRST + i, default_render_states[i]);
  }
  D3DDevice_SetRenderStateNotInline(D3DRS_ZENABLE, *device_field(DEVICE_DEPTH_STENCIL) != 0);
  for (stage = 0; stage < TEXTURE_STAGE_COUNT; stage++) {
    for (state = 0; state < DEFAULT_TEXTURE_STATE_COUNT; state++) {
      set_texture_stage_state_not_inline(stage, state, default_texture_states[state]);
    }
    set_texture_stage_state_not_inline(stage, D3DTSS_TEXCOORDINDEX, stage);
  }
  set_texture_stage_state_not_inline(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  set_texture_stage_state_not_inline(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
}

/* The rest of the XDK's device creation (0x1f39c4..0x1f3a11): the FVF shader
 * object as the current shader, SetVertexShader(D3DFVF_XYZ), then the
 * default state. Called from Direct3D_CreateDevice after
 * compat_d3d_state_initialize; its SetRenderTarget and Clear(depth+stencil)
 * steps are the compat device's own. */
void compat_d3d_draw_initialize(void)
{
  compat_d3d_trace_mark("draw_init");
  *device_field(DEVICE_VERTEX_SHADER) = (unsigned long)FVF_VERTEX_SHADER;
  D3DDevice_SetVertexShader(2);
  set_default_state();
  compat_d3d_trace_mark("draw_init_done");
}
