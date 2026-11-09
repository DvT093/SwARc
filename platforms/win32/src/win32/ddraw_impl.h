#pragma once
// Host objects behind the DirectDraw 7 / Direct3D 7 interfaces, shared by
// ddraw.cpp (DirectDraw, surfaces) and d3d7.cpp (Direct3D, devices,
// vertex buffers). ddraw.cpp also serves the older DirectDraw interfaces
// (IDirectDraw, IDirectDraw2, IDirectDrawSurface, IDirectDrawSurface2)
// with the same objects; `version` on an object says which structure
// sizes its caller uses.

#include <memory>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/lazy_mem.h"
#include "win32/com.h"
#include "win32/ddraw_types.h"
#include "win32/texture_pack.h"
#include "x86/runtime.h"

namespace recomp::ddraw {

// DDSCAPS
enum : uint32_t {
  DDSCAPS_BACKBUFFER = 0x4, DDSCAPS_COMPLEX = 0x8,
  DDSCAPS_FLIP = 0x10, DDSCAPS_FRONTBUFFER = 0x20,
  DDSCAPS_OFFSCREENPLAIN = 0x40, DDSCAPS_PRIMARYSURFACE = 0x200,
  DDSCAPS_SYSTEMMEMORY = 0x800, DDSCAPS_TEXTURE = 0x1000,
  DDSCAPS_3DDEVICE = 0x2000, DDSCAPS_VIDEOMEMORY = 0x4000,
  DDSCAPS_ZBUFFER = 0x20000, DDSCAPS_MIPMAP = 0x400000,
  DDSCAPS_LOCALVIDMEM = 0x10000000, DDSCAPS_NONLOCALVIDMEM = 0x8000000,
};
enum : uint32_t {
  DDSCAPS2_TEXTUREMANAGE = 0x10, DDSCAPS2_MIPMAPSUBLEVEL = 0x10000,
  DDSCAPS2_D3DTEXTUREMANAGE = 0x20000,
};
// DDSD flags
enum : uint32_t {
  DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
  DDSD_BACKBUFFERCOUNT = 0x20, DDSD_ZBUFFERBITDEPTH = 0x40,
  DDSD_LPSURFACE = 0x800, DDSD_PIXELFORMAT = 0x1000,
  DDSD_CKSRCBLT = 0x10000, DDSD_MIPMAPCOUNT = 0x20000,
  DDSD_REFRESHRATE = 0x40000,
};
// DDPIXELFORMAT flags
enum : uint32_t {
  DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4,
  DDPF_PALETTEINDEXED8 = 0x20, DDPF_RGB = 0x40, DDPF_ZBUFFER = 0x400,
  DDPF_STENCILBUFFER = 0x4000, DDPF_LUMINANCE = 0x20000,
};

constexpr guest::GUID kIID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

constexpr uint32_t DD_OK = 0;
constexpr uint32_t DDERR_INVALIDPARAMS = 0x80070057;
constexpr uint32_t DDERR_NOTFOUND = 0x887600FF;
constexpr uint32_t DDERR_UNSUPPORTED = 0x80004001;
constexpr uint32_t DDERR_OUTOFVIDEOMEMORY = 0x8876017C;
constexpr uint32_t DDERR_NOTLOCKED = 0x887601E6;
constexpr uint32_t DDERR_NOCOLORKEY = 0x887600D7;
constexpr uint32_t DDERR_INVALIDPIXELFORMAT = 0x88760091;

// DDSURFACEDESC (DirectDraw 1-5) is the first 108 bytes of DDSURFACEDESC2:
// the same layout up to ddsCaps, which is a DDSCAPS (dwCaps only, at the
// same offset) instead of a DDSCAPS2 followed by dwTextureStage.
constexpr uint32_t kSurfaceDescSize = 108;
static_assert(offsetof(guest::DDSURFACEDESC2, ddsCaps) + sizeof(guest::DDSCAPS) ==
              kSurfaceDescSize);

// Bytes of a surface description / of surface caps that a caller of
// interface `version` (1, 2 or 7) passes.
inline uint32_t SurfaceDescSize(int version) {
  return version >= 4 ? uint32_t(sizeof(guest::DDSURFACEDESC2)) : kSurfaceDescSize;
}
inline uint32_t SurfaceCapsSize(int version) {
  return version >= 4 ? uint32_t(sizeof(guest::DDSCAPS2)) : uint32_t(sizeof(guest::DDSCAPS));
}

template <typename T>
T* G(uint32_t va) {
  return reinterpret_cast<T*>(GPtr(va));
}

gfx::Format FormatFromPixelFormat(const guest::DDPIXELFORMAT& pf);
void PixelFormatFromFormat(gfx::Format f, guest::DDPIXELFORMAT* pf);

struct Surface;

// Process-wide DirectDraw state. The game has one real DirectDraw object
// (others exist briefly during enumeration) and one gfx device.
struct State {
  uint32_t hwnd = 0;
  uint32_t coop_flags = 0;
  int mode_w = 0, mode_h = 0, mode_bpp = 0;
  std::unique_ptr<gfx::Device> gfx;
  Surface* backbuffer = nullptr;  // first back buffer (render target)
  bool vsync = false;
  // Bytes of textures created in explicit video memory (not managed):
  // counted against VidMemFree() so CreateSurface fails when the card is
  // full, as games that probe texture memory by allocating expect.
  uint32_t vidmem_used = 0;
};
State& St();
// The gfx device, created on first use for the cooperative window.
gfx::Device* Gfx();

struct Surface : com::Object {
  // Interface version the surface was created through: 7, or 2 for
  // surfaces of an IDirectDraw/IDirectDraw2 (published as
  // IDirectDrawSurface2, which is also an IDirectDrawSurface).
  int version = 7;
  enum class Kind { Primary, BackBuffer, Texture, ZBuffer, Offscreen };
  Kind kind = Kind::Offscreen;
  guest::DDSURFACEDESC2 desc{};  // as reported by GetSurfaceDesc
  gfx::Format fmt = gfx::Format::Unknown;
  int width = 0, height = 0;
  uint32_t pitch = 0;
  uint32_t mem = 0;  // guest memory (lock shadow / system memory surface)
  gfx::TextureId tex = 0;  // texture (shared by all mip levels)
  int level = 0;           // mip level of this surface in `tex`
  uint32_t vidmem = 0;     // bytes counted in State::vidmem_used (top level)
  gfx::DepthId depth = 0;
  std::vector<std::shared_ptr<Surface>> attached;  // chain / mips / z
  Surface* zbuffer = nullptr;   // attached Z buffer (render targets)
  bool has_ck_src = false;
  guest::DDCOLORKEY ck_src{};
  // Texture of an older interface (DirectX 5 and before) with a source
  // colour key: its gfx texture is A8R8G8B8 with alpha 0 where a texel
  // equals the key, for Direct3D 2's D3DRENDERSTATE_COLORKEYENABLE (the gfx
  // device has no colour keys; d3d2.cpp turns it into an alpha test).
  bool keyed = false;
  bool locked = false;
  // Back buffer and primary: `mem` mirrors the GPU image lazily. Lock
  // reads back only what the game touches, Unlock uploads only rows it
  // changed (games often lock just to wait for the GPU).
  std::unique_ptr<lazy::LazyImage> shadow;
  uint32_t priority = 0, lod = 0;
  // Its replacement from a texture pack (texture_pack.h), if it has had one.
  std::unique_ptr<PackState> pack;
  ~Surface() override;
};

// Uploads a texture level's guest memory to the gfx texture.
void UploadLevel(Surface& s);
// Makes a top-level texture of an older interface with a source colour key
// `keyed` (all its levels), or uploads it again for a changed key.
void KeyTexture(Surface& top);

// New IDirect3D7 for an IDirectDraw7 (QueryInterface), d3d7.cpp.
uint32_t D3D7For(uint32_t dd);

// Shared with Direct3D 2 (d3d2.cpp), from d3d7.cpp: the HAL device's
// caps, the device's initial render and texture stage states, and the
// FPU switch to single precision that device creation makes unless the
// cooperative level asked to keep it (`api` names the caller in the log).
guest::D3DDEVICEDESC7 DeviceDesc7(bool tnl);
void InitDeviceStates(uint32_t rs[256], uint32_t tss[8][32]);
void DeviceFpuSetup(Ctx& c, const char* api);

// New IDirect3D2 for an IDirectDraw2 (QueryInterface), and a new
// IDirect3DTexture2 for a texture surface (QueryInterface), d3d2.cpp.
uint32_t D3D2For(uint32_t dd);
uint32_t Texture2For(uint32_t surface);

}  // namespace recomp::ddraw
