// Frame presentation shared by the graphics APIs (Win32's ddraw, opengl32,
// a console platform's video output).

#include "x86/present.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "runtime/log.h"
#include "runtime/pacing.h"
#include "x86/runtime.h"
#include "x86/watchdog.h"

namespace recomp::hle {

namespace {

// ---- image quality enhancements (GameConfig::gfx) and their keys

// The enhanced profile: GameConfig::gfx with the environment's overrides.
const gfx::Enhancements& EnhancedProfile() {
  static const gfx::Enhancements e = [] {
    gfx::Enhancements p = g_proc->game.gfx;
    if (const char* v = Env("MSAA")) p.msaa = std::atoi(v);
    if (const char* v = Env("A2C")) p.alpha_to_coverage = std::atoi(v) != 0;
    if (const char* v = Env("MIPMAPS")) p.mipmaps = std::atoi(v) != 0;
    if (const char* v = Env("ANISOTROPY")) p.anisotropy = std::atoi(v);
    p.msaa = std::clamp(p.msaa, 1, 8);
    p.anisotropy = std::clamp(p.anisotropy, 1, 16);
    p.build_mips = p.mipmaps;
    return p;
  }();
  return e;
}

bool StartEnhanced() {
  const char* v = Env("GFX_ENHANCED");
  return v ? std::atoi(v) != 0 : g_proc->game.gfx_enhanced;
}

// Which parts are on (switched by GfxKey), and whether the device has
// not seen the change yet.
std::atomic<bool> g_aa_on{true}, g_filter_on{true}, g_gfx_changed{false};
std::once_flag g_gfx_once;

void InitGfxState() {
  std::call_once(g_gfx_once, [] {
    bool on = StartEnhanced();
    g_aa_on = on;
    g_filter_on = on;
  });
}

gfx::Enhancements CurrentEnhancements() {
  InitGfxState();
  gfx::Enhancements e = EnhancedProfile();
  if (!g_aa_on) {
    e.msaa = 1;
    e.alpha_to_coverage = false;
  }
  if (!g_filter_on) {
    e.mipmaps = false;
    e.anisotropy = 1;
  }
  return e;
}

// A 5x7 font for the overlay: rows top to bottom, bit 4 the leftmost
// pixel. Upper case only; other characters show as blanks.
struct Glyph {
  char c;
  uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
};

// `text` in white on a dark box, `scale` screen pixels per font pixel.
std::vector<uint32_t> RenderText(const std::string& text, int scale, int* w, int* h) {
  const int pad = 3 * scale, cell = 6 * scale;
  *w = int(text.size()) * cell - scale + 2 * pad;
  *h = 7 * scale + 2 * pad;
  std::vector<uint32_t> px(size_t(*w) * size_t(*h), 0xC0000000u);
  for (size_t i = 0; i < text.size(); i++) {
    char ch = char(std::toupper(static_cast<unsigned char>(text[i])));
    const Glyph* g = nullptr;
    for (const Glyph& f : kFont)
      if (f.c == ch) g = &f;
    if (!g) continue;
    for (int y = 0; y < 7 * scale; y++)
      for (int x = 0; x < 5 * scale; x++)
        if (g->rows[y / scale] & (0x10 >> (x / scale)))
          px[size_t(pad + y) * size_t(*w) + size_t(pad + int(i) * cell + x)] = 0xFFFFFFFFu;
  }
  return px;
}

// What the overlay says about enhancements `e`.
std::string Describe(gfx::Enhancements e, const gfx::Device& dev) {
  const GameConfig& g = g_proc->game;
  if (e.msaa > 1) e.msaa = dev.MsaaSamples();
  if (e.msaa <= 1 && !e.mipmaps && e.anisotropy <= 1) return g.gfx_label_original;
  std::string s = g.gfx_label_enhanced;
  if (e.msaa > 1) s += "  MSAA " + std::to_string(e.msaa) + "X";
  if (e.anisotropy > 1) s += "  AF " + std::to_string(e.anisotropy) + "X";
  else if (e.mipmaps) s += "  MIPMAPS";
  return s;
}

// SetHostOverlay's image, waiting for the next presented frame.
struct HostOverlay {
  std::vector<uint32_t> argb;
  int w = 0, h = 0, x = 0, y = 0;
};
std::mutex g_host_overlay_mu;
HostOverlay g_host_overlay;
bool g_host_overlay_on = false, g_host_overlay_changed = false;

// Applies a switch made by GfxKey to `dev` and shows it for a moment
// (not while the host shows an image of its own: that says it already).
void UpdateEnhancements(gfx::Device& dev, int w, int h) {
  static Uint64 overlay_until = 0;
  bool host_on, host_changed;
  {
    std::lock_guard<std::mutex> l(g_host_overlay_mu);
    host_on = g_host_overlay_on;
    host_changed = std::exchange(g_host_overlay_changed, false);
    if (host_changed) {
      const HostOverlay& o = g_host_overlay;
      dev.SetOverlay(host_on ? o.argb.data() : nullptr, host_on ? o.w : 0, host_on ? o.h : 0, o.x, o.y);
      overlay_until = 0;
    }
  }
  if (g_gfx_changed.exchange(false)) {
    gfx::Enhancements e = CurrentEnhancements();
    dev.SetEnhancements(e);
    const std::string text = Describe(e, dev);
    WarnF("gfx: %s\n", text.c_str());
    if (host_on) return;
    int scale = std::max(2, h / 240), ow, oh;
    std::vector<uint32_t> img = RenderText(text, scale, &ow, &oh);
    ow = std::min(ow, w);
    if (ow > 0) {
      dev.SetOverlay(img.data(), ow, oh, 4 * scale, 4 * scale);
      overlay_until = SDL_GetTicks() + 2500;
    }
  } else if (overlay_until && SDL_GetTicks() >= overlay_until) {
    dev.SetOverlay(nullptr, 0, 0, 0, 0);
    overlay_until = 0;
  }
}

std::atomic<uint64_t> g_presented{0};

}  // namespace

void SwitchGfx(GfxPart part) {
  InitGfxState();
  if (part == GfxPart::All) {
    bool on = !(g_aa_on && g_filter_on);
    g_aa_on = on;
    g_filter_on = on;
  } else if (part == GfxPart::AntiAliasing) {
    g_aa_on = !g_aa_on;
  } else {
    g_filter_on = !g_filter_on;
  }
  g_gfx_changed = true;
}

bool GfxOn(GfxPart part) {
  InitGfxState();
  if (part == GfxPart::AntiAliasing) return g_aa_on;
  if (part == GfxPart::Filtering) return g_filter_on;
  return g_aa_on && g_filter_on;
}

void SetHostOverlay(const uint32_t* argb, int w, int h, int x, int y) {
  std::lock_guard<std::mutex> l(g_host_overlay_mu);
  g_host_overlay_on = argb && w > 0 && h > 0;
  g_host_overlay_changed = true;
  if (g_host_overlay_on) {
    g_host_overlay.argb.assign(argb, argb + size_t(w) * size_t(h));
    g_host_overlay.w = w;
    g_host_overlay.h = h;
    g_host_overlay.x = x;
    g_host_overlay.y = y;
  } else {
    g_host_overlay = {};
  }
}

uint64_t PresentedFrames() { return g_presented.load(); }

bool GfxKey(int scancode, bool down) {
  const GameConfig& g = g_proc->game;
  if (!scancode) return false;
  const bool all = scancode == g.gfx_key_all, aa = scancode == g.gfx_key_aa,
             filter = scancode == g.gfx_key_filter;
  if (!all && !aa && !filter) return false;
  if (!down) return true;
  SwitchGfx(all ? GfxPart::All : aa ? GfxPart::AntiAliasing : GfxPart::Filtering);
  return true;
}

// Dev aid: <ID>_SHOT_EVERY=N saves every Nth presented frame (from frame
// <ID>_SHOT_FROM on) as $<ID>_SHOT_DIR/frame_NNNNN.bmp (default dir: current
// directory): the back buffer, or with <ID>_SHOT_FRONT=1 the frame as shown
// (with the host's overlay).
static void SaveShot(gfx::Device& dev, int w, int h, int frame, bool front) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  if (front)
    dev.ReadBackFront({0, 0, w, h}, px.data(), uint32_t(w * 4), gfx::Format::X8R8G8B8);
  else
    dev.ReadBack({0, 0, w, h}, px.data(), uint32_t(w * 4), gfx::Format::X8R8G8B8);
  SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888,
                                            px.data(), w * 4);
  const char* dir = Env("SHOT_DIR");
  char path[512];
  std::snprintf(path, sizeof path, "%s/frame_%05d.bmp", dir ? dir : ".", frame);
  if (surf) {
    SDL_SaveBMP(surf, path);
    SDL_DestroySurface(surf);
  }
}

