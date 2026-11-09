// Texture packs (texture_pack.h).

#include "win32/texture_pack.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "runtime/log.h"
#include "win32/ddraw_impl.h"

namespace recomp::ddraw {

namespace {

const std::string& PackDir() {
  static const std::string d = [] {
    const char* e = Env("TEX_PACK");
    return std::string(e ? e : g_proc->game.texture_pack.c_str());
  }();
  return d;
}

const std::string& DumpDir() {
  static const std::string d = [] {
    const char* e = Env("TEX_DUMP");
    return std::string(e ? e : "");
  }();
  return d;
}

// <ID>_TEX_LOG=1: log each replacement and each change the game makes to
// a replaced texture.
bool Log() {
  static const bool on = [] {
    const char* e = Env("TEX_LOG");
    return e && *e && *e != '0';
  }();
  return on;
}

std::string HashName(uint64_t h) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

// The pack's hashes, from its file names.
const std::unordered_set<uint64_t>& Available() {
  static const std::unordered_set<uint64_t> set = [] {
    std::unordered_set<uint64_t> s;
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(PackDir(), ec)) {
      const std::string n = f.path().filename().string();
      if (n.size() != 20 || n.compare(16, 4, ".png") != 0) continue;
      if (n.find_first_not_of("0123456789abcdef") < 16) continue;
      s.insert(std::stoull(n.substr(0, 16), nullptr, 16));
    }
    if (ec) WarnF("ddraw: texture pack %s: %s\n", PackDir().c_str(), ec.message().c_str());
    else WarnF("ddraw: texture pack %s: %zu textures\n", PackDir().c_str(), s.size());
    return s;
  }();
  return set;
}

// FNV-1a 64 (as text_hash.h), continued over `n` bytes.
uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

uint64_t Hash(const Surface& s, const std::vector<uint8_t>& px) {
  const guest::DDPIXELFORMAT& pf = s.desc.ddpfPixelFormat;
  const uint32_t head[7] = {uint32_t(s.width), uint32_t(s.height), gfx::BitsPerPixel(s.fmt),
                            pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask, pf.dwRGBAlphaBitMask};
  uint8_t b[28];
  for (int i = 0; i < 7; i++)
    for (int k = 0; k < 4; k++) b[4 * i + k] = uint8_t(head[i] >> (8 * k));
  return Fnv(Fnv(0xCBF29CE484222325ull, b, sizeof b), px.data(), px.size());
}

// The top level's texels as A8R8G8B8, the colour key's with alpha 0.
std::vector<uint32_t> Decode(const Surface& s, const std::vector<uint8_t>& raw) {
  const uint32_t bpp = gfx::BitsPerPixel(s.fmt) / 8;
  const uint32_t key = s.ck_src.dwColorSpaceLowValue;
  std::vector<uint32_t> px(size_t(s.width) * size_t(s.height));
  for (size_t i = 0; i < px.size(); i++) {
    const uint8_t* p = &raw[i * bpp];
    uint32_t argb = gfx::DecodePixel(p, s.fmt);
    if (s.has_ck_src && gfx::RawPixel(p, s.fmt) == key) argb &= 0x00FFFFFF;
    px[i] = argb;
  }
  return px;
}

void SavePng(const std::string& path, const std::vector<uint32_t>& argb, int w, int h) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
  SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888,
                                            const_cast<uint32_t*>(argb.data()), w * 4);
  if (!surf || !SDL_SavePNG(surf, path.c_str()))
    WarnF("ddraw: cannot write %s: %s\n", path.c_str(), SDL_GetError());
  if (surf) SDL_DestroySurface(surf);
#else
  (void)path, (void)argb, (void)w, (void)h;
#endif
}

// A8R8G8B8 pixels of a PNG; false if it cannot be read.
bool LoadPng(const std::string& path, std::vector<uint32_t>* px, int* w, int* h) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
  SDL_Surface* raw = SDL_LoadPNG(path.c_str());
  if (!raw) {
    WarnF("ddraw: cannot read %s: %s\n", path.c_str(), SDL_GetError());
    return false;
  }
  SDL_Surface* surf = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(raw);
  if (!surf) return false;
  *w = surf->w;
  *h = surf->h;
  px->resize(size_t(*w) * size_t(*h));
  for (int y = 0; y < *h; y++)
    std::memcpy(&(*px)[size_t(y) * size_t(*w)],
                static_cast<const uint8_t*>(surf->pixels) + size_t(y) * size_t(surf->pitch),
                size_t(*w) * 4);
  SDL_DestroySurface(surf);
  return true;
