// The front end link (host_link.h).

#include "win32/host_link.h"

#include <poll.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "runtime/log.h"
#include "win32/user32_window.h"
#include "x86/present.h"
#include "x86/runtime.h"

namespace recomp {

namespace {

int LinkFd() {
  static const int fd = [] {
    const char* e = Env("LINK_FD");
    return e && *e ? std::atoi(e) : -1;
  }();
  return fd;
}

std::mutex g_link_mu;

// Bytes from the front end.
constexpr char kActivate = 'a';  // it has stepped back: take the focus
constexpr char kTookOver = 'x';  // answer to "exit"

#if defined(__APPLE__)
// macOS 14 activates apps cooperatively: the active app hands activation
// on (yieldActivationToApplication:) and the other one asks for it.
void* NsApp() {
  return reinterpret_cast<void* (*)(void*, SEL)>(objc_msgSend)(
      reinterpret_cast<void*>(objc_getClass("NSApplication")), sel_registerName("sharedApplication"));
}

void YieldActivationTo(pid_t pid) {
  void* app = NsApp();
  SEL yield = sel_registerName("yieldActivationToApplication:");
  if (!app || !class_respondsToSelector(object_getClass(reinterpret_cast<id>(app)), yield)) return;
  void* other = reinterpret_cast<void* (*)(void*, SEL, int)>(objc_msgSend)(
      reinterpret_cast<void*>(objc_getClass("NSRunningApplication")),
      sel_registerName("runningApplicationWithProcessIdentifier:"), int(pid));
  if (other) reinterpret_cast<void (*)(void*, SEL, void*)>(objc_msgSend)(app, yield, other);
}
#endif
struct {
  bool set = false;
  int x = 0, y = 0, w = 0, h = 0;
  bool fs = false;
} g_last;

}  // namespace

const HostPlacement* HandedPlacement() {
  static const HostPlacement* p = []() -> const HostPlacement* {
    const char* e = Env("WINDOW");
    if (!e || !*e) return nullptr;
    static HostPlacement hp;
    int fs = 0;
    float d = 0;
    int n = std::sscanf(e, "%d,%d,%d,%d,%d,%f", &hp.x, &hp.y, &hp.w, &hp.h, &fs, &d);
    if (n < 4 || hp.w <= 0 || hp.h <= 0) {
      WarnF("WINDOW=%s: expected x,y,w,h[,fullscreen[,density]]\n", e);
      return nullptr;
    }
    hp.fullscreen = n >= 5 && fs != 0;
    hp.density = n >= 6 ? d : 0;
    return &hp;
  }();
  return p;
}

bool HostLinked() { return LinkFd() >= 0; }

void HostLinkSend(const std::string& line) {
  const int fd = LinkFd();
  if (fd < 0) return;
  std::lock_guard<std::mutex> l(g_link_mu);
  std::string s = line + "\n";
  const char* p = s.data();
  size_t left = s.size();
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n <= 0) return;
    p += n;
    left -= size_t(n);
  }
}

bool HostLinkFrame(bool window_visible) {
  // A few frames after the window shows, so it is really on screen.
  static uint64_t first = 0;
  static bool sent = false;
  const int fd = LinkFd();
  if (fd < 0) return false;
  if (sent) {
    pollfd p{fd, POLLIN, 0};
    char b;
    return poll(&p, 1, 0) > 0 && read(fd, &b, 1) == 1 && b == kActivate;
  }
  if (!window_visible) return false;
  const uint64_t n = hle::PresentedFrames();
  if (!first) first = n;
  if (n < first + 3) return false;
  sent = true;
  HostLinkSend("shown");
  return false;
}

void HostLinkNotePlacement(int x, int y, int w, int h, bool fullscreen) {
  g_last = {true, x, y, w, h, fullscreen};
}

void HostLinkExit() {
  static bool done = false;
  const int fd = LinkFd();
  if (done || fd < 0) return;
  done = true;
  int x, y, w, h;
  bool fs;
  if (HostWindowPlacement(&x, &y, &w, &h, &fs)) HostLinkNotePlacement(x, y, w, h, fs);
  if (g_last.set) {
    x = g_last.x, y = g_last.y, w = g_last.w, h = g_last.h, fs = g_last.fs;
    std::fprintf(stderr, "host link: window %d,%d %dx%d%s at exit\n", x, y, w, h, fs ? " fullscreen" : "");
    char buf[96];
    std::snprintf(buf, sizeof buf, "window %d %d %d %d %d", x, y, w, h, fs ? 1 : 0);
    HostLinkSend(buf);
  }
#if defined(__APPLE__)
  YieldActivationTo(getppid());
#endif
  HostLinkSend("exit");
  bool acked = false;
  for (int waited = 0; !acked && waited < 2000; waited += 50) {
    pollfd p{fd, POLLIN, 0};
    char b;
    if (poll(&p, 1, 50) > 0) {
      if (read(fd, &b, 1) != 1) break;  // the front end is gone
      acked = b == kTookOver;
    }
  }
  std::fprintf(stderr, "host link: exit%s\n", acked ? ", front end took over" : " (no answer)");
}

}  // namespace recomp
