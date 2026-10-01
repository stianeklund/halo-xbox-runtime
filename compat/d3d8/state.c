/*
 * Standalone compat: Direct3D 8 (Xbox) render-state and texture-state setters.
 *
 * Each setter is transcribed from the XDK routine in cachebeta.xbe (VA in the
 * comment above it): the same NV2A methods and values are pushed, and the
 * same D3D__RenderState / D3D__TextureState mirror, dirty-flag and device
 * field stores are made, because the game reads the mirrors back. Only the
 * XDK's own push-buffer management (MakeSpace 0x1efe10 and the put pointer at
 * device+0) is replaced by the compat push buffer (draw.c).
 */
#include "internal.h"

/* Device fields (dword offsets are byte offsets / 4 where indexed). */
#define DEVICE_TEXGEN_MASK 0x4fc
#define DEVICE_FRONT_BUFFER_SURFACE 0x2168
#define DEVICE_SHADER_CONSTANT_MODE 0x2140
#define DEVICE_REGISTER_BASE 0x2308
#define DEVICE_DEBUG_5_SHADOW 0x2b44
#define DEVICE_400B80_SHADOW 0x2b48

/* Xbox D3DRS_* indices of the mirror (from the dispatch table at 0x1f9c58
 * and each setter's mirror store). */
#define D3DRS_ZBIAS_SCALE 0x4d
#define D3DRS_ZBIAS_OFFSET 0x4e
#define D3DRS_POINT_OFFSET_ENABLE 0x4f
#define D3DRS_LINE_OFFSET_ENABLE 0x50
#define D3DRS_FILL_OFFSET_ENABLE 0x51
#define D3DRS_SIMPLE_COUNT 0x52
#define D3DRS_DEFERRED_COUNT 0x74
#define D3DRS_FOGCOLOR 0x76
#define D3DRS_FILLMODE 0x77
#define D3DRS_BACKFILLMODE 0x78
#define D3DRS_STENCILFAIL 0x7d
#define D3DRS_FRONTFACE 0x7e
#define D3DRS_CULLMODE 0x7f
#define D3DRS_ZBIAS 0x81
#define D3DRS_LOGICOP 0x82
#define D3DRS_EDGEANTIALIAS 0x83
#define D3DRS_MULTISAMPLEANTIALIAS 0x84
#define D3DRS_MULTISAMPLEMASK 0x85
#define D3DRS_MULTISAMPLETYPE 0x86
#define D3DRS_SHADOWFUNC 0x87
#define D3DRS_LINEWIDTH 0x88
#define D3DRS_DXT1NOISEENABLE 0x89
#define D3DRS_YUVENABLE 0x8a
#define D3DRS_OCCLUSIONCULLENABLE 0x8b
#define D3DRS_STENCILCULLENABLE 0x8c
#define D3DRS_ROPZCMPALWAYSREAD 0x8d
#define D3DRS_ROPZREAD 0x8e
#define D3DRS_DONOTCULLUNCOMPRESSED 0x8f
#define D3DRS_COMPLEX_END 0x90

#define D3DTSS_BORDERCOLOR 0x1d
#define D3DTSS_COLORKEYCOLOR 0x1e

#define D3DSTENCILOP_KEEP 0x1e00
#define D3DCULL_NONE 0
#define D3DCMP_NEVER 0x200

/* Methods pbkit's nv_regs.h has no name for (nv_objects.h: MULTISAMPLE and
 * CULL_ENABLE). */
#define NV097_SET_ANTI_ALIASING_CONTROL 0x00001d7c
#define NV097_SET_OCCLUSION_CULL_ENABLE 0x00001d84
/* 0x1e9bf5: texture unit u, state t -> 0x1ad0 + (16 u + t) * 4, i.e.
 * BUMP_ENV_MAT of unit u for t = D3DTSS_BUMPENVMAT00. */
#define BUMP_ENV_METHOD_BASE (NV097_SET_TEXTURE_SET_BUMP_ENV_MAT - D3DTSS_BUMPENVMAT00 * 4)
#define TEXGEN_STAGE_STRIDE 0x10
#define TEXGEN_PLANE_STAGE_STRIDE 0x40
#define BORDER_COLOR_STAGE_STRIDE 0x40
#define COLOR_KEY_STAGE_STRIDE 4

/* The XDK raises a PGRAPH software method with NV097_NO_OPERATION and a
 * non-zero parameter; its interrupt handler runs the subprogram with
 * PARAMETER_A/B (NV097_SET_ZSTENCIL_CLEAR_VALUE / _COLOR_CLEAR_VALUE). pbkit's
 * interrupt handler implements the same two subprograms under its own ids
 * (pbkit.c PB_SETOUTER / PB_SETNOISE). */
#define XDK_SUBPROGRAM_WRITE_REGISTER 0x320 /* 0x1ea258 */
#define XDK_SUBPROGRAM_DXT1_NOISE 0x31c     /* 0x1e98bf */
#define PBKIT_SUBPROGRAM_WRITE_REGISTER 0xb2a
#define PBKIT_SUBPROGRAM_DXT1_NOISE 0xbaa
#define NV_PGRAPH_DEBUG_5 0x00400094
#define NV_PGRAPH_400B80 0x00400b80

/* AvSendTVEncoderOption options (nxdk hal/video.h names). */
#define VIDEO_ENC_FLICKERFILTER 11
#define VIDEO_ENC_SOFTEN_FILTER 14

#define CONTROL0_BASE (NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE | \
                       NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE)

/* D3DDevice_SetRenderStateNotInline (0x1e93a0): method headers of the simple
 * render states 0..0x51, D3D__RenderState_Simple table at 0x282b90. */