void PresentFrame(gfx::Device& dev, int w, int h) {
  static const int every = [] {
    const char* e = Env("SHOT_EVERY");
    return e ? std::atoi(e) : 0;
  }();
  static const int from = [] {
    const char* e = Env("SHOT_FROM");
    return e ? std::atoi(e) : 0;
  }();
  static const bool front = [] {
    const char* e = Env("SHOT_FRONT");
    return e && *e && *e != '0';
  }();
  static int frame = 0;
  frame++;
  const bool shot = every > 0 && frame >= from && frame % every == 0;
  if (shot && !front) SaveShot(dev, w, h, frame, false);
  UpdateEnhancements(dev, w, h);
  dev.Present();
  g_presented++;
  if (shot && front) SaveShot(dev, w, h, frame, true);
  // GameConfig::max_fps / <ID>_MAX_FPS: wait for the next frame's slot.
  static const double max_fps = [] {
    const char* e = Env("MAX_FPS");
    return e ? std::atof(e) : g_proc->game.max_fps;
  }();
  if (max_fps > 0) {
    static FramePacer pacer;
    pacer.Wait(max_fps);
  }
  // Loading screens present without pumping messages; keep answering the
  // window system (compositor pings) so the window does not look hung.
  // Events are only queued for the game, as Windows would.
  if (g_proc->pump_events) g_proc->pump_events();
  WatchdogFrame();
  // <ID>_HITCH=ms: report frames that took longer than that.
  static const double hitch_ms = [] {
    const char* e = Env("HITCH");
    return e ? std::atof(e) : 0.0;
  }();
  if (hitch_ms > 0) {
    static Uint64 last = 0, start = 0;
    Uint64 now = SDL_GetTicksNS();
    if (!start) start = now;
    double dt = last ? (now - last) / 1e6 : 0;
    if (dt > hitch_ms)
      WarnF("hitch: frame %d took %.1f ms (t=%.2f s)\n", frame, dt,
            (now - start) / 1e9);
    last = now;
  }
  // <ID>_FPS_REPORT=s: every s seconds, the frame rate and the spread of
  // frame intervals.
  static const double report_s = [] {
    const char* e = Env("FPS_REPORT");
    return e ? std::atof(e) : 0.0;
  }();
  if (report_s > 0) {
    static Uint64 last = 0, first = 0, t0 = 0;
    static std::vector<double> dts;
    Uint64 now = SDL_GetTicksNS();
    if (!t0) t0 = now;
    if (last) dts.push_back((now - last) / 1e6);
    if (!first) first = now;
    last = now;
    if ((now - first) / 1e9 >= report_s && !dts.empty()) {
      std::sort(dts.begin(), dts.end());
      auto q = [&](double p) { return dts[size_t(p * double(dts.size() - 1) + 0.5)]; };
      WarnF("fps: %.2f at t=%.1f s (interval ms: min %.3f p1 %.3f median %.3f p99 %.3f max %.3f)\n",
            dts.size() / ((now - first) / 1e9), (now - t0) / 1e9, dts.front(), q(0.01),
            q(0.5), q(0.99), dts.back());
      dts.clear();
      first = now;
    }
  }
}

