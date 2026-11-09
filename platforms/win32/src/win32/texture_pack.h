#pragma once
// Texture packs: replacements for a game's textures, recognised by their
// pixels (as console emulators' texture packs are), so that the game
// itself needs no change. GameConfig::texture_pack (or <ID>_TEX_PACK) is
// a directory of PNG files named <hash>.png, 16 lowercase hex digits:
// FNV-1a 64 (text_hash.h) over seven little-endian u32 - width, height,
// bits per pixel and the red, green, blue and alpha masks of the texture's
// DDPIXELFORMAT - followed by its top level's pixels as the game wrote
// them, row after row without padding (width * bits / 8 bytes each).
//
// A PNG may be any size (normally a whole multiple of the original's) and
// carries its own alpha: for a colour-keyed texture, 0 where the key is.
// The replacement gets a mip chain of its own and is drawn instead of the
// game's texture while gfx::Enhancements::replace_textures is on
// (gfx::Device::SetTextureReplacement); the game keeps its own texture
// and never sees the replacement.
//
// Recognised on every upload of a texture's top level, so a surface that
// a game's texture cache reuses for other textures follows them. When a
// game copies a few pixels into a replaced texture at run time (a lamp
// switched on, a sign), the replacement stays and gets those pixels,
// scaled up bilinearly; where the pixels go back to what they were, the
// replacement's own come back. A larger change drops the replacement
// until the texture matches one again.
//
// <ID>_TEX_DUMP=dir writes every texture the game uploads, once per hash,
// as dir/<hash>.png (the key's texels transparent): how a pack's author
// finds the hashes. <ID>_TEX_LOG=1 logs each replacement and each change
// the game makes to a replaced texture.

#include <cstdint>
#include <memory>
#include <vector>

#include "runtime/gfx/device.h"

namespace recomp::ddraw {

struct Surface;

// What a surface's replacement is; owned by the surface.
struct PackState {
  uint64_t hash = 0;            // the matched texture's
  gfx::TextureId bound = 0;     // the surface texture the replacement is set on
  gfx::TextureId hd = 0;        // the replacement (shared, or `own`)
  gfx::TextureId own = 0;       // a private copy, after a run-time change
  std::vector<uint32_t> own_px; // its top level, A8R8G8B8
  std::vector<uint8_t> base;    // the top level's pixels as matched
  std::vector<uint8_t> last;    // as last uploaded
  ~PackState();
};

// A texture's top level was uploaded (s.level == 0): match it against the
// pack, follow a change, or dump it.
void PackUpload(Surface& s);

}  // namespace recomp::ddraw