static const unsigned long d3d_simple_render_state_methods[D3DRS_SIMPLE_COUNT] = {
  0x00040260, 0x00040264, 0x00040268, 0x0004026c, 0x00040270, 0x00040274, /* 0x00 */
  0x00040278, 0x0004027c, 0x00040288, 0x0004028c, 0x00040a60, 0x00040a64, /* 0x06 */
  0x00040a68, 0x00040a6c, 0x00040a70, 0x00040a74, 0x00040a78, 0x00040a7c, /* 0x0c */
  0x00040a80, 0x00040a84, 0x00040a88, 0x00040a8c, 0x00040a90, 0x00040a94, /* 0x12 */
  0x00040a98, 0x00040a9c, 0x00040aa0, 0x00040aa4, 0x00040aa8, 0x00040aac, /* 0x18 */
  0x00040ab0, 0x00040ab4, 0x00040ab8, 0x00040abc, 0x00040ac0, 0x00040ac4, /* 0x1e */
  0x00040ac8, 0x00040acc, 0x00040ad0, 0x00040ad4, 0x00040ad8, 0x00040adc, /* 0x24 */
  0x000417f8, 0x00041e20, 0x00041e24, 0x00041e40, 0x00041e44, 0x00041e48, /* 0x2a */
  0x00041e4c, 0x00041e50, 0x00041e54, 0x00041e58, 0x00041e5c, 0x00041e60, /* 0x30 */
  0x00041d90, 0x00041e74, 0x00041e78, 0x00040354, 0x0004033c, 0x00040304, /* 0x36 */
  0x00040300, 0x00040340, 0x00040344, 0x00040348, 0x0004035c, 0x00040310, /* 0x3c */
  0x0004037c, 0x00040358, 0x00040374, 0x00040378, 0x00040364, 0x00040368, /* 0x42 */
  0x0004036c, 0x00040360, 0x00040350, 0x0004034c, 0x000409f8, 0x00040384, /* 0x48 */
  0x00040388, 0x00040330, 0x00040334, 0x00040338,                         /* 0x4e */
};

/* Dirty flags of the deferred render states 0x52..0x73 (0x1f9d50[index],
 * i.e. 0x1f9e98..0x1f9f1c). */
static const unsigned long d3d_deferred_render_state_dirty[D3DRS_DEFERRED_COUNT -
                                                           D3DRS_SIMPLE_COUNT] = {
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x10, 0x10, 0x10, 0x10, 0x22, 0x60, /* 0x52 */
  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, /* 0x5e */
  0x01, 0x01, 0x09, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,             /* 0x6a */
};

/* Per-D3DFORMAT info bytes (0x1f9d58, formats 0..0x41); Dxt1NoiseEnable
 * tests (byte & 0x3c) == 0x20. */
#define FORMAT_INFO_COUNT 0x42
#define FORMAT_INFO_SIZE_MASK 0x3c
#define FORMAT_INFO_SIZE_32BPP 0x20
static const unsigned char d3d_format_info[FORMAT_INFO_COUNT] = {
  0x09, 0x11, 0x11, 0x91, 0x11, 0x91, 0xa1, 0xa1, 0x00, 0x00, 0x00, 0x09, 0x04, 0x00, 0x08, 0x08,
  0x12, 0x92, 0xa2, 0x8a, 0x00, 0x00, 0x12, 0x92, 0x00, 0x09, 0x11, 0x0a, 0x92, 0x12, 0xa2, 0x0a,
  0x12, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x11, 0x11, 0x11, 0x61, 0x61, 0x51, 0x51, 0x62, 0x62,
  0x52, 0x52, 0x11, 0x21, 0x00, 0x12, 0x00, 0x12, 0x11, 0x11, 0x21, 0x21, 0x21, 0x12, 0x12, 0x22,
  0x22, 0x22,
};

/* D3DDevice_SetShaderConstantMode: 12 transform constants (0x1f9f9c). */
static const unsigned long d3d_shader_constant_defaults[12] = {
  0x00000000, 0x3f000000, 0x3f800000, 0x40000000, 0xbf800000, 0x00000000,
  0x3f800000, 0x40000000, 0x00000000, 0x00000000, 0xbf800000, 0x00000000,
};

/* D3DFMT_* depth formats (0x1e9fad): D24S8, F24S8 and their linear forms. */
#define D3DFMT_D24S8 0x2a
#define D3DFMT_F24S8 0x2b
#define D3DFMT_F16 0x2d
#define D3DFMT_LIN_F16 0x31

#define SURFACE_FORMAT_BYTE 0x0d

static unsigned long surface_format(unsigned long surface)
{
  return ((const unsigned char *)surface)[SURFACE_FORMAT_BYTE];
}

/* Device fields the XDK sets up while creating the device; called from
 * Direct3D_CreateDevice after pb_init. */
void compat_d3d_state_initialize(void)
{
  dword_float one;

  /* 0x1f8ef8..0x1f8f4e: the register base (device+0x2308) block also holds
   * the initial NV_PGRAPH_DEBUG_5 (4) and 0x400b80 values, which
   * D3D_CommonSetDebugRegisters edits; 0x400b80 depends on the PBUS ROM
   * version bit (as in pbkit's own init). */
  *device_field(DEVICE_REGISTER_BASE) = 0xfd000000;
  *device_field(DEVICE_DEBUG_5_SHADOW) = 4;
  *device_field(DEVICE_400B80_SHADOW) =
    (*(volatile unsigned long *)(0xfd000000 + 0x1218) & 0x100) ? 0x45ead10f : 0x45ead10e;
  /* FUN_001e9fe0 (run by SetRenderTarget): scale 1.0 on a target that is not
   * super-sampled. */
  one.f = 1.0f;
  *device_field(DEVICE_SUPER_SAMPLE_SCALE_X) = one.u;
  *device_field(DEVICE_SUPER_SAMPLE_SCALE_Y) = one.u;
  *device_field(DEVICE_SUPER_SAMPLE_SCALE) = one.u;
}

