/*
 * Standalone compat: Direct3D 8 (Xbox) device and resource layer.
 *
 * A pbkit-backed device that honors the Xbox resource conventions the game
 * (and the D3D routines already lifted into
 * src/halo/rasterizer/xbox/d3d_resource.c) depend on: creation, targets,
 * Clear, Present and the vblank thread. Render and texture states are pushed
 * by state.c, shaders, streams and draw calls by draw.c.
 *
 * Xbox resource headers (all little-endian dwords):
 *   +0x00 Common  refcount (bits 0-15) | type (bits 16-18) | flags
 *   +0x04 Data    physical address of the bits (vertex/index: see below)
 *   +0x08 Lock    GPU fence of the last use
 *   +0x0c Format  pixel containers: dma | cube | dimension | format | levels
 *                 | log2 u/v/p sizes
 *   +0x10 Size    linear pixel containers: width-1 | height-1 | pitch/64-1
 *   +0x14 Parent  surfaces: owning texture
 * Every header this file creates is a compat_resource, which appends the
 * allocation to free; the game also builds headers of its own
 * (rasterizer_xbox.c), which D3D_DestroyResource must leave alone.
 */
#include "internal.h"

void *__cdecl compat_calloc(size_t count, size_t size) __asm__("_calloc");
void *__cdecl compat_memset(void *dst, int value,
                            size_t size) __asm__("_memset");
void XVideoWaitForVBlank(void);
int XVideoSetMode(int width, int height, int bpp, int refresh);
unsigned char *XVideoGetFB(void);
void XVideoSetFB(unsigned char *fb);
void XVideoFlushFB(void);
void XVideoSetVideoEnable(int enable);
int pb_init(void);
unsigned long pb_wait_for_vbl(void);
void pb_wait_until_gr_not_busy(void);
int pb_busy(void);
void pb_size(unsigned long size);
void pb_set_surface_allocation(int enable);
uint32_t *pb_push1(uint32_t *p, unsigned long command, unsigned long param1);
uint32_t *pb_push2(uint32_t *p, unsigned long command, unsigned long param1,
                   unsigned long param2);
typedef unsigned long(__stdcall *compat_thread_routine)(void *parameter);
void *__cdecl
compat_CreateThread(void *attributes, size_t stack_size,
                    compat_thread_routine start, void *parameter,
                    unsigned long flags,
                    unsigned long *thread_id) __asm__("_CreateThread");
int __cdecl
compat_SetThreadPriority(void *thread,
                         int priority) __asm__("_SetThreadPriority");

#define D3DCOMMON_TYPE_VERTEXBUFFER 0x00000000
#define D3DCOMMON_TYPE_INDEXBUFFER 0x00010000
#define D3DCOMMON_TYPE_PALETTE 0x00030000
#define D3DCOMMON_TYPE_TEXTURE 0x00040000
#define D3DCOMMON_VIDEOMEMORY 0x00800000
#define D3DCOMMON_D3DCREATED 0x01000000

#define D3DFORMAT_DMACHANNEL_A 0x00000001
#define D3DFORMAT_CUBEMAP 0x00000004
/* NV097 texture format bit 3, BORDER_SOURCE_COLOR. 0x1f4630 (the header
 * builder behind CreateTexture/Volume/Cube, 0x1eddd0) ORs it into every
 * Format at 0x1f47ea; 0x1eddd0 clears it again (0x1ede15..0x1ede1f) only
 * for D3DUSAGE_BORDERSOURCE_TEXTURE. With the bit clear the GPU treats the
 * texture as carrying its own border texels and remaps the coordinates. */
#define D3DFORMAT_BORDERSOURCE_COLOR 0x00000008
#define D3DUSAGE_BORDERSOURCE_TEXTURE 0x00010000
#define D3DFORMAT_DIMENSION_SHIFT 4
#define D3DFORMAT_FORMAT_SHIFT 8
#define D3DFORMAT_MIPMAP_SHIFT 16
#define D3DFORMAT_USIZE_SHIFT 20
#define D3DFORMAT_VSIZE_SHIFT 24
#define D3DFORMAT_PSIZE_SHIFT 28

#define D3DFMT_LIN_A8R8G8B8 0x12
#define D3DFMT_LIN_D24S8 0x2e
#define D3DFMT_DXT1 0x0c
#define D3DFMT_DXT2 0x0e
#define D3DFMT_DXT4 0x0f

#define D3DRTYPE_SURFACE 1
#define D3DRTYPE_VOLUME 2
#define D3DRTYPE_TEXTURE 3
#define D3DRTYPE_VOLUMETEXTURE 4
#define D3DRTYPE_CUBETEXTURE 5

#define D3DTS_MATRIX_BYTES 0x40
/* 0x1f9de0, by D3DTS type: view (it also moves the lights), projection,
 * texture 0-3, world 0-3. */
static const unsigned long transform_dirty_bits[D3DTS_COUNT] = {
  DIRTY_TRANSFORMS | DIRTY_LIGHTING, DIRTY_TRANSFORMS,
  DIRTY_TEXTURE_TRANSFORM, DIRTY_TEXTURE_TRANSFORM,
  DIRTY_TEXTURE_TRANSFORM, DIRTY_TEXTURE_TRANSFORM,
  DIRTY_TRANSFORMS, DIRTY_TRANSFORMS, DIRTY_TRANSFORMS, DIRTY_TRANSFORMS,
};
#define D3DTS_PROJECTION 1
#define DEVICE_TRANSFORM_BASE 0x880
#define TEXTURE_ALIGNMENT 0x1000
#define CUBEFACE_ALIGNMENT 0x80
#define COMPAT_RESOURCE_COOKIE 0x48434f4d /* 'HCOM' */

/* Device fields read by the game and by lifted D3D code (D3D_g_pDevice).
 *
 * The device lives where the XDK's static one does (0x1fbb10, zero-filled
 * D3D .bss in the legacy image; D3D_g_pDevice at 0x1fe6a0 follows it), since
 * d3d_find_flipcount (0x1cf880) reads the interrupt, vblank-callback and
 * flip-count fields at those fixed addresses instead of through the pointer.
 * The 2276 D3D offsets: the callback at +0x24ec from
 * D3DDevice_SetVerticalBlankCallback (0x1e70e0), the flip count at +0x2518
 * that the vblank path copies from the swap count at +0x2b60 (0x1ef9c9). */
#define DEVICE_SIZE 0x2b90
#define DEVICE_INTERRUPTS_ENABLED 0x23ac
#define DEVICE_VBLANK_CALLBACK 0x24ec
#define DEVICE_FLIP_COUNT 0x2518
#define DEVICE_SWAP_COUNT 0x2b60
#define D3DPRESENT_INTERVAL_TWO 2
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000ul
#define THREAD_PRIORITY_TIME_CRITICAL_VALUE 15
#define DEVICE_COMPLETED_TIME_POINTER 0x3f0
/* Reference count: D3DDevice_Release (0x1e6f50) decrements it and tears the
 * device down when the last reference goes. */
#define DEVICE_REFERENCE_COUNT 0x408
#define DEVICE_RENDER_TARGET 0x40c
#define DEVICE_DEPTH_STENCIL 0x410
#define DEVICE_PROJECTION_DEPTH_OFFSET 0x4ec
#define DEVICE_PROJECTION_DEPTH_SCALE 0x4f0
#define DEVICE_PROJECTION_MATRIX 0x8c0
#define DEVICE_BUFFER_SURFACE_COUNT 3
#define DEVICE_BUFFER_SURFACE_STRIDE 0x18
#define DEVICE_BACK_BUFFER_SLOT 0
#define DEVICE_FRONT_BUFFER_SLOT 1
#define VIDEO_BITS_PER_PIXEL 32
#define VIDEO_REFRESH_DEFAULT 0

/* Xbox D3DCLEAR_* (the PC values differ). */
#define D3DCLEAR_ZBUFFER 0x01
#define D3DCLEAR_STENCIL 0x02
#define D3DCLEAR_TARGET_R 0x10
#define D3DCLEAR_TARGET_G 0x20
#define D3DCLEAR_TARGET_B 0x40
#define D3DCLEAR_TARGET_A 0x80

#define DEPTH_D16_MAX 0xffff
#define DEPTH_D24_MAX 0xffffff
#define DEPTH_STENCIL_SHIFT 8

/* pbkit's DMA object 3: 3D class over all of RAM (pb_init). Binding it as the
 * color and zeta context makes surface offsets physical addresses, as in the
 * XDK. */
#define PBKIT_DMA_ALL_RAM 3
#define NV097_CLEAR_RECT_SHIFT 16

typedef struct compat_resource {
  unsigned long common;
  unsigned long data;
  unsigned long lock;
  unsigned long format;
  unsigned long size;
  struct compat_resource *parent;
  void *allocation;
  int contiguous;
  unsigned long cookie;
} compat_resource;

typedef struct {
  int pitch;
  void *bits;
} d3d_locked_rect;

