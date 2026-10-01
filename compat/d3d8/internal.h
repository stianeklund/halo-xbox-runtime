/*
 * Standalone compat: what the Direct3D 8 device (device.c), its state setters
 * (state.c) and its draw calls (draw.c) share: the XDK device layout, the
 * state mirrors and the push buffer.
 */
#ifndef HALO_COMPAT_D3D8_INTERNAL_H
#define HALO_COMPAT_D3D8_INTERNAL_H

#include "nv_regs.h"
#include "xbox.h"

/* nxdk pdclib. decl.h carries kb placeholders for some of these names, so
 * bind compat-local names to the pdclib symbols. */
void *__cdecl compat_malloc(size_t size) __asm__("_malloc");
void __cdecl compat_free(void *memory) __asm__("_free");
void *__cdecl compat_memcpy(void *dst, const void *src,
                            size_t size) __asm__("_memcpy");
void halo_standalone_missing(const char *name);

uint32_t *pb_begin(void);
void pb_end(uint32_t *end);
void pb_reset(void);

#define D3D_OK 0
#define E_OUTOFMEMORY 0x8007000e

/* Push-buffer method header: dword count << 18 | method. */
#define METHOD(method, count) (((unsigned long)(count) << 18) | (method))

/* The XDK's static device and the state mirrors next to it, which the game
 * reads back. */
#define D3D_DEVICE ((unsigned long *)0x1fbb10)
#define D3D_RENDER_STATE ((unsigned long *)0x1fb698)
#define D3D_TEXTURE_STATE ((unsigned long *)0x1fb498)
#define TEXTURE_STATE_STRIDE 32

#define device_field(offset)                                                   \
  ((unsigned long *)((unsigned char *)D3D_DEVICE + (offset)))

#define DEVICE_DIRTY 0x08
#define DEVICE_STATE_FLAGS 0x0c
#define DEVICE_CURRENT_TIME 0x1c
#define DEVICE_RENDER_TARGET 0x40c
#define DEVICE_DEPTH_STENCIL 0x410
#define DEVICE_PIXEL_SHADER 0x414
#define DEVICE_PS_TEXTURE_MODES 0x420
#define DEVICE_Z_SCALE_W 0x4f4
#define DEVICE_Z_SCALE 0x4f8
#define DEVICE_SUPER_SAMPLE_SCALE_X 0x500
#define DEVICE_SUPER_SAMPLE_SCALE_Y 0x504
#define DEVICE_SUPER_SAMPLE_SCALE 0x508
#define DEVICE_DERIVED_PROJECTION_MATRIX 0x5a0
#define DEVICE_VIEWPORT 0xb00
#define DEVICE_BUFFER_SURFACES 0x2150

/* DEVICE_DIRTY bits: what the next draw reloads. */
#define DIRTY_TRANSFORMS 0x2
#define DIRTY_TEXTURE_TRANSFORM 0x4
#define DIRTY_LIGHTING 0x20

/* Xbox D3DRS_* indices of the render-state mirror. */
#define D3DRS_PSTEXTUREMODES 0x74
#define D3DRS_VERTEXBLEND 0x75
#define D3DRS_TWOSIDEDLIGHTING 0x79
#define D3DRS_NORMALIZENORMALS 0x7a
#define D3DRS_ZENABLE 0x7b
#define D3DRS_STENCILENABLE 0x7c
#define D3DRS_TEXTUREFACTOR 0x80
#define D3DZB_USEW 2

#define D3DTSS_BUMPENVMAT00 0x16
#define D3DTSS_TEXCOORDINDEX 0x1c

/* D3DTS_* types: view, projection, texture 0-3, world 0-3. */
#define D3DTS_COUNT 10

#define D3DCOMMON_TYPE_MASK 0x00070000ul
#define D3DCOMMON_TYPE_SURFACE 0x00050000ul
#define D3DFMT_LIN_F24S8 0x2f

/* Linear surface Size: width-1 | height-1 << 12 | pitch/64-1 << 24. */
#define SURFACE_SIZE_DIMENSION_MASK 0xfff
#define SURFACE_SIZE_HEIGHT_SHIFT 12
#define SURFACE_SIZE_PITCH_SHIFT 24
#define SURFACE_PITCH_UNIT 64

typedef union {
  unsigned long u;
  float f;
} dword_float;

/* HALO_STANDALONE_GPU_STATE=OFF (HALO_STANDALONE_NO_GPU_STATE, set for
 * state.c and draw.c) keeps every mirror and device-field store but drops
 * their pushes, to bisect GPU faults. */
int compat_d3d_pbkit_ready(void);
#ifdef HALO_STANDALONE_NO_GPU_STATE
#define gpu_state_ready() 0
#else
#define gpu_state_ready() compat_d3d_pbkit_ready()
#endif

/* Push buffer (draw.c). Every push reserves its dword count up front and
 * hands the end pointer to compat_d3d_push_end, which reports a push that
 * ran past its reservation. compat_d3d_push_reserve always takes room in
 * pbkit's buffer; compat_d3d_push_begin does so only once the GPU state is
 * up and otherwise hands out scratch space whose contents are dropped. */
uint32_t *compat_d3d_push_reserve(unsigned long dwords);
uint32_t *compat_d3d_push_begin(unsigned long dwords);
void compat_d3d_push_end(uint32_t *end);
unsigned long compat_d3d_push_buffer_bytes(void);
void compat_d3d_push_kick(void);
void compat_d3d_push_drain(void);
void compat_d3d_push_rewind(void);
void compat_d3d_push_viewport(void);
void compat_d3d_push_passthrough_program(void);
/* FUN_001ef590: METHOD(method, 16), then a 4x4 matrix transposed. */
uint32_t *compat_d3d_push_transposed_matrix(uint32_t *p, unsigned long method,
                                            const float *matrix);

/* 0x1f9f48 */
extern const float compat_d3d_identity_matrix[16];
void compat_d3d_matrix4x4_multiply(float *out, const float *left,
                                   const float *right);

/* Surface dimensions (device.c): from Size when linear, else from the log2
 * sizes in Format. */
unsigned long compat_d3d_surface_width(const unsigned long *surface);
unsigned long compat_d3d_surface_height(const unsigned long *surface);

void compat_d3d_state_initialize(void);
void compat_d3d_draw_initialize(void);
void compat_d3d_set_depth_scale(const void *depth_stencil);
void compat_d3d_trace_mark(const char *what);

#endif