/* 0x1e9350 (ecx = method header, edx = value). */
void D3DDevice_SetRenderState_Simple(uint32_t reg, uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(2);
  p[0] = reg;
  p[1] = value;
  compat_d3d_push_end(p + 2);
}

/* 0x1e9380 (ecx = render state index, edx = value). The original indexes its
 * table unchecked; only the deferred range 0x52..0x73 is defined. */
void D3DDevice_SetRenderState_Deferred(uint32_t reg_index, uint32_t value)
{
  if (reg_index >= D3DRS_SIMPLE_COUNT && reg_index < D3DRS_DEFERRED_COUNT) {
    D3D_DEVICE[DEVICE_DIRTY / 4] |=
      d3d_deferred_render_state_dirty[reg_index - D3DRS_SIMPLE_COUNT];
  }
  D3D_RENDER_STATE[reg_index] = value;
}

typedef void(__stdcall *render_state_setter)(uint32_t value);

/* Complex render states 0x74..0x8f (0x1f9c58[index], i.e. 0x1f9e28). */
static const render_state_setter d3d_complex_render_state_setters[D3DRS_COMPLEX_END -
                                                                  D3DRS_DEFERRED_COUNT] = {
  D3DDevice_SetRenderState_PSTextureModes,
  D3DDevice_SetRenderState_VertexBlend,
  D3DDevice_SetRenderState_FogColor,
  D3DDevice_SetRenderState_FillMode,
  D3DDevice_SetRenderState_BackFillMode,
  D3DDevice_SetRenderState_TwoSidedLighting,
  D3DDevice_SetRenderState_NormalizeNormals,
  D3DDevice_SetRenderState_ZEnable,
  D3DDevice_SetRenderState_StencilEnable,
  D3DDevice_SetRenderState_StencilFail,
  D3DDevice_SetRenderState_FrontFace,
  D3DDevice_SetRenderState_CullMode,
  D3DDevice_SetRenderState_TextureFactor,
  D3DDevice_SetRenderState_ZBias,
  D3DDevice_SetRenderState_LogicOp,
  D3DDevice_SetRenderState_EdgeAntiAlias,
  D3DDevice_SetRenderState_MultiSampleAntiAlias,
  D3DDevice_SetRenderState_MultiSampleMask,
  D3DDevice_SetRenderState_MultiSampleType,
  D3DDevice_SetRenderState_ShadowFunc,
  D3DDevice_SetRenderState_LineWidth,
  D3DDevice_SetRenderState_Dxt1NoiseEnable,
  D3DDevice_SetRenderState_YuvEnable,
  D3DDevice_SetRenderState_OcclusionCullEnable,
  D3DDevice_SetRenderState_StencilCullEnable,
  D3DDevice_SetRenderState_RopZCmpAlwaysRead,
  D3DDevice_SetRenderState_RopZRead,
  D3DDevice_SetRenderState_DoNotCullUncompressed,
};

/* 0x1e93a0. Indices past the complex range call through whatever follows
 * the original table; they are ignored here. */
void __stdcall D3DDevice_SetRenderStateNotInline(uint32_t state_index, uint32_t value)
{
  if ((int)state_index < D3DRS_SIMPLE_COUNT) {
    D3DDevice_SetRenderState_Simple(d3d_simple_render_state_methods[state_index], value);
    D3D_RENDER_STATE[state_index] = value;
    return;
  }
  if ((int)state_index < D3DRS_DEFERRED_COUNT) {
    D3D_DEVICE[DEVICE_DIRTY / 4] |=
      d3d_deferred_render_state_dirty[state_index - D3DRS_SIMPLE_COUNT];
    D3D_RENDER_STATE[state_index] = value;
    return;
  }
  if (state_index < D3DRS_COMPLEX_END) {
    d3d_complex_render_state_setters[state_index - D3DRS_DEFERRED_COUNT](value);
  }
}

/* 0x1e9320: no push; the pixel-shader path picks it up through the dirty
 * flag. */
void __stdcall D3DDevice_SetRenderState_PSTextureModes(uint32_t modes)
{
  unsigned long *d;

  d = D3D_DEVICE;
  d[DEVICE_PS_TEXTURE_MODES / 4] = modes;
  d[DEVICE_DIRTY / 4] |= 0x400;
  D3D_RENDER_STATE[D3DRS_PSTEXTUREMODES] = modes;
}

/* 0x1e92f0: re-applies the current targets under the new type. */
void __stdcall D3DDevice_SetRenderState_MultiSampleType(uint32_t value)
{
  D3D_RENDER_STATE[D3DRS_MULTISAMPLETYPE] = value;
  D3DDevice_SetRenderTarget((void *)*device_field(DEVICE_RENDER_TARGET),
                            (void *)*device_field(DEVICE_DEPTH_STENCIL));
}

/* 0x1e9620: line and polygon smoothing together. */
void __stdcall D3DDevice_SetRenderState_EdgeAntiAlias(uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(3);
  p[0] = METHOD(NV097_SET_LINE_SMOOTH_ENABLE, 2);
  p[1] = value;
  p[2] = value;
  compat_d3d_push_end(p + 3);
  D3D_RENDER_STATE[D3DRS_EDGEANTIALIAS] = value;
}