typedef struct {
  int row_pitch;
  int slice_pitch;
  void *bits;
} d3d_locked_box;

typedef struct {
  unsigned long format;
  unsigned long type;
  unsigned long usage;
  unsigned long size;
  unsigned long multi_sample_type;
  unsigned long width;
  unsigned long height;
} d3d_surface_desc;

typedef struct {
  unsigned long back_buffer_width;
  unsigned long back_buffer_height;
  unsigned long back_buffer_format;
  unsigned long back_buffer_count;
  unsigned long multi_sample_type;
  unsigned long swap_effect;
  void *device_window;
  int windowed;
  int enable_auto_depth_stencil;
  unsigned long auto_depth_stencil_format;
  unsigned long flags;
  unsigned long refresh_rate;
  unsigned long presentation_interval;
} d3d_present_parameters;

/* D3DCAPS8 as the original device reports it (0x3301f8, 0x35 dwords). */
static const unsigned long d3d_device_caps[0x35] = {
  1,          0,        0x20000,    0,          0,      0x80000003, 0,
  0x73bef0,   0x72,     0x377101,   0xff,       0x1fff, 0x1fff,     0xff,
  0x84208,    0x7ec87,  0x1f030000, 0x1f030000, 0,      0x17,       0,
  0x1f,       0x800,    0x800,      0,          0x2000, 0,          2,
  0x501556f9, 0,        0,          0,          0,      0,          0xff,
  0x80000,    0xffffff, 4,          4,          0xbb,   8,          0,
  4,          0,        0x42800000, 0xffff,     0xffff, 0x10,       0xff,
  0xffff0101, 0x60,     0xffff0100, 0x3f800000,
};

static unsigned char d3d_object;
static void *d3d_vblank_thread;
static unsigned long d3d_vblank_count;
static unsigned long d3d_flip_vblank;
static unsigned long d3d_completed_time;
static unsigned long *d3d_render_target;
static unsigned long *d3d_depth_stencil;
static unsigned char *d3d_idle_framebuffer;
static void *d3d_visibility_report_pages[16];
/* Xbox D3DLOCK flags */
#define D3DLOCK_NOFLUSH 0x10
#define D3DLOCK_NOOVERWRITE 0x20
#define D3DLOCK_READONLY 0x80

static int d3d_pbkit_ready;
static int d3d_targets_dirty;
/* Vertical blanks each frame stays on screen; 0 flips without waiting. */
static unsigned long d3d_present_vblanks;

/* state.c pushes only once pbkit is up. */
int compat_d3d_pbkit_ready(void)
{
  return d3d_pbkit_ready;
}

/* The device is created (rasterizer_preinitialize) before
 * physical_memory_allocate claims its fixed ranges, physical
 * 0x61000..0x19a6000 (game state + tag cache); stay above them. */
#define VIDEO_MEMORY_LOWEST 0x019a6000
#define VIDEO_MEMORY_HIGHEST 0x03ffffff

static void *video_alloc(unsigned long size, unsigned long alignment)
{
  return MmAllocateContiguousMemoryEx(size, VIDEO_MEMORY_LOWEST,
                                      VIDEO_MEMORY_HIGHEST, alignment,
                                      PAGE_READWRITE | PAGE_WRITECOMBINE);
}

static void *video_address(unsigned long data)
{
  return (void *)(data | 0x80000000);
}

static compat_resource *resource_new(unsigned long common)
{
  compat_resource *r;

  r = (compat_resource *)compat_calloc(1, sizeof(*r));
  if (r) {
    r->common = common | D3DCOMMON_D3DCREATED | 1;
    r->cookie = COMPAT_RESOURCE_COOKIE;
  }
  return r;
}

static int resource_attach_video(compat_resource *r, unsigned long size)
{
  void *memory;

  memory = video_alloc(size, TEXTURE_ALIGNMENT);
  if (!memory) {
    return 0;
  }
  r->allocation = memory;
  r->contiguous = 1;
  r->data = MmGetPhysicalAddress(memory);
  r->common |= D3DCOMMON_VIDEOMEMORY;
  return 1;
}

static unsigned long format_bits_per_pixel(unsigned long format)
{
  switch (format) {
  case 0x00:
  case 0x01:
  case 0x0b:
  case 0x13:
  case 0x19:
  case 0x1b:
  case 0x1f:
  case D3DFMT_DXT2:
  case D3DFMT_DXT4:
    return 8;
  case D3DFMT_DXT1:
    return 4;
  case 0x02:
  case 0x03:
  case 0x04:
  case 0x05:
  case 0x10:
  case 0x11:
  case 0x17:
  case 0x1a:
  case 0x1c:
  case 0x1d:
  case 0x20:
  case 0x24:
  case 0x25:
  case 0x27:
  case 0x28:
  case 0x29:
  case 0x2c:
  case 0x2d:
  case 0x30:
  case 0x31:
  case 0x32:
  case 0x35:
    return 16;
  default:
    return 32;
  }
}

static int format_is_linear(unsigned long format)
{
  switch (format) {
  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x16:
  case 0x17:
  case 0x1b:
  case 0x1c:
  case 0x1d:
  case 0x1e:
  case 0x1f:
  case 0x20:
  case 0x2e:
  case 0x2f:
  case 0x30:
  case 0x31:
  case 0x35:
  case 0x36:
  case 0x37:
  case 0x3f:
  case 0x40:
  case 0x41:
    return 1;
  default:
    return 0;
  }
}

static int format_is_compressed(unsigned long format)
{
  return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 ||
         format == D3DFMT_DXT4;
}

static unsigned long log2_ceiling(unsigned long value)
{
  unsigned long result;

  result = 0;
  while ((1ul << result) < value) {
    result++;
  }
  return result;
}

/* Geometry of one mip level of a pixel container. */
typedef struct {
  unsigned long format;
  unsigned long width;
  unsigned long height;
  unsigned long depth;
  unsigned long pitch;
  unsigned long slice;
  unsigned long offset;
  unsigned long face_size;
} level_info;

static void level_geometry(unsigned long format_word, unsigned long size_word,
                           unsigned long level, level_info *out)
{
  unsigned long format;
  unsigned long width;
  unsigned long height;
  unsigned long depth;
  unsigned long levels;
  unsigned long bpp;
  unsigned long offset;
  unsigned long i;
  unsigned long pitch;
  unsigned long slice;
  unsigned long total;

  format = (format_word >> D3DFORMAT_FORMAT_SHIFT) & 0xff;
  bpp = format_bits_per_pixel(format);
  if (size_word) {
    out->format = format;
    out->width = (size_word & 0xfff) + 1;
    out->height = ((size_word >> 12) & 0xfff) + 1;
    out->depth = 1;
    out->pitch = ((size_word >> 24) + 1) * 64;
    out->slice = out->pitch * out->height;
    out->offset = 0;
    out->face_size = out->slice;
    return;
  }
  width = 1ul << ((format_word >> D3DFORMAT_USIZE_SHIFT) & 0xf);
  height = 1ul << ((format_word >> D3DFORMAT_VSIZE_SHIFT) & 0xf);
  depth = 1ul << ((format_word >> D3DFORMAT_PSIZE_SHIFT) & 0xf);
  levels = (format_word >> D3DFORMAT_MIPMAP_SHIFT) & 0xf;
  if (levels == 0) {
    levels = 1;
  }
  offset = 0;
  total = 0;
  for (i = 0; i < levels; i++) {
    if (format_is_compressed(format)) {
      pitch = ((width + 3) / 4) * bpp * 2;
      slice = pitch * ((height + 3) / 4);
    } else {
      pitch = width * bpp / 8;
      slice = pitch * height;
    }
    if (i == level) {
      out->format = format;
      out->width = width;
      out->height = height;
      out->depth = depth;
      out->pitch = pitch;
      out->slice = slice;
      out->offset = offset;
    }
    offset += slice * depth;
    total = offset;
    if (width > 1)
      width >>= 1;
    if (height > 1)
      height >>= 1;
    if (depth > 1)
      depth >>= 1;
  }
  if (format_word & D3DFORMAT_CUBEMAP) {
    total = (total + CUBEFACE_ALIGNMENT - 1) &
            ~(unsigned long)(CUBEFACE_ALIGNMENT - 1);
  }
  out->face_size = total;
}

static unsigned long container_bytes(unsigned long format_word,
                                     unsigned long size_word)
{
  level_info info;

  level_geometry(format_word, size_word, 0, &info);
  return (format_word & D3DFORMAT_CUBEMAP) ? info.face_size * 6 :
                                             info.face_size;
}

