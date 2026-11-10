#pragma once
// Embedded in a front end's window (host_link.h, <ID>_EMBED): exporting
// the game window's Metal layer. macOS only; elsewhere these do nothing.

#include <cstdint>

namespace recomp {

// Moves the Metal layer of nswindow (an NSWindow*, whose GPU device has
// claimed it) into a remote context of w x h points at density pixels
// per point, and returns the context's id for the front end; 0 while
// there is no Metal layer yet. Later calls return the same id.
uint32_t EmbedExportLayer(void* nswindow, int w, int h, float density);
// The front end's window has a new size.
void EmbedResize(int w, int h, float density);

}  // namespace recomp