/* 0x1e9650: D3DCMP_* (0x200 based) to the GL compare function. */
void __stdcall D3DDevice_SetRenderState_ShadowFunc(uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_SHADOW_COMPARE_FUNC, 1);
  p[1] = value - D3DCMP_NEVER;
  compat_d3d_push_end(p + 2);
  D3D_RENDER_STATE[D3DRS_SHADOWFUNC] = value;
}

/* 0x1e9680: ARGB with red and blue swapped. */
void __stdcall D3DDevice_SetRenderState_FogColor(uint32_t value)
{
  uint32_t *p;
  unsigned long color;

  color = ((value >> 16) & 0xff) | ((value & 0xff) << 16) | (value & 0xff00ff00);
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_FOG_COLOR, 1);
  p[1] = color;
  compat_d3d_push_end(p + 2);
  D3D_RENDER_STATE[D3DRS_FOGCOLOR] = value;
}

/* 0x1e96d0: culling on unless D3DCULL_NONE; the face to cull is the one
 * whose winding differs from D3DRS_FRONTFACE. */
void __stdcall D3DDevice_SetRenderState_CullMode(uint32_t mode)
{
  uint32_t *p;

  p = compat_d3d_push_begin(4);
  p[0] = METHOD(NV097_SET_CULL_FACE_ENABLE, 1);
  if (mode == D3DCULL_NONE) {
    p[1] = mode;
    compat_d3d_push_end(p + 2);
    D3D_RENDER_STATE[D3DRS_CULLMODE] = mode;
    return;
  }
  p[1] = 1;
  p[2] = METHOD(NV097_SET_CULL_FACE, 1);
  p[3] = NV097_SET_CULL_FACE_V_FRONT + (mode != D3D_RENDER_STATE[D3DRS_FRONTFACE]);
  compat_d3d_push_end(p + 4);
  D3D_RENDER_STATE[D3DRS_CULLMODE] = mode;
}

/* 0x1e9740: then re-derives the culled face (tail jump into CullMode). */
void __stdcall D3DDevice_SetRenderState_FrontFace(uint32_t value)
{
  uint32_t *p;
  unsigned long cull_mode;

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_FRONT_FACE, 1);
  p[1] = value;
  compat_d3d_push_end(p + 2);
  cull_mode = D3D_RENDER_STATE[D3DRS_CULLMODE];
  D3D_RENDER_STATE[D3DRS_FRONTFACE] = value;
  D3DDevice_SetRenderState_CullMode(cull_mode);
}

/* 0x1e9780 */
void __stdcall D3DDevice_SetRenderState_NormalizeNormals(uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_NORMALIZATION_ENABLE, 1);
  p[1] = value;
  compat_d3d_push_end(p + 2);
  D3D_DEVICE[DEVICE_DIRTY / 4] |= 2;
  D3D_RENDER_STATE[D3DRS_NORMALIZENORMALS] = value;
}

/* 0x1e97b0: only pushed while no pixel shader is set (a pixel shader loads
 * its own combiner factors). */
void __stdcall D3DDevice_SetRenderState_TextureFactor(uint32_t value)
{
  uint32_t *p;
  int i;

  if (*device_field(DEVICE_PIXEL_SHADER) == 0) {
    p = compat_d3d_push_begin(17);
    p[0] = METHOD(NV097_SET_COMBINER_FACTOR0, 16);
    for (i = 0; i < 16; i++) {
      p[1 + i] = value;
    }
    compat_d3d_push_end(p + 17);
  }
  D3D_RENDER_STATE[D3DRS_TEXTUREFACTOR] = value;
}

/* 0x1e9800: float width * super-sample scale * 8, rounded, capped at 0x1ff. */
void __stdcall D3DDevice_SetRenderState_LineWidth(uint32_t value)
{
  uint32_t *p;
  dword_float width;
  dword_float scale;
  float scaled;
  unsigned long fixed;

  width.u = value;
  scale.u = *device_field(DEVICE_SUPER_SAMPLE_SCALE);
  scaled = width.f * scale.f * 8.0f + 0.5f;
  fixed = (unsigned long)(long)scaled; /* 0x1f3c30: cvttss2si */
  if (fixed > 0x1ff) {
    fixed = 0x1ff;
  }
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_LINE_WIDTH, 1);
  p[1] = fixed;
  compat_d3d_push_end(p + 2);
  D3D_RENDER_STATE[D3DRS_LINEWIDTH] = value;
}

/* 0x1e9860: only honored on a 32-bit render target; pushed when it changes
 * device flag bit 0. */
void __stdcall D3DDevice_SetRenderState_Dxt1NoiseEnable(uint32_t value)
{
  unsigned long *d;
  unsigned long format;
  unsigned long enable;
  uint32_t *p;

  d = D3D_DEVICE;
  format = surface_format(d[DEVICE_RENDER_TARGET / 4]);
  enable = 0;
  if (format < FORMAT_INFO_COUNT &&
      (d3d_format_info[format] & FORMAT_INFO_SIZE_MASK) == FORMAT_INFO_SIZE_32BPP) {
    enable = value;
  }
  if (enable != (d[DEVICE_STATE_FLAGS / 4] & 1)) {
    d[DEVICE_STATE_FLAGS / 4] ^= 1;
    p = compat_d3d_push_begin(6);
    p[0] = METHOD(NV097_WAIT_FOR_IDLE, 1);
    p[1] = 0;
    p[2] = METHOD(NV097_SET_ZSTENCIL_CLEAR_VALUE, 1);
    p[3] = enable;
    p[4] = METHOD(NV097_NO_OPERATION, 1);
    p[5] = PBKIT_SUBPROGRAM_DXT1_NOISE; /* XDK: XDK_SUBPROGRAM_DXT1_NOISE */
    compat_d3d_push_end(p + 6);
  }
  D3D_RENDER_STATE[D3DRS_DXT1NOISEENABLE] = value;
}