static int create_pixel_container(unsigned long width, unsigned long height,
                                  unsigned long depth, unsigned long levels,
                                  unsigned long usage, unsigned long format,
                                  int cube, unsigned long dimension, void *out)
{
  compat_resource *r;
  unsigned long max_log;

  *(void **)out = 0;
  r = resource_new(D3DCOMMON_TYPE_TEXTURE);
  if (!r) {
    return E_OUTOFMEMORY;
  }
  r->format = D3DFORMAT_DMACHANNEL_A |
              (dimension << D3DFORMAT_DIMENSION_SHIFT) |
              ((format & 0xff) << D3DFORMAT_FORMAT_SHIFT);
  if (!(usage & D3DUSAGE_BORDERSOURCE_TEXTURE)) {
    r->format |= D3DFORMAT_BORDERSOURCE_COLOR;
  }
  if (cube) {
    r->format |= D3DFORMAT_CUBEMAP;
  }
  if (format_is_linear(format)) {
    unsigned long pitch;

    pitch = (width * format_bits_per_pixel(format) / 8 + 63) & ~63ul;
    r->format |= 1ul << D3DFORMAT_MIPMAP_SHIFT;
    r->size = (width - 1) | ((height - 1) << 12) | ((pitch / 64 - 1) << 24);
  } else {
    max_log = log2_ceiling(width);
    if (log2_ceiling(height) > max_log)
      max_log = log2_ceiling(height);
    if (log2_ceiling(depth) > max_log)
      max_log = log2_ceiling(depth);
    if (levels == 0 || levels > max_log + 1) {
      levels = max_log + 1;
    }
    r->format |= (levels << D3DFORMAT_MIPMAP_SHIFT) |
                 (log2_ceiling(width) << D3DFORMAT_USIZE_SHIFT) |
                 (log2_ceiling(height) << D3DFORMAT_VSIZE_SHIFT) |
                 (log2_ceiling(depth) << D3DFORMAT_PSIZE_SHIFT);
  }
  if (!resource_attach_video(r, container_bytes(r->format, r->size))) {
    compat_free(r);
    return E_OUTOFMEMORY;
  }
  *(void **)out = r;
  return D3D_OK;
}

/* A render-target surface with its own memory (back/front/depth buffers). */
static int init_buffer_surface(compat_resource *surface, unsigned long width,
                               unsigned long height, unsigned long format)
{
  unsigned long pitch;

  pitch = (width * format_bits_per_pixel(format) / 8 + 63) & ~63ul;
  surface->common = D3DCOMMON_TYPE_SURFACE | D3DCOMMON_D3DCREATED | 1;
  surface->format =
    D3DFORMAT_DMACHANNEL_A | (2ul << D3DFORMAT_DIMENSION_SHIFT) |
    (format << D3DFORMAT_FORMAT_SHIFT) | (1ul << D3DFORMAT_MIPMAP_SHIFT);
  surface->size = (width - 1) | ((height - 1) << 12) | ((pitch / 64 - 1) << 24);
  surface->parent = 0;
  return resource_attach_video(surface, pitch * height);
}

/* D3DRECT */
typedef struct {
  long x1;
  long y1;
  long x2;
  long y2;
} d3d_rect;

static unsigned long *buffer_surface(int slot)
{
  return device_field(DEVICE_BUFFER_SURFACES +
                      slot * DEVICE_BUFFER_SURFACE_STRIDE);
}

/* CPU fill of a linear 32-bit surface; bits set in keep_mask are preserved.
 * Swizzled and 16-bit targets (Size 0 or other depths) are left alone until
 * the pbkit backend clears through the GPU. */
static void surface_fill(const unsigned long *surface, const d3d_rect *rect,
                         unsigned long value, unsigned long keep_mask)
{
  unsigned long size;
  unsigned long pitch;
  long width;
  long height;
  long x1;
  long y1;
  long x2;
  long y2;
  long x;
  long y;
  unsigned long *row;

  size = surface[4];
  if (!surface[1] || !size ||
      format_bits_per_pixel((surface[3] >> D3DFORMAT_FORMAT_SHIFT) & 0xff) !=
        32) {
    return;
  }
  width = (long)(size & SURFACE_SIZE_DIMENSION_MASK) + 1;
  height =
    (long)((size >> SURFACE_SIZE_HEIGHT_SHIFT) & SURFACE_SIZE_DIMENSION_MASK) +
    1;
  pitch = ((size >> SURFACE_SIZE_PITCH_SHIFT) + 1) * SURFACE_PITCH_UNIT;
  x1 = 0;
  y1 = 0;
  x2 = width;
  y2 = height;
  if (rect) {
    x1 = rect->x1 > 0 ? rect->x1 : 0;
    y1 = rect->y1 > 0 ? rect->y1 : 0;
    x2 = rect->x2 < width ? rect->x2 : width;
    y2 = rect->y2 < height ? rect->y2 : height;
  }
  for (y = y1; y < y2; y++) {
    row =
      (unsigned long *)((unsigned char *)video_address(surface[1]) + y * pitch);
    for (x = x1; x < x2; x++) {
      row[x] = (row[x] & keep_mask) | (value & ~keep_mask);
    }
  }
}

static unsigned long saturate_depth(float scaled, unsigned long max)
{
  if (!(scaled > 0.0f)) {
    return 0;
  }
  return scaled < (float)max ? (unsigned long)scaled : max;
}

/* The high word of the double z * scale: sign, 11-bit exponent and the top
 * 20 mantissa bits. */
static unsigned long float_depth_word(float z, double scale)
{
  double scaled = z * scale;
  unsigned long words[2];

  compat_memcpy(words, &scaled, sizeof(words));
  return words[1];
}

/* 0x1ea735..0x1ea81c, 0x1ea95c: the zeta word as the format stores it. The
 * float formats rebias the double's exponent to 8 bits over a 16-bit
 * mantissa (F24) or 4 bits over 12 (F16), with z = 0 kept as 0. */
static unsigned long depth_clear_value(const unsigned long *surface, float z,
                                       unsigned long stencil)
{
  unsigned long depth;

  switch ((surface[3] >> D3DFORMAT_FORMAT_SHIFT) & 0xff) {
  case 0x2a: /* D3DFMT_D24S8 */
  case 0x2e: /* D3DFMT_LIN_D24S8 */
    depth = saturate_depth(z * (float)DEPTH_D24_MAX, DEPTH_D24_MAX)
            << DEPTH_STENCIL_SHIFT;
    break;
  case 0x2b: /* D3DFMT_F24S8 */
  case 0x2f: /* D3DFMT_LIN_F24S8 */
    depth = z == 0.0f ? 0 :
            ((float_depth_word(z, 1e30) - 0x08000000ul) & 0xfffffff0ul) << 4;
    break;
  case 0x2c: /* D3DFMT_D16 */
  case 0x30: /* D3DFMT_LIN_D16 */
    depth = saturate_depth(z * (float)DEPTH_D16_MAX, DEPTH_D16_MAX);
    break;
  case 0x2d: /* D3DFMT_F16 */
  case 0x31: /* D3DFMT_LIN_F16 */
    depth = z == 0.0f ? 0 :
            ((float_depth_word(z, 511.9375) >> 8) - 0x8000) & 0xffff;
    break;
  default:
    depth = 0;
    break;
  }
  return depth | stencil;
}

/* 0x1e7b0b..0x1e7b5d: a linear surface carries its dimensions in Size; a
 * swizzled one (Size 0) carries log2 width/height in Format. */
unsigned long compat_d3d_surface_width(const unsigned long *surface)
{
  if (surface[4] == 0) {
    return 1ul << ((surface[3] >> D3DFORMAT_USIZE_SHIFT) & 0xf);
  }
  return (surface[4] & SURFACE_SIZE_DIMENSION_MASK) + 1;
}

unsigned long compat_d3d_surface_height(const unsigned long *surface)
{
  if (surface[4] == 0) {
    return 1ul << ((surface[3] >> D3DFORMAT_VSIZE_SHIFT) & 0xf);
  }
  return ((surface[4] >> SURFACE_SIZE_HEIGHT_SHIFT) &
          SURFACE_SIZE_DIMENSION_MASK) +
         1;
}

/* 0x1e77d0: swizzled surfaces are packed at width * bytes per texel. */
static unsigned long surface_pitch(const unsigned long *surface)
{
  if (surface[4] == 0) {
    return compat_d3d_surface_width(surface) *
           format_bits_per_pixel((surface[3] >> D3DFORMAT_FORMAT_SHIFT) &
                                 0xff) /
           8;
  }
  return ((surface[4] >> SURFACE_SIZE_PITCH_SHIFT) + 1) * SURFACE_PITCH_UNIT;
}

#define SURFACE_FORMAT_ZETA_SHIFT 4
#define SURFACE_FORMAT_TYPE_SHIFT 8
/* Swizzled surfaces: log2 width in bits 16-19, log2 height in 24-27, which
 * are Format's USIZE (20-23) moved down four bits and VSIZE as it is. */
#define SURFACE_FORMAT_SWIZZLE_WIDTH(format) (((format) & 0x00f00000ul) >> 4)
#define SURFACE_FORMAT_SWIZZLE_HEIGHT(format) ((format) & 0x0f000000ul)

/* 0x1f3e40: NV097_SET_SURFACE_FORMAT for a color target and optional
 * depth buffer. */