#else
  static bool warned = false;
  if (!warned) WarnF("ddraw: texture packs need SDL 3.4 (PNG)\n");
  warned = true;
  (void)path, (void)px, (void)w, (void)h;
  return false;
#endif
}

int Levels(int w, int h) {
  int n = 1;
  for (int m = std::max(w, h); m > 1; m >>= 1) n++;
  return n;
}

// Texel (x, y) of mip `level` from the top level: the average of the
// block below it, colour weighted by alpha (transparent texels do not
// darken the edges), alpha averaged.
uint32_t MipTexel(const std::vector<uint32_t>& top, int w, int h, int level, int x, int y) {
  const int x0 = x << level, y0 = y << level;
  const int x1 = std::min(w, (x + 1) << level), y1 = std::min(h, (y + 1) << level);
  uint64_t sum[3] = {}, asum = 0, n = 0;
  for (int j = y0; j < y1; j++)
    for (int i = x0; i < x1; i++) {
      const uint32_t p = top[size_t(j) * size_t(w) + size_t(i)], a = p >> 24;
      asum += a;
      n++;
      for (int c = 0; c < 3; c++) sum[c] += uint64_t((p >> (8 * c)) & 0xFF) * a;
    }
  if (!n) return 0;
  uint32_t out = uint32_t((asum + n / 2) / n) << 24;
  for (int c = 0; c < 3; c++) out |= uint32_t(asum ? (sum[c] + asum / 2) / asum : 0) << (8 * c);
  return out;
}

// Uploads `r` of the top level `top` (w x h) to `tex` and the levels below
// it that `r` covers.
void UploadWithMips(gfx::TextureId tex, const std::vector<uint32_t>& top, int w, int h,
                    gfx::Rect r) {
  gfx::Device* g = Gfx();
  g->UpdateTexture(tex, 0, r, &top[size_t(r.top) * size_t(w) + size_t(r.left)], uint32_t(w) * 4);
  const bool whole = r.left == 0 && r.top == 0 && r.right == w && r.bottom == h;
  std::vector<uint32_t> prev = whole ? top : std::vector<uint32_t>{}, cur;
  int pw = w, ph = h;
  for (int l = 1; l < Levels(w, h); l++) {
    const int lw = std::max(1, w >> l), lh = std::max(1, h >> l);
    const gfx::Rect lr{r.left >> l, r.top >> l, std::min(lw, ((r.right - 1) >> l) + 1),
                       std::min(lh, ((r.bottom - 1) >> l) + 1)};
    const int rw = lr.right - lr.left, rh = lr.bottom - lr.top;
    cur.assign(size_t(rw) * size_t(rh), 0);
    for (int y = 0; y < rh; y++)
      for (int x = 0; x < rw; x++) {
        uint32_t v;
        if (whole) {
          // Whole chain: from the level above (2x2), much cheaper.
          v = MipTexel(prev, pw, ph, 1, lr.left + x, lr.top + y);
        } else {
          v = MipTexel(top, w, h, l, lr.left + x, lr.top + y);
        }
        cur[size_t(y) * size_t(rw) + size_t(x)] = v;
      }
    g->UpdateTexture(tex, l, lr, cur.data(), uint32_t(rw) * 4);
    if (whole) {
      prev.swap(cur);
      pw = lw;
      ph = lh;
    }
  }
}

struct Entry {
  gfx::TextureId tex = 0;  // 0: unreadable
  int w = 0, h = 0;
  std::vector<uint32_t> px;  // its top level
};
std::unordered_map<uint64_t, Entry> g_cache;