/* 0x1e98e0: sets the polygon offset states through NotInline. */
void __stdcall D3DDevice_SetRenderState_ZBias(uint32_t value)
{
  unsigned long enable;
  dword_float bias;
  dword_float scale;

  enable = value != 0;
  bias.f = -(float)value;
  scale.f = bias.f * 0.25f;
  D3DDevice_SetRenderStateNotInline(D3DRS_ZBIAS_SCALE, scale.u);
  D3DDevice_SetRenderStateNotInline(D3DRS_ZBIAS_OFFSET, bias.u);
  D3DDevice_SetRenderStateNotInline(D3DRS_POINT_OFFSET_ENABLE, enable);
  D3DDevice_SetRenderStateNotInline(D3DRS_LINE_OFFSET_ENABLE, enable);
  D3DDevice_SetRenderStateNotInline(D3DRS_FILL_OFFSET_ENABLE, enable);
  D3D_RENDER_STATE[D3DRS_ZBIAS] = value;
}

/* 0x1e9960 */
void __stdcall D3DDevice_SetRenderState_LogicOp(uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(3);
  if (value == 0) {
    p[0] = METHOD(NV097_SET_LOGIC_OP_ENABLE, 1);
    p[1] = value;
    compat_d3d_push_end(p + 2);
  } else {
    p[0] = METHOD(NV097_SET_LOGIC_OP_ENABLE, 2);
    p[1] = 1;
    p[2] = value;
    compat_d3d_push_end(p + 3);
  }
  D3D_RENDER_STATE[D3DRS_LOGICOP] = value;
}

/* Front/back polygon modes; the back mode is the front one unless two-sided
 * lighting is on (0x1e99b0, 0x1e99f0, 0x1e9a40). */
static void push_polygon_modes(unsigned long front)
{
  uint32_t *p;
  unsigned long back;

  back = D3D_RENDER_STATE[D3DRS_TWOSIDEDLIGHTING] ? D3D_RENDER_STATE[D3DRS_BACKFILLMODE] : front;
  p = compat_d3d_push_begin(3);
  p[0] = METHOD(NV097_SET_FRONT_POLYGON_MODE, 2);
  p[1] = front;
  p[2] = back;
  compat_d3d_push_end(p + 3);
}

/* 0x1e99b0 */
void __stdcall D3DDevice_SetRenderState_FillMode(uint32_t value)
{
  push_polygon_modes(value);
  D3D_RENDER_STATE[D3DRS_FILLMODE] = value;
}

/* 0x1e99f0 */
void __stdcall D3DDevice_SetRenderState_BackFillMode(uint32_t value)
{
  unsigned long front;

  front = D3D_RENDER_STATE[D3DRS_FILLMODE];
  D3D_RENDER_STATE[D3DRS_BACKFILLMODE] = value;
  push_polygon_modes(front);
  D3D_RENDER_STATE[D3DRS_FILLMODE] = front;
}

/* 0x1e9a40 */
void __stdcall D3DDevice_SetRenderState_TwoSidedLighting(uint32_t value)
{
  unsigned long front;

  D3D_DEVICE[DEVICE_DIRTY / 4] |= 0x20;
  front = D3D_RENDER_STATE[D3DRS_FILLMODE];
  D3D_RENDER_STATE[D3DRS_TWOSIDEDLIGHTING] = value;
  push_polygon_modes(front);
  D3D_RENDER_STATE[D3DRS_FILLMODE] = front;
}

/* 0x1e9aa0 */
void __stdcall D3DDevice_SetRenderState_VertexBlend(uint32_t value)
{
  uint32_t *p;

  D3D_DEVICE[DEVICE_DIRTY / 4] |= 2;
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_SKIN_MODE, 1);
  p[1] = value;
  compat_d3d_push_end(p + 2);
  D3D_RENDER_STATE[D3DRS_VERTEXBLEND] = value;
}

/* FUN_001e9f80 (ecx = device, edx = push pointer): NV097_SET_CONTROL0 from
 * the YUV and w-buffer states and the depth buffer format. */
static uint32_t *push_control0(uint32_t *p)
{
  unsigned long control;
  unsigned long depth;
  unsigned long format;

  control = CONTROL0_BASE;
  if (D3D_RENDER_STATE[D3DRS_YUVENABLE]) {
    control |= NV097_SET_CONTROL0_COLOR_SPACE_CONVERT_CRYCB_TO_RGB;
  }
  if (D3D_RENDER_STATE[D3DRS_ZENABLE] == D3DZB_USEW) {
    control |= NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE;
  }
  depth = *device_field(DEVICE_DEPTH_STENCIL);
  if (depth) {
    format = surface_format(depth);
    if (format == D3DFMT_F16 || format == D3DFMT_F24S8 || format == D3DFMT_LIN_F16 ||
        format == D3DFMT_LIN_F24S8) {
      control |= NV097_SET_CONTROL0_Z_FORMAT_FLOAT;
    }
  }
  p[0] = METHOD(NV097_SET_CONTROL0, 1);
  p[1] = control;
  return p + 2;
}

/* 0x1ea290. Switching into or out of w-buffering also redoes the viewport
 * transform and the passthrough program's constants (FUN_001eb6f0, ecx =
 * device; draw.c compat_d3d_push_passthrough_program). */