static unsigned long surface_format(const unsigned long *color,
                                    const unsigned long *zeta)
{
  unsigned long format;
  unsigned long nv;
  int swizzled;

  format = (color[3] >> D3DFORMAT_FORMAT_SHIFT) & 0xff;
  swizzled = 1;
  switch (format) {
  case 0x03: /* D3DFMT_X1R5G5B5 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5;
    break;
  case 0x05: /* D3DFMT_R5G6B5 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
    break;
  case 0x06: /* D3DFMT_A8R8G8B8 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    break;
  case 0x07: /* D3DFMT_X8R8G8B8 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8;
    break;
  case 0x11: /* D3DFMT_LIN_R5G6B5 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
    swizzled = 0;
    break;
  case 0x13: /* D3DFMT_LIN_B8 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_B8;
    swizzled = 0;
    break;
  case 0x17: /* D3DFMT_LIN_G8B8 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8;
    swizzled = 0;
    break;
  case 0x1c: /* D3DFMT_LIN_X1R5G5B5 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5;
    swizzled = 0;
    break;
  case 0x1e: /* D3DFMT_LIN_X8R8G8B8 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8;
    swizzled = 0;
    break;
  case 0x12: /* D3DFMT_LIN_A8R8G8B8 */
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    swizzled = 0;
    break;
  default:
    /* The XDK asserts on any other render-target format. */
    halo_standalone_missing("d3d8_device: unsupported render-target format");
    nv = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    swizzled = 0;
    break;
  }
  if (swizzled) {
    nv |= (NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE << SURFACE_FORMAT_TYPE_SHIFT) |
          SURFACE_FORMAT_SWIZZLE_WIDTH(color[3]) |
          SURFACE_FORMAT_SWIZZLE_HEIGHT(color[3]);
  } else {
    nv |= NV097_SET_SURFACE_FORMAT_TYPE_PITCH << SURFACE_FORMAT_TYPE_SHIFT;
  }
  if (zeta) {
    switch ((zeta[3] >> D3DFORMAT_FORMAT_SHIFT) & 0xff) {
    case 0x2a: /* D3DFMT_D24S8 */
    case 0x2b: /* D3DFMT_F24S8 */
    case 0x2e: /* D3DFMT_LIN_D24S8 */
    case 0x2f: /* D3DFMT_LIN_F24S8 */
      nv |= NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 << SURFACE_FORMAT_ZETA_SHIFT;
      break;
    case 0x2c: /* D3DFMT_D16 */
    case 0x2d: /* D3DFMT_F16 */
    case 0x30: /* D3DFMT_LIN_D16 */
    case 0x31: /* D3DFMT_LIN_F16 */
      nv |= NV097_SET_SURFACE_FORMAT_ZETA_Z16 << SURFACE_FORMAT_ZETA_SHIFT;
      break;
    }
  } else if (format_bits_per_pixel(format) == 32) {
    nv |= NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 << SURFACE_FORMAT_ZETA_SHIFT;
  } else {
    nv |= NV097_SET_SURFACE_FORMAT_ZETA_Z16 << SURFACE_FORMAT_ZETA_SHIFT;
  }
  return nv;
}

/* Points the NV2A color and zeta surfaces at the current D3D targets. */
static void gpu_bind_targets(void)
{
  unsigned long *color;
  unsigned long *zeta;
  unsigned long width;
  unsigned long height;
  unsigned long format;
  unsigned long zeta_pitch;
  uint32_t *p;

  color = d3d_render_target;
  zeta = d3d_depth_stencil;
  width = compat_d3d_surface_width(color);
  height = compat_d3d_surface_height(color);
  format = surface_format(color, zeta);
  zeta_pitch = zeta ? surface_pitch(zeta) : surface_pitch(color);

  p = compat_d3d_push_reserve(16);
  p = pb_push1(p, NV097_SET_CONTEXT_DMA_COLOR, PBKIT_DMA_ALL_RAM);
  p = pb_push1(p, NV097_SET_CONTEXT_DMA_ZETA, PBKIT_DMA_ALL_RAM);
  p = pb_push1(p, NV097_SET_SURFACE_FORMAT, format);
  p = pb_push1(p, NV097_SET_SURFACE_PITCH,
               (zeta_pitch << 16) | surface_pitch(color));
  p = pb_push1(p, NV097_SET_SURFACE_CLIP_HORIZONTAL, width << 16);
  p = pb_push1(p, NV097_SET_SURFACE_CLIP_VERTICAL, height << 16);
  p = pb_push1(p, NV097_SET_SURFACE_COLOR_OFFSET, color[1]);
  p = pb_push1(p, NV097_SET_SURFACE_ZETA_OFFSET, zeta ? zeta[1] : 0);
  compat_d3d_push_end(p);
  d3d_targets_dirty = 0;
}

static void gpu_clear(const d3d_rect *rect, unsigned long flags,
                      unsigned long color, unsigned long depth)
{
  unsigned long *target;
  long width;
  long height;
  long x1;
  long y1;
  long x2;
  long y2;
  uint32_t *p;

  target = d3d_render_target;
  width = (long)compat_d3d_surface_width(target);
  height = (long)compat_d3d_surface_height(target);
  x1 = 0;
  y1 = 0;
  x2 = width;
  y2 = height;
  if (rect) {
    x1 = rect->x1 > 0 ? rect->x1 : 0;
    y1 = rect->y1 > 0 ? rect->y1 : 0;
    x2 = rect->x2 < width ? rect->x2 : width;
    y2 = rect->y2 < height ? rect->y2 : height;
  }
  if (x2 <= x1 || y2 <= y1) {
    return;
  }
  p = compat_d3d_push_reserve(8);
  p = pb_push2(
    p, NV097_SET_CLEAR_RECT_HORIZONTAL,
    ((unsigned long)(x2 - 1) << NV097_CLEAR_RECT_SHIFT) | (unsigned long)x1,
    ((unsigned long)(y2 - 1) << NV097_CLEAR_RECT_SHIFT) | (unsigned long)y1);
  p = pb_push2(p, NV097_SET_ZSTENCIL_CLEAR_VALUE, depth, color);
  /* NV097_CLEAR_SURFACE takes the Xbox D3DCLEAR bits as they are. */
  p = pb_push1(p, NV097_CLEAR_SURFACE, flags);
  compat_d3d_push_end(p);
}

void *__stdcall Direct3DCreate8(uint32_t sdk_version)
{
  (void)sdk_version;
  return &d3d_object;
}

/* pbkit and XVideoWaitForVBlank each connect the GPU interrupt (IRQ 3), so
 * once pbkit is up every vblank wait goes through its ISR. */
static void wait_for_vblank(void)
{
  if (d3d_pbkit_ready) {
    pb_wait_for_vbl();
  } else {
    XVideoWaitForVBlank();
  }
}

/* D3DVBLANKDATA, as the XDK passes it to the vertical-blank callback. */
typedef struct {
  unsigned long vblank;
  unsigned long swap;
  unsigned long flags;
} d3d_vblank_data;

typedef void(__cdecl *d3d_vblank_callback)(d3d_vblank_data *data);

/* Stands in for the XDK's vblank interrupt DPC: completes the pending flip,
 * then runs the game's callback (main_vertical_blank_interrupt_handler, which
 * also ticks input) once per vertical blank. */
static unsigned long __stdcall vblank_thread(void *parameter)
{
  d3d_vblank_data data;
  d3d_vblank_callback callback;

  (void)parameter;
  for (;;) {
    wait_for_vblank();
    d3d_vblank_count++;
    if (*device_field(DEVICE_FLIP_COUNT) != *device_field(DEVICE_SWAP_COUNT)) {
      *device_field(DEVICE_FLIP_COUNT) = *device_field(DEVICE_SWAP_COUNT);
      d3d_flip_vblank = d3d_vblank_count;
    }
    callback = (d3d_vblank_callback)*device_field(DEVICE_VBLANK_CALLBACK);
    if (callback) {
      data.vblank = d3d_vblank_count;
      data.swap = *device_field(DEVICE_SWAP_COUNT);
      data.flags = 0;
      callback(&data);
    }
  }
  return 0;
}