// The pack's texture for `hash`, loaded on first use (null if unreadable).
const Entry* Load(uint64_t hash) {
  auto it = g_cache.find(hash);
  if (it == g_cache.end()) {
    Entry e;
    const std::string path = PackDir() + "/" + HashName(hash) + ".png";
    if (LoadPng(path, &e.px, &e.w, &e.h)) {
      e.tex = Gfx()->CreateTexture(e.w, e.h, Levels(e.w, e.h), gfx::Format::A8R8G8B8, false);
      if (e.tex) UploadWithMips(e.tex, e.px, e.w, e.h, {0, 0, e.w, e.h});
    }
    if (!e.tex) e.px.clear();
    it = g_cache.emplace(hash, std::move(e)).first;
  }
  return it->second.tex ? &it->second : nullptr;
}

void DropOwn(PackState& st) {
  if (st.own) Gfx()->DestroyTexture(st.own);
  st.own = 0;
  st.own_px.clear();
  st.own_px.shrink_to_fit();
}

void Drop(Surface& s, PackState& st) {
  Gfx()->SetTextureReplacement(s.tex, 0);
  DropOwn(st);
  st.hash = 0;
  st.hd = st.bound = 0;
  st.base.clear();
}

void Bind(Surface& s, PackState& st) {
  if (st.bound && st.bound != s.tex) Gfx()->SetTextureReplacement(st.bound, 0);
  Gfx()->SetTextureReplacement(s.tex, st.hd);
  st.bound = s.tex;
}

// The game changed texels in `r` (top-level texels, exclusive) of a
// replaced texture: those of its own copy that sit on a changed texel
// become the game's, scaled up bilinearly; those on a texel as it was when
// matched get the pack's back.
void Patch(Surface& s, PackState& st, const Entry& e, const std::vector<uint8_t>& raw,
           gfx::Rect r) {
  const int w = s.width, h = s.height, bpp = int(gfx::BitsPerPixel(s.fmt) / 8);
  if (!st.own) {
    st.own_px = e.px;
    st.own = Gfx()->CreateTexture(e.w, e.h, Levels(e.w, e.h), gfx::Format::A8R8G8B8, false);
    if (!st.own) return Drop(s, st);
    UploadWithMips(st.own, st.own_px, e.w, e.h, {0, 0, e.w, e.h});
    st.hd = st.own;
    Bind(s, st);
  }
  const std::vector<uint32_t> cur = Decode(s, raw);
  const double sx = double(e.w) / w, sy = double(e.h) / h;
  // One texel more around: bilinear reads its neighbours.
  r = {std::max(0, r.left - 1), std::max(0, r.top - 1), std::min(w, r.right + 1),
       std::min(h, r.bottom + 1)};
  const gfx::Rect hr{int(r.left * sx), int(r.top * sy), std::min(e.w, int(std::ceil(r.right * sx))),
                     std::min(e.h, int(std::ceil(r.bottom * sy)))};
  for (int Y = hr.top; Y < hr.bottom; Y++)
    for (int X = hr.left; X < hr.right; X++) {
      const int ox = std::min(w - 1, int(X / sx)), oy = std::min(h - 1, int(Y / sy));
      const size_t o = size_t(oy) * size_t(w) + size_t(ox);
      uint32_t& d = st.own_px[size_t(Y) * size_t(e.w) + size_t(X)];
      if (!std::memcmp(&raw[o * bpp], &st.base[o * bpp], size_t(bpp))) {
        d = e.px[size_t(Y) * size_t(e.w) + size_t(X)];
        continue;
      }
      // Bilinear at the texel centre, colour weighted by alpha.
      const double fx = std::clamp((X + 0.5) / sx - 0.5, 0.0, double(w - 1));
      const double fy = std::clamp((Y + 0.5) / sy - 0.5, 0.0, double(h - 1));
      const int x0 = int(fx), y0 = int(fy), x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
      const double ax = fx - x0, ay = fy - y0;
      const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
      const double ws[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
      double sum[3] = {}, asum = 0;
      for (int k = 0; k < 4; k++) {
        const uint32_t p = cur[size_t(ys[k]) * size_t(w) + size_t(xs[k])];
        const double a = (p >> 24) * ws[k];
        asum += a;
        for (int c = 0; c < 3; c++) sum[c] += ((p >> (8 * c)) & 0xFF) * a;
      }
      uint32_t out = uint32_t(asum + 0.5) << 24;
      for (int c = 0; c < 3; c++) out |= uint32_t(asum > 0 ? sum[c] / asum + 0.5 : 0) << (8 * c);
      d = out;
    }
  UploadWithMips(st.own, st.own_px, e.w, e.h, hr);
}

}  // namespace