void __stdcall D3DDevice_SetRenderState_ZEnable(uint32_t enable)
{
  uint32_t *p;
  unsigned long previous;

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_DEPTH_TEST_ENABLE, 1);
  p[1] = (enable && *device_field(DEVICE_DEPTH_STENCIL)) ? 1 : 0;
  compat_d3d_push_end(p + 2);
  previous = D3D_RENDER_STATE[D3DRS_ZENABLE];
  D3D_RENDER_STATE[D3DRS_ZENABLE] = enable;
  if (previous == D3DZB_USEW || enable == D3DZB_USEW) {
    D3D_UpdateProjectionViewportTransform();
    compat_d3d_push_passthrough_program();
    p = compat_d3d_push_begin(2);
    compat_d3d_push_end(push_control0(p));
  }
}

/* Inline at 0x1ea30d and friends (also out of line at 0x1ea170): bit 1 stencil
 * cull, bit 0 occlusion cull unless a stencil test could discard pixels. */
static unsigned long occlusion_cull_bits(void)
{
  unsigned long bits;

  bits = D3D_RENDER_STATE[D3DRS_STENCILCULLENABLE] ? 2 : 0;
  if (D3D_RENDER_STATE[D3DRS_OCCLUSIONCULLENABLE] &&
      (!D3D_RENDER_STATE[D3DRS_STENCILENABLE] ||
       D3D_RENDER_STATE[D3DRS_STENCILFAIL] == D3DSTENCILOP_KEEP)) {
    bits |= 1;
  }
  return bits;
}

/* 0x1ea300: the cull bits are computed from the previous stencil state. */
void __stdcall D3DDevice_SetRenderState_StencilEnable(uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(4);
  p[0] = METHOD(NV097_SET_OCCLUSION_CULL_ENABLE, 1);
  p[1] = occlusion_cull_bits();
  p[2] = METHOD(NV097_SET_STENCIL_TEST_ENABLE, 1);
  p[3] = (value && *device_field(DEVICE_DEPTH_STENCIL)) ? 1 : 0;
  compat_d3d_push_end(p + 4);
  D3D_RENDER_STATE[D3DRS_STENCILENABLE] = value;
}

/* 0x1ea380: the cull bits are computed from the previous stencil state. */
void __stdcall D3DDevice_SetRenderState_StencilFail(uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(4);
  p[0] = METHOD(NV097_SET_OCCLUSION_CULL_ENABLE, 1);
  p[1] = occlusion_cull_bits();
  p[2] = METHOD(NV097_SET_STENCIL_OP_FAIL, 1);
  p[3] = value;
  compat_d3d_push_end(p + 4);
  D3D_RENDER_STATE[D3DRS_STENCILFAIL] = value;
}

/* 0x1ea3f0 */
void __stdcall D3DDevice_SetRenderState_YuvEnable(uint32_t value)
{
  uint32_t *p;

  D3D_RENDER_STATE[D3DRS_YUVENABLE] = value;
  p = compat_d3d_push_begin(2);
  compat_d3d_push_end(push_control0(p));
}

/* 0x1ea420 */
void __stdcall D3DDevice_SetRenderState_OcclusionCullEnable(uint32_t value)
{
  uint32_t *p;

  D3D_RENDER_STATE[D3DRS_OCCLUSIONCULLENABLE] = value;
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_OCCLUSION_CULL_ENABLE, 1);
  p[1] = occlusion_cull_bits();
  compat_d3d_push_end(p + 2);
}

/* 0x1ea480 */
void __stdcall D3DDevice_SetRenderState_StencilCullEnable(uint32_t value)
{
  uint32_t *p;

  D3D_RENDER_STATE[D3DRS_STENCILCULLENABLE] = value;
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_OCCLUSION_CULL_ENABLE, 1);
  p[1] = occlusion_cull_bits();
  compat_d3d_push_end(p + 2);
}

/* 0x1ea1c0: rewrites NV_PGRAPH_DEBUG_5 and 0x400b80 from their shadows
 * through the register-write subprogram. */
void D3D_CommonSetDebugRegisters(void)
{
  unsigned long *d;
  uint32_t *p;

  d = D3D_DEVICE;
  d[DEVICE_DEBUG_5_SHADOW / 4] &= ~8ul;
  if (D3D_RENDER_STATE[D3DRS_DONOTCULLUNCOMPRESSED]) {
    d[DEVICE_DEBUG_5_SHADOW / 4] |= 8;
  }
  d[DEVICE_400B80_SHADOW / 4] &= 0xe7effffful;
  if (D3D_RENDER_STATE[D3DRS_ROPZCMPALWAYSREAD]) {
    d[DEVICE_400B80_SHADOW / 4] |= 0x100000;
  }
  if (D3D_RENDER_STATE[D3DRS_ROPZREAD]) {
    d[DEVICE_400B80_SHADOW / 4] |= 0x8000000;
  }
  p = compat_d3d_push_begin(12);
  p[0] = METHOD(NV097_WAIT_FOR_IDLE, 1);
  p[1] = 0;
  p[2] = METHOD(NV097_SET_ZSTENCIL_CLEAR_VALUE, 2);
  p[3] = NV_PGRAPH_DEBUG_5;
  p[4] = d[DEVICE_DEBUG_5_SHADOW / 4];
  p[5] = METHOD(NV097_NO_OPERATION, 1);
  p[6] = PBKIT_SUBPROGRAM_WRITE_REGISTER; /* XDK: XDK_SUBPROGRAM_WRITE_REGISTER */
  p[7] = METHOD(NV097_SET_ZSTENCIL_CLEAR_VALUE, 2);
  p[8] = NV_PGRAPH_400B80;
  p[9] = d[DEVICE_400B80_SHADOW / 4];
  p[10] = METHOD(NV097_NO_OPERATION, 1);
  p[11] = PBKIT_SUBPROGRAM_WRITE_REGISTER;
  compat_d3d_push_end(p + 12);
}

