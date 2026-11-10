// Embedded in a front end's window (host_link.h, <ID>_EMBED): the game
// window's Metal layer as a remote layer (CAContext) that the front end
// shows with a CALayerHost. Both are private QuartzCore classes, the
// way browsers show their GPU process's frames in the browser window.

#include "win32/host_embed.h"

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <cstdio>

#include "runtime/log.h"

namespace recomp {

namespace {

using Obj = void*;

template <typename R = Obj, typename... A>
R Msg(Obj o, const char* sel, A... a) {
  return reinterpret_cast<R (*)(Obj, SEL, A...)>(objc_msgSend)(o, sel_registerName(sel), a...);
}
Obj Cls(const char* name) { return reinterpret_cast<Obj>(objc_getClass(name)); }
bool IsKind(Obj o, const char* cls) {
  Obj c = Cls(cls);
  return o && c && Msg<bool>(o, "isKindOfClass:", c);
}

Obj g_context = nullptr;  // CAContext, retained
Obj g_root = nullptr;     // its layer (CALayer), retained
Obj g_metal = nullptr;    // SDL's CAMetalLayer inside it
Obj g_view = nullptr;     // SDL's Metal view, retained, out of the window

void SetGeometry(int w, int h, float density) {
  if (!g_root || w <= 0 || h <= 0) return;
  if (!(density > 0)) density = 1;
  Obj tx = Cls("CATransaction");
  Msg<void>(tx, "begin");
  Msg<void>(tx, "setDisableActions:", true);
  const CGRect r = CGRectMake(0, 0, w, h);
  // SDL sizes the drawable from its view's bounds on a window resize:
  // the view, out of the window, no longer follows the window itself.
  if (g_view) Msg<void>(g_view, "setFrame:", r);
  Msg<void>(g_root, "setBounds:", r);
  Msg<void>(g_root, "setPosition:", CGPointMake(0, 0));
  Msg<void>(g_metal, "setAnchorPoint:", CGPointMake(0, 0));
  Msg<void>(g_metal, "setPosition:", CGPointMake(0, 0));
  Msg<void>(g_metal, "setBounds:", r);
  Msg<void>(g_metal, "setContentsScale:", CGFloat(density));
  Msg<void>(g_metal, "setDrawableSize:", CGSizeMake(w * density, h * density));
  Msg<void>(tx, "commit");
  Msg<void>(tx, "flush");
}

}  // namespace

uint32_t EmbedExportLayer(Obj nswindow, int w, int h, float density) {
  if (g_context) return Msg<uint32_t>(g_context, "contextId");
  if (!nswindow) return 0;
  Obj content = Msg(nswindow, "contentView");
  Obj subviews = content ? Msg(content, "subviews") : nullptr;
  const unsigned long n = subviews ? Msg<unsigned long>(subviews, "count") : 0;
  Obj view = nullptr, metal = nullptr;
  for (unsigned long i = 0; i < n && !metal; i++) {
    Obj v = Msg(subviews, "objectAtIndex:", i);
    Obj l = Msg(v, "layer");
    if (IsKind(l, "CAMetalLayer")) view = v, metal = l;
  }
  if (!metal) return 0;  // the GPU device has not claimed the window yet
  using MainConnection = uint32_t (*)();
  auto cgs = reinterpret_cast<MainConnection>(dlsym(RTLD_DEFAULT, "CGSMainConnectionID"));
  Obj ctx_cls = Cls("CAContext");
  if (!cgs || !ctx_cls) {
    WarnF("host link: no CAContext on this system, the game cannot draw into the front end\n");
    return 0;
  }
  Obj opts = Msg(Cls("NSDictionary"), "dictionaryWithObject:forKey:",
                 Msg(Cls("NSString"), "stringWithUTF8String:", "ignore"),
                 Msg(Cls("NSString"), "stringWithUTF8String:", "kCAContextCIFilterBehavior"));
  Obj ctx = Msg(ctx_cls, "contextWithCGSConnection:options:", cgs(), opts);
  if (!ctx) return 0;
  g_context = Msg(ctx, "retain");
  // The view leaves the window: AppKit would otherwise put its layer
  // back into the window's tree (a layer has one parent).
  g_view = Msg(view, "retain");
  Msg<void>(view, "removeFromSuperview");
  g_metal = metal;
  g_root = Msg(Msg(Cls("CALayer"), "layer"), "retain");
  Msg<void>(g_root, "setAnchorPoint:", CGPointMake(0, 0));
  Msg<void>(g_root, "addSublayer:", metal);
  Msg<void>(g_context, "setLayer:", g_root);
  SetGeometry(w, h, density);
  const uint32_t id = Msg<uint32_t>(g_context, "contextId");
  std::fprintf(stderr, "host link: Metal layer exported as context %u (%dx%d points)\n", id, w, h);
  return id;
}

void EmbedResize(int w, int h, float density) { SetGeometry(w, h, density); }

}  // namespace recomp

#else

namespace recomp {
uint32_t EmbedExportLayer(void*, int, int, float) { return 0; }
void EmbedResize(int, int, float) {}
}  // namespace recomp

#endif
