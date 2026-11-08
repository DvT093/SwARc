// d3d2: Direct3D 2 (DirectX 5 Immediate Mode) on the same gfx device as
// Direct3D 7: IDirect3D2, IDirect3DDevice2, IDirect3DViewport2,
// IDirect3DMaterial2, IDirect3DLight and IDirect3DTexture2.
//
// What DirectX 5 does differently from Direct3D 7, and how it maps:
//   - Vertices come as D3DVERTEXTYPE: D3DVERTEX (lit by Direct3D),
//     D3DLVERTEX (lit by the game) and D3DTLVERTEX (transformed), which
//     are the FVFs XYZ|NORMAL|TEX1, XYZ|RESERVED1|DIFFUSE|SPECULAR|TEX1
//     and XYZRHW|DIFFUSE|SPECULAR|TEX1. Lighting is on for D3DVERTEX only
//     (unless D3DDP_DONOTLIGHT).
//   - Textures are bound by handle (D3DRENDERSTATE_TEXTUREHANDLE, from
//     IDirect3DTexture2::GetHandle) and combined with
//     D3DRENDERSTATE_TEXTUREMAPBLEND; filtering and addressing are render
//     states too. They become stage 0's texture and texture stage states.
//   - Viewports, materials and lights are objects with handles; the
//     material in use is a light state, the clear colour the viewport's
//     background material. A viewport's scale (D3DVIEWPORT) or clip
//     volume (D3DVIEWPORT2) becomes a matrix applied after the projection.
// Texture handles are the texture surface's interface pointer, which
// IDirect3DTexture2 maps back to.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

#include "runtime/log.h"
#include "win32/ddraw_impl.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::ddraw {

