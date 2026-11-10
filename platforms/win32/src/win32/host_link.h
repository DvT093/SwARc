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
//   <ID>_EMBED=1 (macOS, with <ID>_LINK_FD)
//     The game draws into the front end's own window: to macOS there is
//     one app with one window, so the front end's fullscreen (with its own
//     Space and animation) stays as the game starts and ends. The game's
//     window is borderless, off screen, never activated and never
//     fullscreen; its Metal layer goes to the front end as a remote layer:
//       "layer <context id>"   a CAContext; the front end shows it with a
//                              CALayerHost over its window
//     The front end then sends lines (instead of the bytes above):
//       "key d|u <scancode> <mod> <repeat>", "motion <x> <y> <xrel> <yrel>"
//       (points in its window), "button d|u <button> <clicks>",
//       "wheel <dy>", "leave", "focus 1|0", "quit" (Cmd+Q, close button),
//       "size <w> <h> <density>" (its window, points), and "x" after
//       "exit". The game sends "cursor 0|1" (show the pointer) and
//       "relmouse 0|1" (SDL's relative mouse mode) for the front end's
//       window. A game without this support never sends "layer", and the
//       front end falls back to the handover above.
//       "msgbox <flags> <def> <id>:<label>,... \t<caption>\t<text>" asks the
//       front end to show a message box over its window (flags as SDL's,
//       def the index of the default button, \n in the texts as "\\n"); it
//       answers "msgbox <id>" (the button chosen). The game's own process
//       would have to become the active app to show one, and macOS would
//       leave the front end's fullscreen Space.
//
// All are optional and independent.

#include <string>
#include <vector>

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
// <ID>_EMBED (host_link.h, top): the game draws into the front end's window.
bool HostEmbedded();
// <ID>_EMBED is set, whether or not this system can do it (a front end
// asking for it has its fullscreen in a Space of its own).
bool HostEmbedRequested();
// Embedded: the lines the front end sent since the last call.
void HostLinkReadLines(std::vector<std::string>* out);
// Embedded: sends `line` and waits for the answer that starts with
// `prefix` (other lines wait for HostLinkReadLines). False if the front
// end went away.
bool HostLinkAsk(const std::string& line, const std::string& prefix, std::string* answer);
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