int __stdcall Direct3D_CreateDevice(uint32_t adapter, uint32_t device_type,
                                    void *focus_window, uint32_t behavior_flags,
                                    void *present_parameters, void **out_device)
{
  const d3d_present_parameters *pp;
  compat_resource *surfaces;
  compat_resource *depth;
  unsigned long width;
  unsigned long height;
  int i;

  (void)adapter;
  (void)device_type;
  (void)focus_window;
  (void)behavior_flags;
  pp = (const d3d_present_parameters *)present_parameters;
  width = pp->back_buffer_width ? pp->back_buffer_width : 640;
  height = pp->back_buffer_height ? pp->back_buffer_height : 480;
  switch (pp->presentation_interval) {
  case D3DPRESENT_INTERVAL_IMMEDIATE:
    d3d_present_vblanks = 0;
    break;
  case D3DPRESENT_INTERVAL_TWO:
    d3d_present_vblanks = 2;
    break;
  default:
    d3d_present_vblanks = 1;
    break;
  }

  compat_memset(D3D_DEVICE, 0, DEVICE_SIZE);
  *device_field(DEVICE_INTERRUPTS_ENABLED) = 1;
  *device_field(DEVICE_REFERENCE_COUNT) = 1;
  *device_field(DEVICE_COMPLETED_TIME_POINTER) =
    (unsigned long)&d3d_completed_time;
  surfaces = (compat_resource *)device_field(DEVICE_BUFFER_SURFACES);
  for (i = 0; i < 2; i++) {
    /* Only the leading 0x18 bytes of each slot are the surface header. */
    compat_resource tmp;

    compat_memset(&tmp, 0, sizeof(tmp));
    if (!init_buffer_surface(&tmp, width, height, D3DFMT_LIN_A8R8G8B8)) {
      *out_device = 0;
      return E_OUTOFMEMORY;
    }
    /* The startup probe presents before Halo renders or clears a frame. */
    compat_memset(tmp.allocation, 0, container_bytes(tmp.format, tmp.size));
    compat_memcpy((unsigned char *)surfaces + i * 0x18, &tmp, 0x18);
  }
  XVideoFlushFB();
  if (pp->enable_auto_depth_stencil) {
    depth = resource_new(D3DCOMMON_TYPE_SURFACE);
    if (!depth || !init_buffer_surface(depth, width, height,
                                       pp->auto_depth_stencil_format == 0x2b ?
                                         D3DFMT_LIN_F24S8 :
                                         D3DFMT_LIN_D24S8)) {
      *out_device = 0;
      return E_OUTOFMEMORY;
    }
    *device_field(DEVICE_DEPTH_STENCIL) = (unsigned long)depth;
  }
  d3d_render_target = buffer_surface(DEVICE_BACK_BUFFER_SLOT);
  d3d_depth_stencil =
    (unsigned long *)*device_field(DEVICE_DEPTH_STENCIL);
  d3d_targets_dirty = 1;
  d3d_idle_framebuffer = 0;
  if (XVideoSetMode((int)width, (int)height, VIDEO_BITS_PER_PIXEL,
                    VIDEO_REFRESH_DEFAULT)) {
    /* nxdk owns this cleared buffer until the next mode change. Keep its
     * physical address before scanout switches to the device's surfaces. */
    d3d_idle_framebuffer =
      (unsigned char *)MmGetPhysicalAddress(XVideoGetFB());
  }
  if (!d3d_pbkit_ready) {
    /* Halo asks the XDK for a 768KB push buffer (D3D_SetPushBufferSize);
     * pbkit does not wrap, so it gets a larger one (draw.c). */
    pb_size(compat_d3d_push_buffer_bytes());
    /* The device renders into and scans out its own surfaces above, as the
     * XDK does; pbkit's triple frame buffers and depth buffer (~4.7MB of
     * low-64MB contiguous memory) would go unused and starve the sound
     * cache in physical_memory_allocate. */
    pb_set_surface_allocation(0);
    d3d_pbkit_ready = pb_init() == 0;
    if (d3d_pbkit_ready) {
      compat_d3d_push_rewind();
    }
  }
  D3D_g_pDevice = (uint8_t *)D3D_DEVICE;
  *device_field(DEVICE_RENDER_TARGET) =
    (unsigned long)d3d_render_target;
  compat_d3d_state_initialize();
  compat_d3d_set_depth_scale(d3d_depth_stencil);
  compat_d3d_draw_initialize();
  if (d3d_pbkit_ready) {
    /* Establish both color surfaces on the GPU before scanout. A CPU clear
     * alone leaves xemu on its VGA fallback until the first rendered frame. */
    for (i = DEVICE_FRONT_BUFFER_SLOT; i >= DEVICE_BACK_BUFFER_SLOT; i--) {
      d3d_render_target = buffer_surface(i);
      gpu_bind_targets();
      gpu_clear(0, D3DCLEAR_TARGET_R | D3DCLEAR_TARGET_G |
                     D3DCLEAR_TARGET_B | D3DCLEAR_TARGET_A, 0, 0);
    }
    compat_d3d_push_kick();
    pb_wait_until_gr_not_busy();
  }
  XVideoSetFB((unsigned char *)buffer_surface(DEVICE_FRONT_BUFFER_SLOT)[1]);
  if (!d3d_vblank_thread) {
    d3d_vblank_thread =
      compat_CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);
    compat_SetThreadPriority(d3d_vblank_thread,
                             THREAD_PRIORITY_TIME_CRITICAL_VALUE);
  }
  *out_device = D3D_DEVICE;
  return D3D_OK;
}

void __stdcall D3DDevice_GetDeviceCaps(void *caps)
{
  compat_memcpy(caps, d3d_device_caps, sizeof(d3d_device_caps));
}

/* 0x1e7d50: -1 is the front buffer (slot 1), 0 the back buffer (slot 0). */
int __stdcall D3DDevice_GetBackBuffer(int back_buffer, uint32_t type,
                                      void **out_surface)
{
  unsigned long *surface;

  (void)type;
  if (back_buffer == -1) {
    back_buffer = 1;
  } else if (back_buffer != 0) {
    back_buffer++;
  }
  surface = device_field(DEVICE_BUFFER_SURFACES + back_buffer * 0x18);
  *out_surface = surface;
  surface[0]++;
  return D3D_OK;
}

int __stdcall D3DDevice_GetDepthStencilSurface(void **out_surface)
{
  unsigned long *surface;

  surface = (unsigned long *)*device_field(DEVICE_DEPTH_STENCIL);
  *out_surface = surface;
  if (!surface) {
    return 0x88760827; /* D3DERR_NOTFOUND */
  }
  surface[0]++;
  return D3D_OK;
}

int __stdcall D3DDevice_CreateTexture(uint32_t width, uint32_t height,
                                      uint32_t levels, uint32_t usage,
                                      uint32_t format, uint32_t pool,
                                      void *out_texture)
{
  (void)pool;
  return create_pixel_container(width, height, 1, levels, usage, format, 0, 2,
                                out_texture);
}

int __stdcall D3DDevice_CreateVolumeTexture(uint32_t width, uint32_t height,
                                            uint32_t depth, uint32_t levels,
                                            uint32_t usage, uint32_t format,
                                            uint32_t pool,
                                            void *out_volume_texture)
{
  (void)pool;
  return create_pixel_container(width, height, depth, levels, usage, format, 0,
                                3, out_volume_texture);
}

int __stdcall D3DDevice_CreateCubeTexture(uint32_t edge_length, uint32_t levels,
                                          uint32_t usage, uint32_t format,
                                          uint32_t pool, void *out_cube_texture)
{
  (void)pool;
  return create_pixel_container(edge_length, edge_length, 1, levels, usage,
                                format, 1, 2, out_cube_texture);
}

int __stdcall D3DDevice_CreateVertexBuffer(uint32_t length, uint32_t usage,
                                           uint32_t fvf, uint32_t pool,
                                           void **ppVertexBuffer)
{
  compat_resource *r;

  (void)usage;
  (void)fvf;
  (void)pool;
  *ppVertexBuffer = 0;
  r = resource_new(D3DCOMMON_TYPE_VERTEXBUFFER);
  if (!r || !resource_attach_video(r, length ? length : 1)) {
    compat_free(r);
    return E_OUTOFMEMORY;
  }
  *ppVertexBuffer = r;
  return D3D_OK;
}

/* Index buffers live in cached system memory; Data is a virtual address. */
int __stdcall D3DDevice_CreateIndexBuffer(uint32_t length, uint32_t usage,
                                          uint32_t format, uint32_t pool,
                                          void **index_buffer)
{
  compat_resource *r;

  (void)usage;
  (void)format;
  (void)pool;
  *index_buffer = 0;
  r = resource_new(D3DCOMMON_TYPE_INDEXBUFFER);
  if (!r) {
    return E_OUTOFMEMORY;
  }
  r->allocation = compat_malloc(length ? length : 1);
  if (!r->allocation) {
    compat_free(r);
    return E_OUTOFMEMORY;
  }
  r->data = (unsigned long)r->allocation;
  *index_buffer = r;
  return D3D_OK;
}

/* Palettes: D3DPALETTE_256 (0) .. D3DPALETTE_32 (3), four bytes per entry. */
int __stdcall D3DDevice_CreatePalette(uint32_t size, void **out_palette)
{
  static const unsigned long entries[4] = { 256, 128, 64, 32 };
  compat_resource *r;

  *out_palette = 0;
  r = resource_new(D3DCOMMON_TYPE_PALETTE | ((size & 3) << 30));
  if (!r || !resource_attach_video(r, entries[size & 3] * 4)) {
    compat_free(r);
    return E_OUTOFMEMORY;
  }
  *out_palette = r;
  return D3D_OK;
}

/* D3D_BlockOnResource (0x1efd80) waits for the GPU fence of the resource.
 * No fences are kept here, so wait for all queued GPU work instead. */
static void block_on_resource(void)
{
  if (d3d_pbkit_ready) {
    compat_d3d_push_drain();
  }
}