PackState::~PackState() {
  if (own && St().gfx) St().gfx->DestroyTexture(own);
}

void PackUpload(Surface& s) {
  if (s.level != 0 || s.kind != Surface::Kind::Texture || !s.mem || !s.tex ||
      (s.desc.ddsCaps.dwCaps & DDSCAPS_3DDEVICE) || gfx::IsCompressed(s.fmt) ||
      s.fmt == gfx::Format::P8 || gfx::BitsPerPixel(s.fmt) % 8)
    return;
  const bool pack = !PackDir().empty(), dump = !DumpDir().empty();
  if (!pack && !dump) return;
  const size_t row = size_t(s.width) * gfx::BitsPerPixel(s.fmt) / 8;
  std::vector<uint8_t> raw(row * size_t(s.height));
  for (int y = 0; y < s.height; y++)
    std::memcpy(&raw[size_t(y) * row], GPtr(s.mem + uint32_t(y) * s.pitch), row);
  const uint64_t hash = Hash(s, raw);

  if (dump) {
    static std::unordered_set<uint64_t> seen;
    if (seen.insert(hash).second) {
      std::error_code ec;
      std::filesystem::create_directories(DumpDir(), ec);
      SavePng(DumpDir() + "/" + HashName(hash) + ".png", Decode(s, raw), s.width, s.height);
    }
  }
  if (!pack) return;
  if (!s.pack) {
    if (!Available().count(hash)) return;
    s.pack = std::make_unique<PackState>();
  }
  PackState& st = *s.pack;

  if (Available().count(hash) && !(st.hd && st.hash == hash && !st.own)) {
    if (const Entry* e = Load(hash)) {
      if (Log()) WarnF("ddraw: texture %dx%d %s replaced (%dx%d)\n", s.width, s.height,
                       HashName(hash).c_str(), e->w, e->h);
      DropOwn(st);
      st.hash = hash;
      st.hd = e->tex;
      st.base = raw;
      st.last = std::move(raw);
      Bind(s, st);
      return;
    }
  }
  if (!st.hd) {
    st.last = std::move(raw);
    return;
  }
  if (st.bound != s.tex) Bind(s, st);  // the surface has a new texture (colour key)
  if (raw.size() != st.last.size() || raw.size() != st.base.size()) {
    Drop(s, st);
    st.last = std::move(raw);
    return;
  }
  // What changed since the last upload.
  const int bpp = int(gfx::BitsPerPixel(s.fmt) / 8);
  gfx::Rect r{s.width, s.height, 0, 0};
  for (int y = 0; y < s.height; y++) {
    const uint8_t *a = &raw[size_t(y) * row], *b = &st.last[size_t(y) * row];
    if (!std::memcmp(a, b, row)) continue;
    int x0 = 0, x1 = s.width;
    while (!std::memcmp(a + x0 * bpp, b + x0 * bpp, size_t(bpp))) x0++;
    while (!std::memcmp(a + (x1 - 1) * bpp, b + (x1 - 1) * bpp, size_t(bpp))) x1--;
    r = {std::min(r.left, x0), std::min(r.top, y), std::max(r.right, x1), std::max(r.bottom, y + 1)};
  }
  if (r.right <= r.left) return;
  // A large change is another texture, not a few copied pixels.
  if (int64_t(r.right - r.left) * (r.bottom - r.top) * 4 > int64_t(s.width) * s.height) {
    if (Log()) WarnF("ddraw: texture %s became another one: original\n", HashName(st.hash).c_str());
    Drop(s, st);
    st.last = std::move(raw);
    return;
  }
  if (Log())
    WarnF("ddraw: texture %s changed at %d,%d %dx%d: patched\n", HashName(st.hash).c_str(), r.left,
          r.top, r.right - r.left, r.bottom - r.top);
  if (const Entry* e = Load(st.hash)) Patch(s, st, *e, raw, r);
  st.last = std::move(raw);
}

}  // namespace recomp::ddraw