namespace {

using guest::DWORD;
using guest::D3DMATRIX;

constexpr guest::GUID kIID_IDirect3D2 = {0x6AAE1EC1, 0x662A, 0x11D0, {0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A}};
constexpr guest::GUID kIID_IDirect3DDevice2 = {0x93281501, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};
constexpr guest::GUID kIID_IDirect3DTexture2 = {0x93281502, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};
constexpr guest::GUID kIID_IDirect3DViewport2 = {0x93281500, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};
constexpr guest::GUID kIID_IDirect3DMaterial2 = {0x93281503, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};
constexpr guest::GUID kIID_IDirect3DHALDevice = {0x84E63DE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};
constexpr guest::GUID kIID_IDirect3DRGBDevice = {0xA4665C60, 0x2673, 0x11CF, {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};

constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3DERR_VIEWPORTHASNODEVICE = 0x88760883;  // not attached
constexpr uint32_t D3DERR_MATERIAL_SETDATA_FAILED = 0x887602EE;

// ---- DirectX 5 structures (32-bit guest layout) --------------------------

struct D3DTRANSFORMCAPS {
  DWORD dwSize, dwCaps;
};
struct D3DLIGHTINGCAPS {
  DWORD dwSize, dwCaps, dwLightingModel, dwNumLights;
};
// D3DDEVICEDESC: DirectX 3 callers know the first 172 bytes, DirectX 5
// ones 204, DirectX 6 ones all 252.
struct D3DDEVICEDESC {
  DWORD dwSize, dwFlags, dcmColorModel, dwDevCaps;
  D3DTRANSFORMCAPS dtcTransformCaps;
  DWORD bClipping;
  D3DLIGHTINGCAPS dlcLightingCaps;
  guest::D3DPRIMCAPS dpcLineCaps, dpcTriCaps;
  DWORD dwDeviceRenderBitDepth, dwDeviceZBufferBitDepth, dwMaxBufferSize,
      dwMaxVertexCount;
  DWORD dwMinTextureWidth, dwMinTextureHeight, dwMaxTextureWidth,
      dwMaxTextureHeight, dwMinStippleWidth, dwMaxStippleWidth,
      dwMinStippleHeight, dwMaxStippleHeight;
  DWORD dwMaxTextureRepeat, dwMaxTextureAspectRatio, dwMaxAnisotropy;
  float dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom,
      dvExtentsAdjust;
  DWORD dwStencilCaps, dwFVFCaps, dwTextureOpCaps;
  uint16_t wMaxTextureBlendStages, wMaxSimultaneousTextures;
};
static_assert(offsetof(D3DDEVICEDESC, dwMinTextureWidth) == 172);
static_assert(offsetof(D3DDEVICEDESC, dwMaxTextureRepeat) == 204);
static_assert(sizeof(D3DDEVICEDESC) == 252);
constexpr uint32_t kDeviceDescDX5 = 204;

struct D3DCOLORVALUE {
  float r, g, b, a;
};

struct D3DMATERIAL {
  DWORD dwSize;
  D3DCOLORVALUE diffuse, ambient, specular, emissive;
  float power;
  DWORD hTexture, dwRampSize;
};
static_assert(sizeof(D3DMATERIAL) == 80);

// D3DVIEWPORT (scale and max x/y) and D3DVIEWPORT2 (clip volume): the same
// size, the four floats after the rectangle mean different things.
struct D3DVIEWPORTX {
  DWORD dwSize, dwX, dwY, dwWidth, dwHeight;
  float f[4];  // dvScaleX, dvScaleY, dvMaxX, dvMaxY / dvClipX, dvClipY, dvClipWidth, dvClipHeight
  float dvMinZ, dvMaxZ;
};
static_assert(sizeof(D3DVIEWPORTX) == 44);

// D3DLIGHT (76 bytes) and D3DLIGHT2 (80, with dwFlags).
struct D3DLIGHT2 {
  DWORD dwSize, dltType;
  D3DCOLORVALUE dcvColor;
  float dvPosition[3], dvDirection[3];
  float dvRange, dvFalloff, dvAttenuation0, dvAttenuation1, dvAttenuation2,
      dvTheta, dvPhi;
  DWORD dwFlags;
};
static_assert(sizeof(D3DLIGHT2) == 80);
constexpr DWORD D3DLIGHT_ACTIVE = 1, D3DLIGHT_NO_SPECULAR = 2;

// ---- host objects --------------------------------------------------------

struct Direct3D2 : com::Object {
  uint32_t ddraw = 0;  // IDirectDraw2 guest pointer (referenced)
};

struct Material2 : com::Object {
  D3DMATERIAL m{};
};

struct Light2 : com::Object {
  D3DLIGHT2 l{};
  bool set = false;
};

struct Device2;

struct Viewport2 : com::Object {
  Device2* device = nullptr;  // the device it was added to
  D3DVIEWPORTX vp{};
  int version = 0;            // 1 (SetViewport), 2 (SetViewport2); 0 unset
  uint32_t background = 0;    // material handle
  std::vector<uint32_t> lights;  // IDirect3DLight guest pointers (referenced)
  ~Viewport2() override;
};

struct Texture2 : com::Object {
  uint32_t surface = 0;  // the texture surface (referenced)
  ~Texture2() override {
    if (surface && com::Lookup(surface)) com::Release(surface);
  }
};

struct Device2 : com::Object {
  uint32_t d3d = 0;  // IDirect3D2 guest pointer (referenced)
  Surface* target = nullptr;
  uint32_t target_guest = 0;
  uint32_t rs[256] = {};
  uint32_t tss[8][32] = {};
  uint32_t light_state[8] = {};
  std::map<uint32_t, D3DMATRIX> transforms;
  std::vector<uint32_t> viewports;  // added viewports (referenced)
  uint32_t current = 0;             // current viewport
  // Begin/Vertex/End
  uint32_t begin_prim = 0, begin_vt = 0;
  std::vector<uint8_t> begin_verts;
  ~Device2() override;
};

Viewport2::~Viewport2() {
  for (uint32_t l : lights)
    if (com::Lookup(l)) com::Release(l);
  if (device && device->current == guest) device->current = 0;
}

Device2::~Device2() {
  for (uint32_t v : viewports)
    if (auto* vp = com::As<Viewport2>(v)) {
      vp->device = nullptr;
      com::Release(v);
    }
}

uint32_t Direct3D2Vtable() {
  return com::Vtable("ddraw.dll", "IDirect3D2",
                     {"QueryInterface", "AddRef", "Release", "EnumDevices",
                      "CreateLight", "CreateMaterial", "CreateViewport",
                      "FindDevice", "CreateDevice"});
}

uint32_t Device2Vtable() {
  return com::Vtable(
      "ddraw.dll", "IDirect3DDevice2",
      {"QueryInterface", "AddRef", "Release", "GetCaps", "SwapTextureHandles",
       "GetStats", "AddViewport", "DeleteViewport", "NextViewport",
       "EnumTextureFormats", "BeginScene", "EndScene", "GetDirect3D",
       "SetCurrentViewport", "GetCurrentViewport", "SetRenderTarget",
       "GetRenderTarget", "Begin", "BeginIndexed", "Vertex", "Index", "End",
       "GetRenderState", "SetRenderState", "GetLightState", "SetLightState",
       "SetTransform", "GetTransform", "MultiplyTransform", "DrawPrimitive",
       "DrawIndexedPrimitive", "SetClipStatus", "GetClipStatus"});
}

uint32_t Viewport2Vtable() {
  return com::Vtable(
      "ddraw.dll", "IDirect3DViewport2",
      {"QueryInterface", "AddRef", "Release", "Initialize", "GetViewport",
       "SetViewport", "TransformVertices", "LightElements", "SetBackground",
       "GetBackground", "SetBackgroundDepth", "GetBackgroundDepth", "Clear",
       "AddLight", "DeleteLight", "NextLight", "GetViewport2", "SetViewport2"});
}

uint32_t Material2Vtable() {
  return com::Vtable("ddraw.dll", "IDirect3DMaterial2",
                     {"QueryInterface", "AddRef", "Release", "SetMaterial",
                      "GetMaterial", "GetHandle"});
}

uint32_t LightVtable() {
  return com::Vtable("ddraw.dll", "IDirect3DLight",
                     {"QueryInterface", "AddRef", "Release", "Initialize",
                      "SetLight", "GetLight"});
}

uint32_t Texture2Vtable() {
  return com::Vtable("ddraw.dll", "IDirect3DTexture2",
                     {"QueryInterface", "AddRef", "Release", "GetHandle",
                      "PaletteChanged", "Load"});
}

template <typename T>
T* Obj(Ctx& c, const char* iface) {
  T* o = com::As<T>(hle::Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(hle::Arg(c, 0)));
  return o;
}

Device2* Dev(Ctx& c) { return Obj<Device2>(c, "IDirect3DDevice2"); }

// The HAL device's caps in DirectX 5 terms. `hal`: the hardware
// description; false gives the (empty) software emulation one.
D3DDEVICEDESC DeviceDesc(bool hal) {
  D3DDEVICEDESC d{};
  d.dwSize = sizeof d;
  if (!hal) return d;
  guest::D3DDEVICEDESC7 d7 = DeviceDesc7(false);
  d.dwFlags = 0x7FF;     // D3DDD_COLORMODEL ... D3DDD_MAXVERTEXCOUNT
  d.dcmColorModel = 2;   // D3DCOLOR_RGB
  d.dwDevCaps = d7.dwDevCaps;
  d.dtcTransformCaps = {uint32_t(sizeof(D3DTRANSFORMCAPS)), 1};  // D3DTRANSFORMCAPS_CLIP
  d.bClipping = 1;
  // Point, spot, directional and parallel point lights; the RGB model.
  d.dlcLightingCaps = {uint32_t(sizeof(D3DLIGHTINGCAPS)), 0xF, 1, 8};
  d.dpcLineCaps = d7.dpcLineCaps;
  d.dpcTriCaps = d7.dpcTriCaps;
  d.dwDeviceRenderBitDepth = d7.dwDeviceRenderBitDepth;
  d.dwDeviceZBufferBitDepth = d7.dwDeviceZBufferBitDepth;
  d.dwMaxBufferSize = 0;
  d.dwMaxVertexCount = 65535;
  d.dwMinTextureWidth = d7.dwMinTextureWidth;
  d.dwMinTextureHeight = d7.dwMinTextureHeight;
  d.dwMaxTextureWidth = d7.dwMaxTextureWidth;
  d.dwMaxTextureHeight = d7.dwMaxTextureHeight;
  d.dwMaxTextureRepeat = d7.dwMaxTextureRepeat;
  d.dwMaxTextureAspectRatio = d7.dwMaxTextureAspectRatio;
  d.dwMaxAnisotropy = d7.dwMaxAnisotropy;
  d.dvGuardBandLeft = d7.dvGuardBandLeft;
  d.dvGuardBandTop = d7.dvGuardBandTop;
  d.dvGuardBandRight = d7.dvGuardBandRight;
  d.dvGuardBandBottom = d7.dvGuardBandBottom;
  d.dwStencilCaps = d7.dwStencilCaps;
  d.dwFVFCaps = d7.dwFVFCaps;
  d.dwTextureOpCaps = d7.dwTextureOpCaps;
  d.wMaxTextureBlendStages = d7.wMaxTextureBlendStages;
  d.wMaxSimultaneousTextures = d7.wMaxSimultaneousTextures;
  return d;
}

// Writes `d` into the caller's D3DDEVICEDESC (its dwSize says how much of
// it there is).
void PutDeviceDesc(uint32_t va, D3DDEVICEDESC d) {
  if (!va) return;
  uint32_t size = std::min<uint32_t>(Rd32(va), sizeof d);
  if (size < 4) size = kDeviceDescDX5;
  d.dwSize = size;
  std::memcpy(GPtr(va), &d, size);
}

// ---- state translation ---------------------------------------------------

// D3DVERTEXTYPE -> FVF.
uint32_t FvfFor(uint32_t vt) {
  switch (vt) {
    case 1: return 0x112;  // D3DVT_VERTEX: XYZ | NORMAL | TEX1
    case 2: return 0x1E2;  // D3DVT_LVERTEX: XYZ | RESERVED1 | DIFFUSE | SPECULAR | TEX1
    case 3: return 0x1C4;  // D3DVT_TLVERTEX: XYZRHW | DIFFUSE | SPECULAR | TEX1
  }
  FatalF("Direct3D 2: vertex type %u", vt);
}

constexpr uint32_t RS_TEXTUREHANDLE = 1, RS_TEXTUREADDRESS = 3, RS_WRAPU = 5,
                   RS_WRAPV = 6, RS_TEXTUREMAG = 17, RS_TEXTUREMIN = 18,
                   RS_TEXTUREMAPBLEND = 21, RS_BORDERCOLOR = 42,
                   RS_TEXTUREADDRESSU = 43, RS_TEXTUREADDRESSV = 44,
                   RS_MIPMAPLODBIAS = 45, RS_ANISOTROPY = 48, RS_LIGHTING = 137,
                   RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140, RS_WRAP0 = 128;
// Texture stage states.
constexpr uint32_t TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3,
                   TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
                   TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_BORDERCOLOR = 15,
                   TSS_MAGFILTER = 16, TSS_MINFILTER = 17, TSS_MIPFILTER = 18,
                   TSS_MIPMAPLODBIAS = 19, TSS_MAXANISOTROPY = 21;
constexpr uint32_t TOP_SELECTARG1 = 2, TOP_SELECTARG2 = 3, TOP_MODULATE = 4,
                   TOP_ADD = 7, TOP_BLENDTEXTUREALPHA = 13;
constexpr uint32_t TA_DIFFUSE = 0, TA_TEXTURE = 2;

void SetTss(Device2& d, uint32_t type, uint32_t v) {
  d.tss[0][type] = v;
  Gfx()->SetTextureStageState(0, type, v);
}

// The texture surface of a handle (null if none or released).
Surface* TextureFor(uint32_t handle) {
  if (!handle) return nullptr;
  Surface* s = com::As<Surface>(handle);
  return s && s->tex ? s : nullptr;
}

bool HasAlpha(const Surface& s) {
  return (s.desc.ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS) != 0;
}

constexpr uint32_t RS_ALPHATESTENABLE = 15, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25,
                   RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_ALPHABLENDENABLE = 27,
                   RS_COLORKEYENABLE = 41;

// Whether alpha blending is on with a factor that uses the source alpha
// (SRCALPHA, INVSRCALPHA, SRCALPHASAT, BOTHSRCALPHA, BOTHINVSRCALPHA).
bool BlendReadsSourceAlpha(const Device2& d) {
  if (!d.rs[RS_ALPHABLENDENABLE]) return false;
  for (uint32_t f : {d.rs[RS_SRCBLEND], d.rs[RS_DESTBLEND]})
    if (f == 5 || f == 6 || f == 11 || f == 12 || f == 13) return true;
  return false;
}

// Stage 0 from the texture handle and D3DRENDERSTATE_TEXTUREMAPBLEND.
// Colour keying (COLORKEYENABLE with a keyed texture, see Surface::keyed):
// the texture's alpha, 0 at key texels, goes into the fragment's alpha and
// an alpha test drops those; otherwise the game's alpha test applies.
void UpdateStage0(Device2& d) {
  Surface* t = TextureFor(d.rs[RS_TEXTUREHANDLE]);
  Gfx()->SetTexture(0, t ? t->tex : 0);
  bool ck = t && t->keyed && t->has_ck_src && d.rs[RS_COLORKEYENABLE];
  Gfx()->SetRenderState(RS_ALPHATESTENABLE, ck ? 1 : d.rs[RS_ALPHATESTENABLE]);
  if (ck && !d.rs[RS_ALPHATESTENABLE]) {
    Gfx()->SetRenderState(RS_ALPHAFUNC, 5);  // GREATER
    Gfx()->SetRenderState(RS_ALPHAREF, 0);
  } else {
    Gfx()->SetRenderState(RS_ALPHAFUNC, d.rs[RS_ALPHAFUNC]);
    Gfx()->SetRenderState(RS_ALPHAREF, d.rs[RS_ALPHAREF]);
  }
  uint32_t cop = TOP_SELECTARG2, aop = TOP_SELECTARG2;  // no texture: the vertex colour
  if (t) {
    switch (d.rs[RS_TEXTUREMAPBLEND]) {
      case 1:  // DECAL
      case 5:  // DECALMASK
      case 7:  // COPY
        cop = aop = TOP_SELECTARG1;
        break;
      case 3:  // DECALALPHA: texture over the vertex colour by texture alpha
        cop = TOP_BLENDTEXTUREALPHA;
        aop = TOP_SELECTARG2;
        break;
      case 4:  // MODULATEALPHA
        cop = aop = TOP_MODULATE;
        break;
      case 8:  // ADD
        cop = TOP_ADD;
        aop = TOP_SELECTARG2;
        break;
      default:  // MODULATE, MODULATEMASK: alpha from the texture if it has it
        cop = TOP_MODULATE;
        aop = HasAlpha(*t) ? TOP_SELECTARG1 : TOP_SELECTARG2;
        break;
    }
    // Keyed: the key's alpha 0 has to reach the alpha test, whatever the
    // vertex alpha (pre-lit vertices often carry 0 there). Unless blending
    // reads the source alpha, the fragment's alpha is otherwise unused: the
    // texture's alone; if it does, the vertex alpha still scales it.
    if (ck && aop == TOP_SELECTARG2)
      aop = BlendReadsSourceAlpha(d) ? TOP_MODULATE : TOP_SELECTARG1;
  }
  SetTss(d, TSS_COLOROP, cop);
  SetTss(d, TSS_COLORARG1, TA_TEXTURE);
  SetTss(d, TSS_COLORARG2, TA_DIFFUSE);
  SetTss(d, TSS_ALPHAOP, aop);
  SetTss(d, TSS_ALPHAARG1, TA_TEXTURE);
  SetTss(d, TSS_ALPHAARG2, TA_DIFFUSE);
}

// D3DRENDERSTATE_TEXTUREMIN (D3DFILTER_*) -> MINFILTER, MIPFILTER.
void SetMinFilter(Device2& d, uint32_t f) {
  uint32_t min = 1, mip = 1;  // point, no mips
  switch (f) {
    case 2: min = 2; break;              // LINEAR
    case 3: mip = 2; break;              // MIPNEAREST
    case 4: min = 2; mip = 2; break;     // MIPLINEAR
    case 5: mip = 3; break;              // LINEARMIPNEAREST
    case 6: min = 2; mip = 3; break;     // LINEARMIPLINEAR
  }
  SetTss(d, TSS_MINFILTER, min);
  SetTss(d, TSS_MIPFILTER, mip);
}

void SetRenderState2(Device2& d, uint32_t s, uint32_t v) {
  if (s < 256) d.rs[s] = v;
  switch (s) {
    case RS_TEXTUREHANDLE:
    case RS_TEXTUREMAPBLEND:
    case RS_COLORKEYENABLE:
    case RS_ALPHATESTENABLE:
    case RS_ALPHAREF:
    case RS_ALPHAFUNC:
      UpdateStage0(d);
      return;
    case RS_ALPHABLENDENABLE:
    case RS_SRCBLEND:
    case RS_DESTBLEND:
      Gfx()->SetRenderState(s, v);
      UpdateStage0(d);
      return;
    case RS_TEXTUREMAG: SetTss(d, TSS_MAGFILTER, v == 2 ? 2 : 1); return;
    case RS_TEXTUREMIN: SetMinFilter(d, v); return;
    case RS_TEXTUREADDRESS:
      SetTss(d, TSS_ADDRESSU, v);
      SetTss(d, TSS_ADDRESSV, v);
      return;
    case RS_TEXTUREADDRESSU: SetTss(d, TSS_ADDRESSU, v); return;
    case RS_TEXTUREADDRESSV: SetTss(d, TSS_ADDRESSV, v); return;
    case RS_BORDERCOLOR: SetTss(d, TSS_BORDERCOLOR, v); return;
    case RS_MIPMAPLODBIAS: SetTss(d, TSS_MIPMAPLODBIAS, v); return;
    case RS_ANISOTROPY: SetTss(d, TSS_MAXANISOTROPY, v); return;
    case RS_WRAPU:
    case RS_WRAPV: {
      uint32_t w = (d.rs[RS_WRAPU] ? 1 : 0) | (d.rs[RS_WRAPV] ? 2 : 0);
      Gfx()->SetRenderState(RS_WRAP0, w);
      return;
    }
    // States without an effect here: line pattern, mono, ROP2, plane mask,
    // subpixel, stippling, batch flushing, sort independence.
    case 10: case 11: case 12: case 13: case 31: case 32: case 33: case 39:
    case 49: case 50:
      return;
    default:
      if (s >= 64 && s < 96) return;  // STIPPLEPATTERN00-31
      Gfx()->SetRenderState(s, v);
      return;
  }
}

// ---- viewports -----------------------------------------------------------

// The projection the gfx device gets: the game's, then the mapping from
// the viewport's scale or clip volume to Direct3D 7's (x and y in -1..1
// over the viewport rectangle).
void ApplyProjection(Device2& d) {
  D3DMATRIX p{};
  auto it = d.transforms.find(3);
  if (it != d.transforms.end()) {
    p = it->second;
  } else {
    for (int i = 0; i < 4; i++) p.m[i][i] = 1.0f;
  }
  auto* v = d.current ? com::As<Viewport2>(d.current) : nullptr;
  float ax = 1, bx = 0, ay = 1, by = 0;
  if (v && v->version == 1 && v->vp.dwWidth && v->vp.dwHeight) {
    // Screen x = X + W/2 + x/w * dvScaleX (y downwards with dvScaleY).
    ax = 2.0f * v->vp.f[0] / float(v->vp.dwWidth);
    ay = 2.0f * v->vp.f[1] / float(v->vp.dwHeight);
  } else if (v && v->version == 2 && v->vp.f[2] != 0 && v->vp.f[3] != 0) {
    // The clip volume [ClipX, ClipX + ClipWidth] x [ClipY - ClipHeight,
    // ClipY] fills the viewport.
    ax = 2.0f / v->vp.f[2];
    bx = -2.0f * v->vp.f[0] / v->vp.f[2] - 1.0f;
    ay = 2.0f / v->vp.f[3];
    by = 1.0f - 2.0f * v->vp.f[1] / v->vp.f[3];
  }
  // p * A with A: x' = ax x + bx w, y' = ay y + by w (row vectors).
  for (int i = 0; i < 4; i++) {
    float x = p.m[i][0], y = p.m[i][1], w = p.m[i][3];
    p.m[i][0] = ax * x + bx * w;
    p.m[i][1] = ay * y + by * w;
  }
  Gfx()->SetTransform(3, &p.m[0][0]);
}

gfx::Light ToGfxLight(const D3DLIGHT2& l) {
  gfx::Light g{};
  g.type = l.dltType;  // point 1, spot 2, directional 3 (parallel point 4 below)
  if (g.type == 4) g.type = 1;
  g.diffuse = {l.dcvColor.r, l.dcvColor.g, l.dcvColor.b, l.dcvColor.a};
  if (!(l.dwFlags & D3DLIGHT_NO_SPECULAR)) g.specular = g.diffuse;
  std::memcpy(g.position, l.dvPosition, sizeof g.position);
  std::memcpy(g.direction, l.dvDirection, sizeof g.direction);
  g.range = l.dvRange;
  g.falloff = l.dvFalloff;
  g.attenuation0 = l.dvAttenuation0;
  g.attenuation1 = l.dvAttenuation1;
  g.attenuation2 = l.dvAttenuation2;
  g.theta = l.dvTheta;
  g.phi = l.dvPhi;
  return g;
}

int g_lights_on = 0;
// The material handle of D3DLIGHTSTATE_MATERIAL: materials are used by
// reference, so changing it (SetMaterial) changes what is drawn.
uint32_t g_material = 0;  // gfx lights enabled by the last ApplyLights

// The lights of the current viewport become gfx lights 0..n-1.
void ApplyLights(Device2& d) {
  auto* v = d.current ? com::As<Viewport2>(d.current) : nullptr;
  int n = 0;
  if (v)
    for (uint32_t lp : v->lights) {
      auto* l = com::As<Light2>(lp);
      if (!l || !l->set || (l->l.dwSize >= sizeof(D3DLIGHT2) && !(l->l.dwFlags & D3DLIGHT_ACTIVE)))
        continue;
      Gfx()->SetLight(uint32_t(n), ToGfxLight(l->l));
      Gfx()->LightEnable(uint32_t(n), true);
      n++;
    }
  for (int i = n; i < g_lights_on; i++) Gfx()->LightEnable(uint32_t(i), false);
  g_lights_on = n;
}

void ApplyViewport(Device2& d) {
  auto* v = d.current ? com::As<Viewport2>(d.current) : nullptr;
  if (!v || !v->version) return;
  Gfx()->SetViewport({v->vp.dwX, v->vp.dwY, v->vp.dwWidth, v->vp.dwHeight,
                      v->vp.dvMinZ, v->vp.dvMaxZ});
  ApplyProjection(d);
  ApplyLights(d);
}

void ApplyMaterial(uint32_t handle) {
  auto* m = handle ? com::As<Material2>(handle) : nullptr;
  if (!m) return;
  gfx::Material g{};
  auto col = [](const D3DCOLORVALUE& c) { return gfx::Color{c.r, c.g, c.b, c.a}; };
  g.diffuse = col(m->m.diffuse);
  g.ambient = col(m->m.ambient);
  g.specular = col(m->m.specular);
  g.emissive = col(m->m.emissive);
  g.power = m->m.power;
  Gfx()->SetMaterial(g);
}

uint8_t ToByte(float f) { return uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); }

}  // namespace