/* 0x1e6fb0: jmp CDevice_KickOff. */
void D3DDevice_KickPushBuffer(void)
{
  if (d3d_pbkit_ready) {
    compat_d3d_push_kick();
  }
}

/* 0x1e8a00: the GPU is busy until it has fetched up to the kicked put
 * pointer and PGRAPH is idle. It does not kick. kb.json declares it void,
 * but it returns a BOOL in eax. */
int compat_D3DDevice_IsBusy(void) __asm__("_D3DDevice_IsBusy");
int compat_D3DDevice_IsBusy(void)
{
  return d3d_pbkit_ready && pb_busy();
}

/* 0x1efa80 returns at once for time 0 and otherwise waits until the GPU
 * passes the fence. The resources here carry no fence, so a nonzero time
 * only comes from the device's own time: wait for all queued work. */
void __stdcall D3D_BlockOnTime(uint32_t time, int param2)
{
  (void)param2;
  if (time != 0) {
    block_on_resource();
  }
}

/* 0x1eefe0 blocks unless READONLY or NOOVERWRITE. */
void __stdcall D3DPalette_Lock(void *palette, void **out_data, uint32_t flags)
{
  if (!(flags & (D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE))) {
    block_on_resource();
  }
  *out_data = video_address(((compat_resource *)palette)->data);
}

