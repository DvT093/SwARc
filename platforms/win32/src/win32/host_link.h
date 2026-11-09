#pragma once
// Running as the child of a front end (a launcher or menu that starts the
// game for each session and takes over again afterwards), so the two feel
// like one program: the game's window appears exactly where the front
// end's was, and the front end gets its window back in the same place.
//
//   <ID>_WINDOW=x,y,w,h[,fullscreen[,density]]
//     Host placement of the game's top-level windows in SDL points (global
//     coordinates): a window of w x h at (x, y), or with fullscreen=1
//     fullscreen on the display that holds (x, y), without a separate
//     Space or its animation on macOS. The game's frames are fitted into
//     it with their aspect kept, whatever size the game draws at; the
//     game's own window sizes and the fullscreen policy no longer move or
//     resize it. density (pixels per point) is for the game's own sizing.
//   <ID>_LINK_FD=n
//     A stream socket to the front end. The runtime writes lines:
//       "shown"                once the game's window shows its frames
//       "window x y w h fs"    the window's placement, just before exit
//       "closed"               just before "exit" when the user closed
//                              the game's window (close button, Cmd+Q):
//                              the front end quits too, the two are one
//                              program to the user
//       "exit"                 then waits (up to 2 s) for the byte 'x':
//                              the front end has its own window up again,
//                              so the desktop never shows in between
//     and reads the byte 'a' (the front end has stepped back after
//     "shown"): the game window then takes the keyboard focus (on macOS 14
//     and later the front end yields activation to the game first, and
//     the game yields it back before "exit").
//     plus the game's own lines (HostLinkSend). On macOS the game then
//     runs as an accessory app: no Dock icon of its own.
//
// Both are optional and independent.

#include <string>

namespace recomp {

struct HostPlacement {
  int x = 0, y = 0, w = 0, h = 0;  // points
  bool fullscreen = false;
  float density = 0;  // 0: not given
};

// <ID>_WINDOW, or null.
const HostPlacement* HandedPlacement();

// Whether <ID>_LINK_FD is set (a front end listens).
bool HostLinked();
// Sends one line (without '\n') to the front end; nothing if not linked.
void HostLinkSend(const std::string& line);

// Runtime hooks: after each presented frame (with a visible window;
// returns true when the game window should take the focus now), and once
// just before the process exits (sends placement and "exit", waits).
bool HostLinkFrame(bool window_visible);
void HostLinkExit();
// The user asked to close the game's window (SDL quit / close request):
// HostLinkExit sends "closed" first.
void HostLinkNoteUserClose();
// The game window's placement now (points, as HostWindowPlacement): the
// last one noted goes to the front end at exit.
void HostLinkNotePlacement(int x, int y, int w, int h, bool fullscreen);

}  // namespace recomp