gfx::Backend GfxBackend() {
  static const gfx::Backend b = [] {
    const char* e = Env("GFX");
    if (e && !std::strcmp(e, "gpu")) return gfx::Backend::Gpu;
    if (e && !std::strcmp(e, "d3d9")) {
#ifdef RECOMP_HAVE_D3D9
      return gfx::Backend::D3D9;
#else
      FatalF("graphics backend d3d9 is not in this build (no dxvk-native)");
#endif
    }
    if (e && *e) FatalF("unknown graphics backend '%s' (d3d9 or gpu)", e);
    return gfx::Backend::Gpu;
  }();
  return b;
}

uint64_t GfxWindowFlags() {
  switch (GfxBackend()) {
    case gfx::Backend::D3D9:
      return SDL_WINDOW_VULKAN;  // dxvk's SDL3 WSI makes a Vulkan surface
    case gfx::Backend::Gpu:
#if defined(__APPLE__)
      return SDL_WINDOW_METAL;
#else
      return SDL_WINDOW_VULKAN;
#endif
  }
  return 0;
}

std::unique_ptr<gfx::Device> CreateGfxDevice(SDL_Window* w) {
  std::unique_ptr<gfx::Device> d = gfx::CreateDevice(GfxBackend(), w);
  if (!d) FatalF("cannot create the graphics device");
  d->SetEnhancements(CurrentEnhancements());
  return d;
}

}  // namespace recomp::hle