void __stdcall D3DVertexBuffer_Lock(void *vertex_buffer,
                                    uint32_t offset_to_lock,
                                    uint32_t size_to_lock, void **ppb_data,
                                    uint32_t flags)
{
  uint32_t *p;

  (void)size_to_lock;
  /* 0x1ef100: break the vertex cache unless NOFLUSH, and block unless
   * READONLY or NOOVERWRITE. */
  if (!(flags & D3DLOCK_NOFLUSH)) {
    p = compat_d3d_push_begin(2);
    p = pb_push1(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
    compat_d3d_push_end(p);
  }
  if (!(flags & (D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE))) {
    block_on_resource();
  }
  *ppb_data =
    (unsigned char *)video_address(((compat_resource *)vertex_buffer)->data) +
    offset_to_lock;
}

void __stdcall Get2DSurfaceDesc(void *texture, unsigned int level, void *desc)
{
  const compat_resource *r;
  d3d_surface_desc *d;
  level_info info;

  r = (const compat_resource *)texture;
  d = (d3d_surface_desc *)desc;
  level_geometry(r->format, r->size, level, &info);
  d->format = info.format;
  d->type = ((r->common & D3DCOMMON_TYPE_MASK) == D3DCOMMON_TYPE_SURFACE) ?
              D3DRTYPE_SURFACE :
              D3DRTYPE_TEXTURE;
  d->usage = 0;
  d->size = info.slice;
  d->multi_sample_type = 0;
  d->width = info.width;
  d->height = info.height;
}

int __stdcall D3DSurface_GetDesc(void *surface, void *desc)
{
  Get2DSurfaceDesc(surface, 0, desc);
  return D3D_OK;
}

void __stdcall Lock2DSurface(void *texture, unsigned int face,
                             unsigned int level, void *locked_rect, void *rect,
                             unsigned int flags)
{
  const compat_resource *r;
  d3d_locked_rect *out;
  level_info info;
  unsigned char *bits;

  /* 0x1f44f0 blocks unless NOOVERWRITE. */
  if (!(flags & D3DLOCK_NOOVERWRITE)) {
    block_on_resource();
  }
  r = (const compat_resource *)texture;
  out = (d3d_locked_rect *)locked_rect;
  level_geometry(r->format, r->size, level, &info);
  bits = (unsigned char *)video_address(r->data) + face * info.face_size +
         info.offset;
  if (rect) {
    const long *rc = (const long *)rect; /* left, top, right, bottom */

    bits += rc[1] * info.pitch + rc[0] * format_bits_per_pixel(info.format) / 8;
  }
  out->pitch = (int)info.pitch;
  out->bits = bits;
}

void __stdcall D3DCubeTexture_LockRect(void *cube_texture, uint32_t face_type,
                                       uint32_t level, void *locked_rect,
                                       void *rect, uint32_t flags)
{
  Lock2DSurface(cube_texture, face_type, level, locked_rect, rect, flags);
}

int __stdcall D3DSurface_LockRect(void *surface, void *locked_rect, void *rect,
                                  uint32_t flags)
{
  Lock2DSurface(surface, 0, 0, locked_rect, rect, flags);
  return D3D_OK;
}

void __stdcall D3DVolumeTexture_LockBox(void *volume_texture, uint32_t level,
                                        void *locked_box, void *box,
                                        uint32_t flags)
{
  const compat_resource *r;
  d3d_locked_box *out;
  level_info info;

  (void)box;
  /* Lock3DSurface (0x1f4590) blocks unless NOOVERWRITE. */
  if (!(flags & D3DLOCK_NOOVERWRITE)) {
    block_on_resource();
  }
  r = (const compat_resource *)volume_texture;
  out = (d3d_locked_box *)locked_box;
  level_geometry(r->format, r->size, level, &info);
  out->row_pitch = (int)info.pitch;
  out->slice_pitch = (int)info.slice;
  out->bits = (unsigned char *)video_address(r->data) + info.offset;
}

/* Surfaces share the parent's memory; freeing happens with the parent. */
int __stdcall D3DTexture_GetSurfaceLevel(void *texture, uint32_t level,
                                         void **out_surface)
{
  compat_resource *parent;
  compat_resource *s;
  level_info info;

  parent = (compat_resource *)texture;
  *out_surface = 0;
  s = resource_new(D3DCOMMON_TYPE_SURFACE);
  if (!s) {
    return E_OUTOFMEMORY;
  }
  level_geometry(parent->format, parent->size, level, &info);
  s->data = parent->data + info.offset;
  s->size = parent->size;
  s->format = parent->format & ~(0xful << D3DFORMAT_MIPMAP_SHIFT) &
              ~(0xfful << D3DFORMAT_USIZE_SHIFT);
  s->format |= (1ul << D3DFORMAT_MIPMAP_SHIFT) |
               (log2_ceiling(info.width) << D3DFORMAT_USIZE_SHIFT) |
               (log2_ceiling(info.height) << D3DFORMAT_VSIZE_SHIFT);
  s->parent = parent;
  parent->common++;
  *out_surface = s;
  return D3D_OK;
}

void __stdcall D3D_DestroyResource(void *resource)
{
  compat_resource *r;

  r = (compat_resource *)resource;
  if (r->cookie != COMPAT_RESOURCE_COOKIE) {
    return;
  }
  if (r->allocation) {
    /* 0x1ed7d0: BlockOnResource before the memory goes back. */
    block_on_resource();
    if (r->contiguous) {
      MmFreeContiguousMemory(r->allocation);
    } else {
      compat_free(r->allocation);
    }
  }
  r->cookie = 0;
  compat_free(r);
}

/* 0x1e6f50: the last reference uninitialises the device (0x1f3a90) and
 * clears it and D3D_g_pDevice. rasterizer_preinitialize creates a probe
 * device and releases it before rasterizer_initialize creates the real one,
 * so the probe's buffers must go back to contiguous memory here. The
 * display must switch to nxdk's cleared buffer before those pages can be
 * reused by physical_memory_allocate or other startup allocations. */
void D3DDevice_Release(void)
{
  unsigned long idle_surface[6];
  unsigned long *surface;
  unsigned long count;
  int i;

  count = *device_field(DEVICE_REFERENCE_COUNT);
  if (count != 1) {
    *device_field(DEVICE_REFERENCE_COUNT) = count - 1;
    return;
  }
  if (d3d_pbkit_ready) {
    if (d3d_idle_framebuffer) {
      /* The idle buffer must also have a GPU color surface while the probe's
       * surfaces are freed and Halo performs the rest of its startup. */
      compat_memcpy(idle_surface, buffer_surface(DEVICE_FRONT_BUFFER_SLOT),
                     sizeof(idle_surface));
      idle_surface[1] = (unsigned long)d3d_idle_framebuffer;
      d3d_render_target = idle_surface;
      d3d_depth_stencil = 0;
      gpu_bind_targets();
      gpu_clear(0, D3DCLEAR_TARGET_R | D3DCLEAR_TARGET_G |
                     D3DCLEAR_TARGET_B | D3DCLEAR_TARGET_A, 0, 0);
    }
    compat_d3d_push_kick();
    pb_wait_until_gr_not_busy();
  }
  if (d3d_idle_framebuffer) {
    XVideoSetFB(d3d_idle_framebuffer);
  } else {
    XVideoSetVideoEnable(0);
  }
  /* Let scanout finish the old frame before freeing its memory. */
  wait_for_vblank();
  for (i = 0; i < DEVICE_BUFFER_SURFACE_COUNT; i++) {
    /* Buffer slots are bare 0x18-byte headers (Direct3D_CreateDevice), not
     * compat_resources; the bits are video_alloc memory at Data. */
    surface = buffer_surface(i);
    if (surface[1]) {
      MmFreeContiguousMemory(video_address(surface[1]));
    }
  }
  surface = (unsigned long *)*device_field(DEVICE_DEPTH_STENCIL);
  if (surface) {
    D3D_DestroyResource(surface);
  }
  d3d_render_target = 0;
  d3d_depth_stencil = 0;
  D3D_g_pDevice = 0;
  compat_memset(D3D_DEVICE, 0, DEVICE_SIZE);
}

/* 0x1f02d0: build the viewport/depth adjustment matrix and pre-multiply the
 * current projection matrix into device+0x5a0. */
void D3D_UpdateProjectionViewportTransform(void)
{
  float adjustment[16];
  float width;
  float height;
  float depth_range;
  float depth_offset;
  float w_scale;

  compat_memset(adjustment, 0, sizeof(adjustment));
  width = (float)*device_field(DEVICE_VIEWPORT + 8) *
          *(float *)device_field(DEVICE_SUPER_SAMPLE_SCALE_X) * 0.5f;
  height = (float)*device_field(DEVICE_VIEWPORT + 12) *
           *(float *)device_field(DEVICE_SUPER_SAMPLE_SCALE_Y) * -0.5f;
  depth_range = (*(float *)device_field(DEVICE_VIEWPORT + 20) -
                 *(float *)device_field(DEVICE_VIEWPORT + 16)) *
                *(float *)device_field(DEVICE_Z_SCALE);
  depth_offset = *(float *)device_field(DEVICE_VIEWPORT + 16) *
                 *(float *)device_field(DEVICE_Z_SCALE);
  w_scale = 1.0f;
  if (*device_field(DEVICE_STATE_FLAGS) & 2) {
    w_scale = *(float *)device_field(DEVICE_Z_SCALE_W) *
              *(float *)device_field(DEVICE_Z_SCALE);
    width *= w_scale;
    height *= w_scale;
    depth_range *= w_scale;
    depth_offset *= w_scale;
  }
  adjustment[0] = width;
  adjustment[5] = height;
  adjustment[10] = depth_range;
  adjustment[12] = width;
  adjustment[13] = -height;
  adjustment[14] = depth_offset;
  adjustment[15] = w_scale;
  compat_d3d_matrix4x4_multiply((float *)device_field(DEVICE_DERIVED_PROJECTION_MATRIX),
                     (const float *)device_field(DEVICE_PROJECTION_MATRIX),
                     adjustment);
  *device_field(DEVICE_DIRTY) |= DIRTY_TRANSFORMS;
}

void __stdcall D3DDevice_SetTransform(uint32_t type, void *matrix)
{
  const float *m;
  float depth_scale;

  if (type < D3DTS_COUNT) {
    compat_memcpy(device_field(DEVICE_TRANSFORM_BASE + type * D3DTS_MATRIX_BYTES),
                  matrix, D3DTS_MATRIX_BYTES);
    *device_field(DEVICE_DIRTY) |= transform_dirty_bits[type];
    if (type == D3DTS_PROJECTION) {
      m = (const float *)matrix;
      if (m[10] == m[11] || m[10] == 0.0f) {
        *(float *)device_field(DEVICE_PROJECTION_DEPTH_OFFSET) = 0.0f;
        *(float *)device_field(DEVICE_PROJECTION_DEPTH_SCALE) = 1.0f;
      } else {
        *(float *)device_field(DEVICE_PROJECTION_DEPTH_OFFSET) =
          m[15] - (m[14] / m[10]) * m[11];
        *(float *)device_field(DEVICE_PROJECTION_DEPTH_SCALE) =
          ((m[15] - m[14]) / (m[10] - m[11])) * m[11] + m[15];
      }
      depth_scale =
        1.0f / *(float *)device_field(DEVICE_PROJECTION_DEPTH_SCALE);
      *(float *)device_field(DEVICE_Z_SCALE_W) = depth_scale;
      *device_field(DEVICE_STATE_FLAGS) &= ~(unsigned long)2;
      if (m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f) {
        *device_field(DEVICE_STATE_FLAGS) |= 2;
      }
      D3D_UpdateProjectionViewportTransform();
      compat_d3d_push_passthrough_program();
    }
  }
}

void __stdcall D3DDevice_GetTransform(uint32_t type, void *matrix_out)
{
  if (type < D3DTS_COUNT) {
    compat_memcpy(matrix_out,
                  device_field(DEVICE_TRANSFORM_BASE + type * D3DTS_MATRIX_BYTES),
                  D3DTS_MATRIX_BYTES);
  }
}

/* Vertex shaders: draw.c. */

#define D3DERR_VISIBILITY_PENDING 0x88760828
#define VISIBILITY_REPORT_PAGE_SIZE 0x1000
#define VISIBILITY_REPORT_STRIDE 0x10
#define VISIBILITY_REPORT_STATUS_OFFSET 0x0c
#define VISIBILITY_REPORT_RESULT_OFFSET 0x08
#define VISIBILITY_REPORT_PAGE(index) ((index) >> 8)
#define VISIBILITY_REPORT_SLOT(index) ((index) & 0xff)
#define VISIBILITY_REPORT_PAGE_COUNT 16

static unsigned long *visibility_report_get(unsigned int index)
{
  unsigned int page_index;
  unsigned long *report;

  page_index = VISIBILITY_REPORT_PAGE(index);
  if (page_index >= VISIBILITY_REPORT_PAGE_COUNT) {
    return NULL;
  }
  if (d3d_visibility_report_pages[page_index] == NULL) {
    d3d_visibility_report_pages[page_index] = MmAllocateContiguousMemoryEx(
      VISIBILITY_REPORT_PAGE_SIZE, 0, 0xffffff, 0, PAGE_READWRITE);
    if (d3d_visibility_report_pages[page_index] == NULL) {
      return NULL;
    }
    compat_memset(d3d_visibility_report_pages[page_index], 0,
                  VISIBILITY_REPORT_PAGE_SIZE);
  }
  report =
    (unsigned long *)((unsigned char *)d3d_visibility_report_pages[page_index] +
                      VISIBILITY_REPORT_SLOT(index) * VISIBILITY_REPORT_STRIDE);
  return report;
}

void D3DDevice_BeginVisibilityTest(void)
{
  uint32_t *p;

  if (!d3d_pbkit_ready) {
    return;
  }
  p = compat_d3d_push_reserve(4);
  p = pb_push1(p, NV097_CLEAR_REPORT_VALUE,
               NV097_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT);
  p = pb_push1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
  compat_d3d_push_end(p);
}

int __stdcall D3DDevice_EndVisibilityTest(unsigned int index)
{
  unsigned long *report;
  unsigned long report_offset;
  uint32_t *p;

  report = visibility_report_get(index);
  if (report == NULL) {
    return E_OUTOFMEMORY;
  }
  /* Without pbkit nothing was drawn and no GPU will write the report:
   * complete it with no pixels, or the game polls it forever. */
  if (!d3d_pbkit_ready) {
    *(unsigned long *)((unsigned char *)report +
                       VISIBILITY_REPORT_RESULT_OFFSET) = 0;
    *(unsigned long *)((unsigned char *)report +
                       VISIBILITY_REPORT_STATUS_OFFSET) = 0;
    return D3D_OK;
  }
  /* Arm the slot here only: the GPU clears the status word when it writes
   * the report, and GetVisibilityTestResult polls it. Re-arming on every
   * lookup made the poll see "pending" forever. */
  if (!(*device_field(DEVICE_STATE_FLAGS) & 4)) {
    *(unsigned long *)((unsigned char *)report +
                       VISIBILITY_REPORT_STATUS_OFFSET) = 0xffffffff;
  }
  report_offset = MmGetPhysicalAddress(report) & NV097_GET_REPORT_OFFSET;
  p = compat_d3d_push_reserve(4);
  p = pb_push1(p, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE, 0);
  p = pb_push1(p, NV097_GET_REPORT,
               report_offset | (NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT << 24));
  compat_d3d_push_end(p);
  /* 0x1e8aba: KickOff, so the report gets written while the game polls. */
  compat_d3d_push_kick();
  return D3D_OK;
}

int __stdcall D3DDevice_GetVisibilityTestResult(unsigned int index,
                                                unsigned int *result,
                                                unsigned int *timestamp)
{
  unsigned long *report;

  report = visibility_report_get(index);
  if (report == NULL) {
    return E_OUTOFMEMORY;
  }
  if (*(unsigned long *)((unsigned char *)report +
                         VISIBILITY_REPORT_STATUS_OFFSET) == 0xffffffff) {
    return D3DERR_VISIBILITY_PENDING;
  }
  if (result) {
    *result = *(unsigned long *)((unsigned char *)report +
                                 VISIBILITY_REPORT_RESULT_OFFSET);
  }
  if (timestamp) {
    timestamp[0] = report[0];
    timestamp[1] = report[1];
  }
  return D3D_OK;
}

/* The back and front buffer headers keep their slots (the game holds the back
 * buffer as its render target); their bits trade places and the new front is
 * scanned out.
 *
 * The scanout address latches at the next vblank, and the device has a
 * single back buffer: until then the new back buffer is still on screen. The
 * XDK (0x1ee931) lets the CPU run two swaps ahead because its GPU waits for
 * the flip before rendering; this one renders immediately, so it returns only
 * once the vblank thread has seen the flip, unless the game asked for
 * D3DPRESENT_INTERVAL_IMMEDIATE (tearing allowed).
 *
 * D3DPRESENT_INTERVAL_TWO keeps each frame up for at least two vblanks: the
 * flip waits until one vblank has passed since the last one latched. */
void __stdcall D3DDevice_Present(void *source_rect, void *dest_rect,
                                 void *window_override, void *dirty_region)
{
  unsigned long *back;
  unsigned long *front;
  unsigned long bits;

  (void)source_rect;
  (void)dest_rect;
  (void)window_override;
  (void)dirty_region;
  compat_d3d_trace_mark("present");
  if (d3d_pbkit_ready) {
    /* The frame must be complete before its bits are scanned out, and pbkit
     * only rewinds its push buffer on request. The rewind waits for the
     * FIFO to drain and PGRAPH to go idle. */
    compat_d3d_push_rewind();
  }
  while (d3d_vblank_count - d3d_flip_vblank + 1 < d3d_present_vblanks) {
    wait_for_vblank();
  }
  back = buffer_surface(DEVICE_BACK_BUFFER_SLOT);
  front = buffer_surface(DEVICE_FRONT_BUFFER_SLOT);
  bits = back[1];
  back[1] = front[1];
  front[1] = bits;
  d3d_targets_dirty = 1;
  XVideoSetFB((unsigned char *)bits);
  (*device_field(DEVICE_SWAP_COUNT))++;
  while (d3d_present_vblanks &&
         *device_field(DEVICE_SWAP_COUNT) != *device_field(DEVICE_FLIP_COUNT)) {
    wait_for_vblank();
  }
}

void __stdcall D3DDevice_SetVerticalBlankCallback(void *callback)
{
  *device_field(DEVICE_VBLANK_CALLBACK) = (unsigned long)callback;
}

void D3DDevice_BlockUntilVerticalBlank(void)
{
  wait_for_vblank();
}

/* D3DX (0x1ff03f): out = in * matrix, row-vector convention. */
float *__stdcall mat4x4_transform_vec4(float *out, float *in, float *matrix)
{
  float x;
  float y;
  float z;
  float w;
  int i;

  x = in[0];
  y = in[1];
  z = in[2];
  w = in[3];
  for (i = 0; i < 4; i++) {
    out[i] = x * matrix[i] + y * matrix[4 + i] + z * matrix[8 + i] +
             w * matrix[12 + i];
  }
  return out;
}

/* D3DX (0x1ff913): left-handed perspective from view-volume width/height at
 * the near plane (D3DXMatrixPerspectiveLH; the kb parameter names say fov and
 * aspect, but the code divides 2*z_near by them directly). */
void __stdcall matrix_build_perspective_projection(void *matrix_out, float fov,
                                                   float aspect, float z_near,
                                                   float z_far)
{
  float *m;
  float q;

  m = (float *)matrix_out;
  compat_memset(m, 0, 16 * sizeof(float));
  m[0] = (z_near + z_near) / fov;
  m[5] = (z_near + z_near) / aspect;
  q = z_far / (z_far - z_near);
  m[10] = q;
  m[11] = 1.0f;
  m[14] = -(q * z_near);
}

/* D3DX (0x1ffc57): left-handed orthographic projection (D3DXMatrixOrthoLH). */
void __stdcall matrix_build_ortho_projection(void *matrix_out, float scale_x,
                                             float scale_y, float z_near,
                                             float z_far)
{
  float *m;
  float q;

  m = (float *)matrix_out;
  compat_memset(m, 0, 16 * sizeof(float));
  m[0] = 2.0f / scale_x;
  m[5] = 2.0f / scale_y;
  q = 1.0f / (z_far - z_near);
  m[10] = q;
  m[14] = -(q * z_near);
  m[15] = 1.0f;
}

/* Reference counts of the targets are not adjusted yet. A NULL render
 * target keeps the current one; a NULL depth buffer disables it. */
void __stdcall D3DDevice_SetRenderTarget(void *render_target,
                                         void *depth_stencil)
{
  uint32_t *p;

  if (render_target) {
    d3d_render_target = (unsigned long *)render_target;
  }
  d3d_depth_stencil = (unsigned long *)depth_stencil;
  d3d_targets_dirty = 1;
  /* The XDK keeps the current targets at device+0x40c/+0x410 (0x1e7bda,
   * 0x1e7bff); the render-state setters read them there. */
  *device_field(DEVICE_RENDER_TARGET) =
    (unsigned long)d3d_render_target;
  *device_field(DEVICE_DEPTH_STENCIL) =
    (unsigned long)d3d_depth_stencil;
  compat_d3d_set_depth_scale(d3d_depth_stencil);
  /* 0x1e7c1a..0x1e7cc2: the surface is programmed here, not at the next
   * Clear. Offscreen passes that never clear (the sun-glow targets) would
   * otherwise draw into the previous surface. Depth and stencil tests are
   * re-gated on the new depth buffer (0x1fb884 / 0x1fb888 && zeta). */
  if (d3d_pbkit_ready && d3d_render_target) {
    gpu_bind_targets();
    p = compat_d3d_push_reserve(4);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE,
                 (D3D_RENDER_STATE[D3DRS_ZENABLE] && d3d_depth_stencil) ? 1 : 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE,
                 (D3D_RENDER_STATE[D3DRS_STENCILENABLE] && d3d_depth_stencil) ? 1 : 0);
    compat_d3d_push_end(p);
  }
}