uint32_t D3D2For(uint32_t dd) {
  auto d3d = std::make_shared<Direct3D2>();
  d3d->ddraw = dd;
  com::AddRef(dd);
  return com::Publish(d3d, Direct3D2Vtable());
}

uint32_t Texture2For(uint32_t surface) {
  auto t = std::make_shared<Texture2>();
  t->surface = surface;
  com::AddRef(surface);
  return com::Publish(t, Texture2Vtable());
}

}  // namespace recomp::ddraw

namespace recomp::hle {
namespace {

using namespace ddraw;

uint32_t QueryInterfaceSelf(Ctx& c, const guest::GUID& iid) {
  if (com::GuidEq(Arg(c, 1), iid)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return D3D_OK;
  }
  guest::GUID g = com::ReadGuid(Arg(c, 1));
  WarnF("d3d2: QueryInterface({%08X-...}) on %08X: no interface\n", g.Data1,
        uint32_t(Arg(c, 0)));
  Wr32(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}

// ---- IDirect3D2 ------------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirect3D2::QueryInterface", D3D2_QueryInterface, 3) {
  return QueryInterfaceSelf(c, kIID_IDirect3D2);
}
HLE_NAMED("ddraw.dll", "IDirect3D2::AddRef", D3D2_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirect3D2::Release", D3D2_Release, 1) {
  uint32_t self = Arg(c, 0), dd = 0;
  if (auto* d = com::As<Direct3D2>(self)) dd = d->ddraw;
  uint32_t left = com::Release(self);
  if (!left && dd) com::Release(dd);
  return left;
}

// Callback: HRESULT(GUID*, LPSTR desc, LPSTR name, D3DDEVICEDESC* hw,
// D3DDEVICEDESC* hel, LPVOID ctx): the HAL device, then the RGB
// (software) device, which is the same device here.
HLE_NAMED("ddraw.dll", "IDirect3D2::EnumDevices", D3D2_EnumDevices, 3) {
  uint32_t cb = Arg(c, 1), ctx = Arg(c, 2);
  uint32_t buf = HeapAllocProcess(256 + 16 + 2 * sizeof(D3DDEVICEDESC), true);
  uint32_t guid = buf + 256, hw = guid + 16, hel = hw + sizeof(D3DDEVICEDESC);
  struct {
    const guest::GUID* guid;
    const char* desc;
    const char* name;
    bool hal;
  } devs[] = {{&kIID_IDirect3DHALDevice, "Microsoft Direct3D Hardware acceleration through Direct3D HAL",
               "Direct3D HAL", true},
              {&kIID_IDirect3DRGBDevice, "Microsoft Direct3D RGB Software Emulation",
               "RGB Emulation", false}};
  for (const auto& d : devs) {
    PutStr(buf, d.desc);
    PutStr(buf + 128, d.name);
    std::memcpy(GPtr(guid), d.guid, 16);
    D3DDEVICEDESC h = DeviceDesc(d.hal), e = DeviceDesc(!d.hal);
    h.dwSize = e.dwSize = kDeviceDescDX5;
    std::memcpy(GPtr(hw), &h, sizeof h);
    std::memcpy(GPtr(hel), &e, sizeof e);
    if (!CallGuest(c, cb, {guid, buf, buf + 128, hw, hel, ctx})) break;
  }
  HeapFreeProcess(buf);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3D2::CreateMaterial", D3D2_CreateMaterial, 3) {
  Wr32(Arg(c, 1), com::Publish(std::make_shared<Material2>(), Material2Vtable()));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3D2::CreateViewport", D3D2_CreateViewport, 3) {
  Wr32(Arg(c, 1), com::Publish(std::make_shared<Viewport2>(), Viewport2Vtable()));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3D2::CreateLight", D3D2_CreateLight, 3) {
  Wr32(Arg(c, 1), com::Publish(std::make_shared<Light2>(), LightVtable()));
  return D3D_OK;
}

// (clsid, render target surface, out)
HLE_NAMED("ddraw.dll", "IDirect3D2::CreateDevice", D3D2_CreateDevice, 4) {
  uint32_t iid = Arg(c, 1), surf = Arg(c, 2), out = Arg(c, 3);
  Surface* t = com::As<Surface>(surf);
  if (!t) return DDERR_INVALIDPARAMS;
  if (!com::GuidEq(iid, kIID_IDirect3DHALDevice) && !com::GuidEq(iid, kIID_IDirect3DRGBDevice)) {
    guest::GUID g = com::ReadGuid(iid);
    WarnF("d3d2: CreateDevice of device {%08X-...}: using the HAL device\n", g.Data1);
  }
  auto d = std::make_shared<Device2>();
  d->d3d = Arg(c, 0);
  d->target = t;
  d->target_guest = surf;
  com::AddRef(surf);
  com::AddRef(d->d3d);
  InitDeviceStates(d->rs, d->tss);
  // DirectX 5's defaults where they differ from Direct3D 7's.
  d->rs[7] = t->zbuffer ? 1 : 0;  // ZENABLE
  d->rs[29] = 1;                  // SPECULARENABLE
  d->rs[RS_TEXTUREMAPBLEND] = 2;  // MODULATE
  d->rs[RS_TEXTUREMAG] = d->rs[RS_TEXTUREMIN] = 1;  // NEAREST
  d->rs[RS_TEXTUREADDRESS] = d->rs[RS_TEXTUREADDRESSU] = d->rs[RS_TEXTUREADDRESSV] = 1;
  gfx::DepthId z = t->zbuffer ? t->zbuffer->depth : 0;
  Gfx()->SetRenderTarget(t->kind == Surface::Kind::Texture ? t->tex : 0, z);
  Gfx()->SetViewport({0, 0, uint32_t(t->width), uint32_t(t->height), 0.0f, 1.0f});
  // What differs from the gfx device's Direct3D 7 defaults.
  for (uint32_t s : {29u, RS_TEXTUREMAG, RS_TEXTUREMIN, RS_TEXTUREADDRESS})
    SetRenderState2(*d, s, d->rs[s]);
  UpdateStage0(*d);
  Wr32(out, com::Publish(d, Device2Vtable()));
  DeviceFpuSetup(c, "d3d2");
  std::fprintf(stderr, "d3d2: CreateDevice on %dx%d target\n", t->width, t->height);
  return D3D_OK;
}

// ---- IDirect3DDevice2 -------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::QueryInterface", D3DD2_QueryInterface, 3) {
  return QueryInterfaceSelf(c, kIID_IDirect3DDevice2);
}
HLE_NAMED("ddraw.dll", "IDirect3DDevice2::AddRef", D3DD2_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirect3DDevice2::Release", D3DD2_Release, 1) {
  uint32_t self = Arg(c, 0), surf = 0, d3d = 0;
  if (auto* d = com::As<Device2>(self)) {
    surf = d->target_guest;
    d3d = d->d3d;
  }
  uint32_t left = com::Release(self);
  if (!left) {
    if (surf) com::Release(surf);
    if (d3d) com::Release(d3d);
  }
  return left;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::GetCaps", D3DD2_GetCaps, 3) {
  Dev(c);
  PutDeviceDesc(Arg(c, 1), DeviceDesc(true));
  PutDeviceDesc(Arg(c, 2), DeviceDesc(false));
  return D3D_OK;
}

// Callback: HRESULT(DDSURFACEDESC*, LPVOID ctx). DirectX 5 had no
// compressed formats.
HLE_NAMED("ddraw.dll", "IDirect3DDevice2::EnumTextureFormats", D3DD2_EnumTextureFormats, 3) {
  Dev(c);
  uint32_t cb = Arg(c, 1), ctx = Arg(c, 2);
  uint32_t va = HeapAllocProcess(sizeof(guest::DDSURFACEDESC2), true);
  using F = gfx::Format;
  for (F f : {F::R5G6B5, F::X1R5G5B5, F::A1R5G5B5, F::A4R4G4B4, F::X8R8G8B8, F::A8R8G8B8}) {
    guest::DDSURFACEDESC2 d{};
    d.dwSize = kSurfaceDescSize;
    d.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    PixelFormatFromFormat(f, &d.ddpfPixelFormat);
    std::memcpy(GPtr(va), &d, kSurfaceDescSize);
    if (!CallGuest(c, cb, {va, ctx})) break;
  }
  HeapFreeProcess(va);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::BeginScene", D3DD2_BeginScene, 1) {
  Dev(c);
  Gfx()->BeginScene();
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::EndScene", D3DD2_EndScene, 1) {
  Dev(c);
  Gfx()->EndScene();
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::GetDirect3D", D3DD2_GetDirect3D, 2) {
  Device2* d = Dev(c);
  com::AddRef(d->d3d);
  Wr32(Arg(c, 1), d->d3d);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::GetRenderTarget", D3DD2_GetRenderTarget, 2) {
  Device2* d = Dev(c);
  com::AddRef(d->target_guest);
  Wr32(Arg(c, 1), d->target_guest);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::AddViewport", D3DD2_AddViewport, 2) {
  Device2* d = Dev(c);
  auto* v = com::As<Viewport2>(Arg(c, 1));
  if (!v) return DDERR_INVALIDPARAMS;
  com::AddRef(Arg(c, 1));
  v->device = d;
  d->viewports.push_back(Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::DeleteViewport", D3DD2_DeleteViewport, 2) {
  Device2* d = Dev(c);
  auto it = std::find(d->viewports.begin(), d->viewports.end(), uint32_t(Arg(c, 1)));
  if (it == d->viewports.end()) return DDERR_INVALIDPARAMS;
  d->viewports.erase(it);
  if (d->current == Arg(c, 1)) d->current = 0;
  if (auto* v = com::As<Viewport2>(Arg(c, 1))) v->device = nullptr;
  com::Release(Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::SetCurrentViewport", D3DD2_SetCurrentViewport, 2) {
  Device2* d = Dev(c);
  if (!com::As<Viewport2>(Arg(c, 1))) return DDERR_INVALIDPARAMS;
  d->current = Arg(c, 1);
  ApplyViewport(*d);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::GetCurrentViewport", D3DD2_GetCurrentViewport, 2) {
  Device2* d = Dev(c);
  if (d->current) com::AddRef(d->current);
  Wr32(Arg(c, 1), d->current);
  return d->current ? D3D_OK : DDERR_INVALIDPARAMS;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::SetRenderState", D3DD2_SetRenderState, 3) {
  SetRenderState2(*Dev(c), Arg(c, 1), Arg(c, 2));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::GetRenderState", D3DD2_GetRenderState, 3) {
  Device2* d = Dev(c);
  uint32_t s = Arg(c, 1);
  Wr32(Arg(c, 2), s < 256 ? d->rs[s] : 0);
  return D3D_OK;
}

// D3DLIGHTSTATETYPE: MATERIAL 1, AMBIENT 2, COLORMODEL 3, FOGMODE 4,
// FOGSTART 5, FOGEND 6, FOGDENSITY 7.
HLE_NAMED("ddraw.dll", "IDirect3DDevice2::SetLightState", D3DD2_SetLightState, 3) {
  Device2* d = Dev(c);
  uint32_t s = Arg(c, 1), v = Arg(c, 2);
  if (s >= 8) return DDERR_INVALIDPARAMS;
  d->light_state[s] = v;
  switch (s) {
    case 1:
      g_material = v;
      ApplyMaterial(v);
      break;
    case 2: Gfx()->SetRenderState(RS_AMBIENT, v); break;
    case 4: Gfx()->SetRenderState(RS_FOGVERTEXMODE, v); break;
    case 5: Gfx()->SetRenderState(36, v); break;  // FOGSTART
    case 6: Gfx()->SetRenderState(37, v); break;  // FOGEND
    case 7: Gfx()->SetRenderState(38, v); break;  // FOGDENSITY
    default: break;  // COLORMODEL: always RGB
  }
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::GetLightState", D3DD2_GetLightState, 3) {
  Device2* d = Dev(c);
  uint32_t s = Arg(c, 1);
  if (s >= 8) return DDERR_INVALIDPARAMS;
  Wr32(Arg(c, 2), d->light_state[s]);
  return D3D_OK;
}

D3DMATRIX Transform(const Device2& d, uint32_t type) {
  auto it = d.transforms.find(type);
  if (it != d.transforms.end()) return it->second;
  D3DMATRIX m{};
  for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
  return m;
}

void PutTransform(Device2& d, uint32_t type, const D3DMATRIX& m) {
  d.transforms[type] = m;
  if (type == 3)
    ApplyProjection(d);
  else
    Gfx()->SetTransform(type, &m.m[0][0]);
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::SetTransform", D3DD2_SetTransform, 3) {
  PutTransform(*Dev(c), Arg(c, 1), *G<D3DMATRIX>(Arg(c, 2)));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice2::GetTransform", D3DD2_GetTransform, 3) {
  D3DMATRIX m = Transform(*Dev(c), Arg(c, 1));
  std::memcpy(GPtr(Arg(c, 2)), &m, sizeof m);
  return D3D_OK;
}

// The given matrix times the current one, as in Direct3D 7.
HLE_NAMED("ddraw.dll", "IDirect3DDevice2::MultiplyTransform", D3DD2_MultiplyTransform, 3) {
  Device2* d = Dev(c);
  D3DMATRIX a = Transform(*d, Arg(c, 1));
  const D3DMATRIX& b = *G<D3DMATRIX>(Arg(c, 2));
  D3DMATRIX r{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++) r.m[i][j] += b.m[i][k] * a.m[k][j];
  PutTransform(*d, Arg(c, 1), r);
  return D3D_OK;
}

// D3DDP_DONOTLIGHT
constexpr uint32_t kDoNotLight = 0x10;

void PrepareDraw(Device2& d, uint32_t vt, uint32_t flags) {
  bool light = vt == 1 && !(flags & kDoNotLight);
  if (d.rs[RS_LIGHTING] != uint32_t(light)) {
    d.rs[RS_LIGHTING] = light;
    Gfx()->SetRenderState(RS_LIGHTING, light);
  }
  // A texture released and recreated under the same handle.
  UpdateStage0(d);
}

// (primtype, vertex type, vertices, count, flags)
HLE_NAMED("ddraw.dll", "IDirect3DDevice2::DrawPrimitive", D3DD2_DrawPrimitive, 6) {
  Device2* d = Dev(c);
  uint32_t prim = Arg(c, 1), vt = Arg(c, 2), verts = Arg(c, 3), n = Arg(c, 4);
  uint32_t fvf = FvfFor(vt);
  PrepareDraw(*d, vt, Arg(c, 5));
  Gfx()->Draw(prim, fvf, GPtr(verts), n, gfx::FvfStride(fvf), nullptr, 0);
  return D3D_OK;
}

// (primtype, vertex type, vertices, vertex count, indices, index count, flags)
HLE_NAMED("ddraw.dll", "IDirect3DDevice2::DrawIndexedPrimitive", D3DD2_DrawIndexedPrimitive, 8) {
  Device2* d = Dev(c);
  uint32_t prim = Arg(c, 1), vt = Arg(c, 2), verts = Arg(c, 3), nv = Arg(c, 4),
           idx = Arg(c, 5), ni = Arg(c, 6);
  uint32_t fvf = FvfFor(vt);
  PrepareDraw(*d, vt, Arg(c, 7));
  Gfx()->Draw(prim, fvf, GPtr(verts), nv, gfx::FvfStride(fvf),
              reinterpret_cast<const uint16_t*>(GPtr(idx)), ni);
  return D3D_OK;
}

// ---- IDirect3DViewport2 ------------------------------------------------------

Viewport2* Vp(Ctx& c) { return Obj<Viewport2>(c, "IDirect3DViewport2"); }

HLE_NAMED("ddraw.dll", "IDirect3DViewport2::QueryInterface", D3DV2_QueryInterface, 3) {
  return QueryInterfaceSelf(c, kIID_IDirect3DViewport2);
}
HLE_NAMED("ddraw.dll", "IDirect3DViewport2::AddRef", D3DV2_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirect3DViewport2::Release", D3DV2_Release, 1) { return com::Release(Arg(c, 0)); }

uint32_t SetViewportX(Ctx& c, int version) {
  Viewport2* v = Vp(c);
  if (!v->device) return D3DERR_VIEWPORTHASNODEVICE;
  std::memcpy(&v->vp, GPtr(Arg(c, 1)), sizeof v->vp);
  v->version = version;
  if (v->device->current == v->guest) ApplyViewport(*v->device);
  return D3D_OK;
}

uint32_t GetViewportX(Ctx& c, int version) {
  Viewport2* v = Vp(c);
  D3DVIEWPORTX out = v->vp;
  if (v->version && v->version != version) {
    // Asked in the other form: describe the same mapping.
    float hw = float(out.dwWidth) / 2, hh = float(out.dwHeight) / 2;
    if (version == 1)
      out.f[0] = hw, out.f[1] = hh, out.f[2] = 1, out.f[3] = 1;
    else
      out.f[0] = -1, out.f[1] = 1, out.f[2] = 2, out.f[3] = 2;
  }
  out.dwSize = sizeof out;
  std::memcpy(GPtr(Arg(c, 1)), &out, sizeof out);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DViewport2::SetViewport", D3DV2_SetViewport, 2) { return SetViewportX(c, 1); }
HLE_NAMED("ddraw.dll", "IDirect3DViewport2::SetViewport2", D3DV2_SetViewport2, 2) { return SetViewportX(c, 2); }
HLE_NAMED("ddraw.dll", "IDirect3DViewport2::GetViewport", D3DV2_GetViewport, 2) { return GetViewportX(c, 1); }
HLE_NAMED("ddraw.dll", "IDirect3DViewport2::GetViewport2", D3DV2_GetViewport2, 2) { return GetViewportX(c, 2); }

HLE_NAMED("ddraw.dll", "IDirect3DViewport2::SetBackground", D3DV2_SetBackground, 2) {
  Vp(c)->background = Arg(c, 1);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DViewport2::GetBackground", D3DV2_GetBackground, 3) {
  Viewport2* v = Vp(c);
  Wr32(Arg(c, 1), v->background);
  Wr32(Arg(c, 2), v->background ? 1 : 0);
  return D3D_OK;
}

// (count, D3DRECT*, flags): the target to the background material's
// diffuse colour (black without one), Z to the far plane.
HLE_NAMED("ddraw.dll", "IDirect3DViewport2::Clear", D3DV2_Clear, 4) {
  Viewport2* v = Vp(c);
  if (!v->device) return D3DERR_VIEWPORTHASNODEVICE;
  uint32_t count = Arg(c, 1), rects = Arg(c, 2), flags = Arg(c, 3);
  uint32_t color = 0xFF000000;
  if (auto* m = v->background ? com::As<Material2>(v->background) : nullptr) {
    const D3DCOLORVALUE& d = m->m.diffuse;
    color = uint32_t(ToByte(d.a)) << 24 | uint32_t(ToByte(d.r)) << 16 |
            uint32_t(ToByte(d.g)) << 8 | ToByte(d.b);
  }
  if (!v->device->target || !v->device->target->zbuffer) flags &= ~2u;
  Gfx()->Clear(count, rects ? G<gfx::Rect>(rects) : nullptr, flags & 3, color, 1.0f, 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DViewport2::AddLight", D3DV2_AddLight, 2) {
  Viewport2* v = Vp(c);
  if (!com::As<Light2>(Arg(c, 1))) return DDERR_INVALIDPARAMS;
  com::AddRef(Arg(c, 1));
  v->lights.push_back(Arg(c, 1));
  if (v->device && v->device->current == v->guest) ApplyLights(*v->device);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DViewport2::DeleteLight", D3DV2_DeleteLight, 2) {
  Viewport2* v = Vp(c);
  auto it = std::find(v->lights.begin(), v->lights.end(), uint32_t(Arg(c, 1)));
  if (it == v->lights.end()) return DDERR_INVALIDPARAMS;
  v->lights.erase(it);
  com::Release(Arg(c, 1));
  if (v->device && v->device->current == v->guest) ApplyLights(*v->device);
  return D3D_OK;
}

// ---- IDirect3DMaterial2 --------------------------------------------------------

Material2* Mat(Ctx& c) { return Obj<Material2>(c, "IDirect3DMaterial2"); }

HLE_NAMED("ddraw.dll", "IDirect3DMaterial2::QueryInterface", D3DM2_QueryInterface, 3) {
  return QueryInterfaceSelf(c, kIID_IDirect3DMaterial2);
}
HLE_NAMED("ddraw.dll", "IDirect3DMaterial2::AddRef", D3DM2_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirect3DMaterial2::Release", D3DM2_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("ddraw.dll", "IDirect3DMaterial2::SetMaterial", D3DM2_SetMaterial, 2) {
  Material2* m = Mat(c);
  uint32_t size = Rd32(Arg(c, 1));
  if (size != sizeof(D3DMATERIAL)) return D3DERR_MATERIAL_SETDATA_FAILED;
  std::memcpy(&m->m, GPtr(Arg(c, 1)), sizeof m->m);
  if (m->guest == g_material) ApplyMaterial(m->guest);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DMaterial2::GetMaterial", D3DM2_GetMaterial, 2) {
  Material2* m = Mat(c);
  D3DMATERIAL out = m->m;
  out.dwSize = sizeof out;
  std::memcpy(GPtr(Arg(c, 1)), &out, sizeof out);
  return D3D_OK;
}

// (device, out handle): the material's interface pointer.
HLE_NAMED("ddraw.dll", "IDirect3DMaterial2::GetHandle", D3DM2_GetHandle, 3) {
  Wr32(Arg(c, 2), Mat(c)->guest);
  return D3D_OK;
}

// ---- IDirect3DLight -----------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirect3DLight::AddRef", D3DL_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirect3DLight::Release", D3DL_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("ddraw.dll", "IDirect3DLight::SetLight", D3DL_SetLight, 2) {
  auto* l = Obj<Light2>(c, "IDirect3DLight");
  uint32_t size = Rd32(Arg(c, 1));
  if (size != 76 && size != sizeof(D3DLIGHT2)) return DDERR_INVALIDPARAMS;
  l->l = {};
  std::memcpy(&l->l, GPtr(Arg(c, 1)), size);
  l->set = true;
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DLight::GetLight", D3DL_GetLight, 2) {
  auto* l = Obj<Light2>(c, "IDirect3DLight");
  uint32_t size = std::min<uint32_t>(Rd32(Arg(c, 1)), sizeof(D3DLIGHT2));
  D3DLIGHT2 out = l->l;
  out.dwSize = size;
  std::memcpy(GPtr(Arg(c, 1)), &out, size);
  return D3D_OK;
}

// ---- IDirect3DTexture2 ----------------------------------------------------------

Texture2* Tex(Ctx& c) { return Obj<Texture2>(c, "IDirect3DTexture2"); }

HLE_NAMED("ddraw.dll", "IDirect3DTexture2::QueryInterface", D3DT2_QueryInterface, 3) {
  return QueryInterfaceSelf(c, kIID_IDirect3DTexture2);
}
HLE_NAMED("ddraw.dll", "IDirect3DTexture2::AddRef", D3DT2_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirect3DTexture2::Release", D3DT2_Release, 1) { return com::Release(Arg(c, 0)); }

// (device, out handle): the texture surface's interface pointer.
HLE_NAMED("ddraw.dll", "IDirect3DTexture2::GetHandle", D3DT2_GetHandle, 3) {
  Wr32(Arg(c, 2), Tex(c)->surface);
  return D3D_OK;
}

// (source texture): copies the source surface's pixels (each mip level),
// colour key and palette into this texture, which is how games move a
// texture from system memory to video memory.
HLE_NAMED("ddraw.dll", "IDirect3DTexture2::Load", D3DT2_Load, 2) {
  Texture2* t = Tex(c);
  auto* st = com::As<Texture2>(Arg(c, 1));
  if (!st) return DDERR_INVALIDPARAMS;
  Surface* dst = com::As<Surface>(t->surface);
  Surface* src = com::As<Surface>(st->surface);
  if (!dst || !src) return DDERR_INVALIDPARAMS;
  Surface* src_top = src;
  while (dst && src) {
    if (dst->width != src->width || dst->height != src->height || dst->fmt != src->fmt) {
      WarnF("d3d2: Texture Load %dx%d fmt %u <- %dx%d fmt %u: unsupported\n", dst->width,
            dst->height, unsigned(dst->fmt), src->width, src->height, unsigned(src->fmt));
      return DDERR_INVALIDPARAMS;
    }
    if (dst->mem && src->mem)
      std::memcpy(GPtr(dst->mem), GPtr(src->mem),
                  size_t(dst->pitch) * gfx::RowCount(dst->fmt, uint32_t(dst->height)));
    if (!dst->keyed) {
      dst->has_ck_src = src->has_ck_src;
      dst->ck_src = src->ck_src;
    }
    UploadLevel(*dst);
    dst = dst->attached.empty() ? nullptr : dst->attached[0].get();
    src = src->attached.empty() ? nullptr : src->attached[0].get();
  }
  // The colour key comes along: key the destination (all levels).
  Surface* top = com::As<Surface>(t->surface);
  if (src_top->has_ck_src) {
    top->has_ck_src = true;
    top->ck_src = src_top->ck_src;
    KeyTexture(*top);
  }
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