/* 0x1ea4e0 */
void __stdcall D3DDevice_SetRenderState_RopZCmpAlwaysRead(uint32_t value)
{
  D3D_RENDER_STATE[D3DRS_ROPZCMPALWAYSREAD] = value;
  D3D_CommonSetDebugRegisters();
}

/* 0x1ea500 */
void __stdcall D3DDevice_SetRenderState_RopZRead(uint32_t value)
{
  D3D_RENDER_STATE[D3DRS_ROPZREAD] = value;
  D3D_CommonSetDebugRegisters();
}

/* 0x1ea520 */
void __stdcall D3DDevice_SetRenderState_DoNotCullUncompressed(uint32_t value)
{
  D3D_RENDER_STATE[D3DRS_DONOTCULLUNCOMPRESSED] = value;
  D3D_CommonSetDebugRegisters();
}

/* 0x1ea540 / 0x1ea590 tail: FUN_001eb6f0 and the viewport push of
 * FUN_001e9db0 (both ecx = device; draw.c), then bits 0-3
 * anti-aliasing, 16-31 sample mask. */
static void push_anti_aliasing_control(void)
{
  uint32_t *p;

  compat_d3d_push_passthrough_program();
  compat_d3d_push_viewport();
  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_ANTI_ALIASING_CONTROL, 1);
  p[1] = (D3D_RENDER_STATE[D3DRS_MULTISAMPLEMASK] << 16) |
         (D3D_RENDER_STATE[D3DRS_MULTISAMPLEANTIALIAS] & 0xf);
  compat_d3d_push_end(p + 2);
}

/* 0x1ea540 (see push_anti_aliasing_control) */
void __stdcall D3DDevice_SetRenderState_MultiSampleAntiAlias(uint32_t value)
{
  D3D_RENDER_STATE[D3DRS_MULTISAMPLEANTIALIAS] = value;
  push_anti_aliasing_control();
}

/* 0x1ea590 (see push_anti_aliasing_control) */
void __stdcall D3DDevice_SetRenderState_MultiSampleMask(uint32_t value)
{
  D3D_RENDER_STATE[D3DRS_MULTISAMPLEMASK] = value;
  push_anti_aliasing_control();
}

/* 0x1e9ae0: D3DTSS_TCI_* to the texgen mode of S, T and R. */
void __stdcall D3DDevice_SetTextureState_TexCoordIndex(uint32_t stage, uint32_t index)
{
  unsigned long *d;
  unsigned long mode;
  unsigned long texgen;
  unsigned long uses_normals;
  unsigned long mask;
  uint32_t *p;

  D3D_TEXTURE_STATE[stage * TEXTURE_STATE_STRIDE + D3DTSS_TEXCOORDINDEX] = index;
  d = D3D_DEVICE;
  mode = index & 0xffff0000;
  uses_normals = 0;
  switch (mode) {
  case 0x00000: /* D3DTSS_TCI_PASSTHRU */
    d[DEVICE_DIRTY / 4] |= 0x180;
    texgen = NV097_SET_TEXGEN_S_DISABLE;
    break;
  case 0x10000: /* D3DTSS_TCI_CAMERASPACENORMAL */
    texgen = NV097_SET_TEXGEN_S_NORMAL_MAP;
    uses_normals = 1;
    break;
  case 0x20000: /* D3DTSS_TCI_CAMERASPACEPOSITION */
    texgen = NV097_SET_TEXGEN_S_EYE_LINEAR;
    break;
  case 0x30000: /* D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR */
    texgen = NV097_SET_TEXGEN_S_REFLECTION_MAP;
    uses_normals = 1;
    break;
  case 0x40000: /* D3DTSS_TCI_OBJECT */
    texgen = NV097_SET_TEXGEN_S_OBJECT_LINEAR;
    break;
  default: /* D3DTSS_TCI_SPHERE */
    texgen = NV097_SET_TEXGEN_S_SPHERE_MAP;
    uses_normals = 1;
    break;
  }
  mask = d[DEVICE_TEXGEN_MASK / 4];
  if (mask == 0 && uses_normals) {
    d[DEVICE_DIRTY / 4] |= 2;
  }
  d[DEVICE_TEXGEN_MASK / 4] = (mask & ~(1ul << stage)) | (uses_normals << stage);
  p = compat_d3d_push_begin(4);
  p[0] = METHOD(NV097_SET_TEXGEN_S + stage * TEXGEN_STAGE_STRIDE, 3);
  p[1] = texgen;
  p[2] = texgen;
  p[3] = texgen;
  compat_d3d_push_end(p + 4);
  d[DEVICE_DIRTY / 4] |= 0x194;
}

/* 0x1e9bc0: without a pixel shader stage s drives texture unit s + 1; only
 * units 1-3 have bump-env registers. */
void __stdcall D3DDevice_SetTextureState_BumpEnv(uint32_t stage, uint32_t state, uint32_t value)
{
  unsigned long unit;
  uint32_t *p;

  unit = stage;
  if (*device_field(DEVICE_PIXEL_SHADER) == 0) {
    unit = stage + 1;
  }
  if (unit & 3) {
    p = compat_d3d_push_begin(2);
    p[0] = METHOD(BUMP_ENV_METHOD_BASE + (unit * 16 + state) * 4, 1);
    p[1] = value;
    compat_d3d_push_end(p + 2);
  }
  D3D_TEXTURE_STATE[stage * TEXTURE_STATE_STRIDE + state] = value;
}