void __stdcall D3DDevice_Clear(uint32_t count, void *rects, uint32_t flags,
                               uint32_t color, float z, uint32_t stencil)
{
  const d3d_rect *rect;
  unsigned long keep;
  unsigned long depth;
  uint32_t i;

  compat_d3d_trace_mark("clear");
  keep = 0;
  if (!(flags & D3DCLEAR_TARGET_A)) {
    keep |= 0xff000000ul;
  }
  if (!(flags & D3DCLEAR_TARGET_R)) {
    keep |= 0x00ff0000ul;
  }
  if (!(flags & D3DCLEAR_TARGET_G)) {
    keep |= 0x0000ff00ul;
  }
  if (!(flags & D3DCLEAR_TARGET_B)) {
    keep |= 0x000000fful;
  }
  depth = 0;
  if (d3d_depth_stencil) {
    depth = depth_clear_value(d3d_depth_stencil, z, stencil);
  }
  rect = (const d3d_rect *)rects;
  if (d3d_pbkit_ready && d3d_render_target) {
    unsigned long swizzled_format;
    uint32_t *p;

    if (d3d_targets_dirty) {
      gpu_bind_targets();
    }
    /* 0x1ea669..0x1ea6ad: the NV2A only clears pitch surfaces. For a
     * swizzled target the XDK reprograms SET_SURFACE_FORMAT with the type
     * switched from SWIZZLE to PITCH (and ~0x200, or 0x100) for the clear,
     * and 0x1ea9a8..0x1ea9c2 restores the swizzled format afterwards. */
    swizzled_format = surface_format(d3d_render_target, d3d_depth_stencil);
    if (((swizzled_format >> SURFACE_FORMAT_TYPE_SHIFT) & 0xf) !=
        NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
      swizzled_format = 0;
    }
    if (swizzled_format) {
      p = compat_d3d_push_reserve(2);
      p = pb_push1(p, NV097_SET_SURFACE_FORMAT,
                   (swizzled_format &
                    ~(NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE
                      << SURFACE_FORMAT_TYPE_SHIFT)) |
                     (NV097_SET_SURFACE_FORMAT_TYPE_PITCH
                      << SURFACE_FORMAT_TYPE_SHIFT));
      compat_d3d_push_end(p);
    }
    if (!d3d_depth_stencil) {
      flags &= ~(uint32_t)(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    }
    for (i = 0; i == 0 || i < count; i++) {
      gpu_clear(count ? &rect[i] : 0, flags, color, depth);
    }
    if (swizzled_format) {
      p = compat_d3d_push_reserve(2);
      p = pb_push1(p, NV097_SET_SURFACE_FORMAT, swizzled_format);
      compat_d3d_push_end(p);
    }
    return;
  }
  for (i = 0; i == 0 || i < count; i++) {
    const d3d_rect *r = count ? &rect[i] : 0;

    if (keep != 0xfffffffful && d3d_render_target) {
      surface_fill(d3d_render_target, r, color, keep);
    }
    if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && d3d_depth_stencil) {
      surface_fill(d3d_depth_stencil, r, depth,
                   (flags & D3DCLEAR_ZBUFFER ? 0 : ~0xfful) |
                     (flags & D3DCLEAR_STENCIL ? 0 : 0xfful));
    }
  }
}