/* 0x1e9c20 */
void __stdcall D3DDevice_SetTextureState_BorderColor(uint32_t stage, uint32_t color)
{
  uint32_t *p;

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_TEXTURE_BORDER_COLOR + stage * BORDER_COLOR_STAGE_STRIDE, 1);
  p[1] = color;
  compat_d3d_push_end(p + 2);
  D3D_TEXTURE_STATE[stage * TEXTURE_STATE_STRIDE + D3DTSS_BORDERCOLOR] = color;
}

/* 0x1e9c60 */
void __stdcall D3DDevice_SetTextureState_ColorKeyColor(uint32_t stage, uint32_t value)
{
  uint32_t *p;

  p = compat_d3d_push_begin(2);
  p[0] = METHOD(NV097_SET_COLOR_KEY_COLOR + stage * COLOR_KEY_STAGE_STRIDE, 1);
  p[1] = value;
  compat_d3d_push_end(p + 2);
  D3D_TEXTURE_STATE[stage * TEXTURE_STATE_STRIDE + D3DTSS_COLORKEYCOLOR] = value;
}

/* D3DVIEWPORT8 */
typedef struct {
  unsigned long x;
  unsigned long y;
  unsigned long width;
  unsigned long height;
  unsigned long min_z;
  unsigned long max_z;
} d3d_viewport;

/* 0x1e82a0: clips the viewport to the render target (the back buffer is
 * measured through the front buffer header) and stores it at device+0xb00,
 * where GetViewport reads it. The original then pushes the transform:
 * D3D_UpdateProjectionViewportTransform (called), D3DDevice_SetScissors(0,
 * 0, 0) (not called: its kb decl is a void(void) placeholder over a 3-argument
 * __stdcall) and the viewport push of FUN_001e9db0 (draw.c). */
void __stdcall D3DDevice_SetViewport(void *viewport)
{
  const d3d_viewport *vp;
  unsigned long *d;
  const unsigned long *surface;
  unsigned long right;
  unsigned long bottom;
  unsigned long limit;

  vp = (const d3d_viewport *)viewport;
  d = D3D_DEVICE;
  surface = (const unsigned long *)d[DEVICE_RENDER_TARGET / 4];
  if (surface == device_field(DEVICE_BUFFER_SURFACES)) {
    surface = device_field(DEVICE_FRONT_BUFFER_SURFACE);
  }
  right = vp->x + vp->width;
  limit = compat_d3d_surface_width(surface);
  if (right >= limit) {
    right = limit;
  }
  bottom = vp->y + vp->height;
  limit = compat_d3d_surface_height(surface);
  if (bottom >= limit) {
    bottom = limit;
  }
  d[DEVICE_VIEWPORT / 4 + 0] = vp->x;
  d[DEVICE_VIEWPORT / 4 + 1] = vp->y;
  d[DEVICE_VIEWPORT / 4 + 2] = right - vp->x;
  d[DEVICE_VIEWPORT / 4 + 3] = bottom - vp->y;
  d[DEVICE_VIEWPORT / 4 + 4] = vp->min_z;
  d[DEVICE_VIEWPORT / 4 + 5] = vp->max_z;
  D3D_UpdateProjectionViewportTransform();
  compat_d3d_push_viewport();
  d[DEVICE_DIRTY / 4] |= 1;
}

/* 0x1eb590: bit 4 is a device flag; mode 0 reloads the fixed-function
 * constants, identity texgen planes for all four stages and the fog plane. */
void __stdcall D3DDevice_SetShaderConstantMode(uint32_t mode)
{
  unsigned long *d;
  uint32_t *p;
  int i;

  d = D3D_DEVICE;
  if (mode & 0x10) {
    d[DEVICE_STATE_FLAGS / 4] |= 0x200;
  } else {
    d[DEVICE_STATE_FLAGS / 4] &= ~0x200ul;
  }
  mode &= ~0x10ul;
  d[DEVICE_SHADER_CONSTANT_MODE / 4] = mode;
  if (mode != 0) {
    return;
  }
  d[DEVICE_DIRTY / 4] |= 0x26;
  p = compat_d3d_push_begin(88);
  p[0] = METHOD(NV097_SET_TRANSFORM_CONSTANT_LOAD, 1);
  p[1] = 0x3c;
  p[2] = METHOD(NV097_SET_TRANSFORM_CONSTANT, 12);
  for (i = 0; i < 12; i++) {
    p[3 + i] = d3d_shader_constant_defaults[i];
  }
  p += 15;
  for (i = 0; i < 4; i++) {
    p = compat_d3d_push_transposed_matrix(
      p, NV097_SET_TEXGEN_PLANE_S + i * TEXGEN_PLANE_STAGE_STRIDE,
      compat_d3d_identity_matrix);
  }
  p[0] = METHOD(NV097_SET_FOG_PLANE, 4);
  p[1] = 0;
  p[2] = 0;
  p[3] = 0x3f800000; /* 1.0f */
  p[4] = 0;
  compat_d3d_push_end(p + 5);
}

/* 0x1e72a0: through the kernel's AV encoder, as the XDK does. */
void __stdcall D3DDevice_SetFlickerFilter(uint32_t level)
{
  AvSendTVEncoderOption((void *)*device_field(DEVICE_REGISTER_BASE), VIDEO_ENC_FLICKERFILTER, level, NULL);
}

/* 0x1e72c0 */
void __stdcall D3DDevice_SetSoftDisplayFilter(uint32_t enable)
{
  AvSendTVEncoderOption((void *)*device_field(DEVICE_REGISTER_BASE), VIDEO_ENC_SOFTEN_FILTER, enable, NULL);
}
