// dinput8: DirectInput8Create, IDirectInput8A and IDirectInputDevice8A for
// the system keyboard and mouse; and dinput: DirectInputCreateA,
// IDirectInputA/IDirectInput2A and IDirectInputDeviceA/IDirectInputDevice2A
// for games written against DirectX 3 and 5.
//
// The older interfaces are the DirectInput 8 ones cut short (IDirectInput8A
// starts with IDirectInputA's 8 methods, IDirectInputDevice8A with
// IDirectInputDevice2A's 27, same arguments), so most of their methods are
// the DirectInput 8 handlers registered under the older names. What
// differs: the device types (DIDEVTYPE_* instead of DI8DEVTYPE_*), so
// EnumDevices has its own handler, and the sizes of DIDEVCAPS and
// DIDEVICEINSTANCEA, which DirectX 3 callers pass without the fields
// DirectX 5 added; objects carry the version DirectInputCreateA was
// given for that.
//
// Input comes from the event pump's raw input (input_state.h): the
// keyboard by DIK code, the mouse in mickeys. Each acquired device keeps
// its own state and, with DIPROP_BUFFERSIZE set, a buffer of events.
// Game controllers are not enumerated: games that read pads through
// XInput find them there.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/com.h"
#include "win32/hle.h"
#include "win32/input_state.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "win32/xinput.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr GUID kGUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kIID_IDirectInputDevice8A = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
constexpr GUID kIID_IDirectInputA = {0x89521360, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_IDirectInput2A = {0x5944E662, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_IDirectInputDeviceA = {0x5944E680, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_IDirectInputDevice2A = {0x5944E682, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

constexpr uint32_t DI_OK = 0;
constexpr uint32_t DI_BUFFEROVERFLOW = 1;
constexpr uint32_t DI_NOEFFECT = 1;
constexpr uint32_t DIERR_INVALIDPARAM = 0x80070057;
constexpr uint32_t DIERR_NOINTERFACE = 0x80004002;
constexpr uint32_t DIERR_UNSUPPORTED = 0x80004001;
constexpr uint32_t DIERR_DEVICENOTREG = 0x80040154;
constexpr uint32_t DIERR_NOTACQUIRED = 0x8007000C;
constexpr uint32_t DIERR_ACQUIRED = 0x800700AA;

// Device classes (EnumDevices) and types.
constexpr uint32_t DI8DEVCLASS_ALL = 0, DI8DEVCLASS_DEVICE = 1,
                   DI8DEVCLASS_POINTER = 2, DI8DEVCLASS_KEYBOARD = 3,
                   DI8DEVCLASS_GAMECTRL = 4;
constexpr uint32_t DI8DEVTYPE_MOUSE = 0x12, DI8DEVTYPE_KEYBOARD = 0x13,
                   DI8DEVTYPE_GAMEPAD = 0x15;
constexpr uint32_t kMouseType = DI8DEVTYPE_MOUSE | (2 << 8);        // traditional
constexpr uint32_t kKeyboardType = DI8DEVTYPE_KEYBOARD | (4 << 8);  // PC enhanced
constexpr uint32_t kGamepadType = DI8DEVTYPE_GAMEPAD | (2 << 8) | 0x10000;  // standard, HID
// Before DirectInput 8 (EnumDevices filter and dwDevType alike).
constexpr uint32_t DIDEVTYPE_MOUSE = 2, DIDEVTYPE_KEYBOARD = 3,
                   DIDEVTYPE_JOYSTICK = 4;
constexpr uint32_t kLegacyMouseType = DIDEVTYPE_MOUSE | (2 << 8);        // traditional
constexpr uint32_t kLegacyKeyboardType = DIDEVTYPE_KEYBOARD | (4 << 8);  // PC enhanced
// DIDEVCAPS and DIDEVICEINSTANCEA as DirectX 3 has them (DirectX 5 added
// the force feedback fields and the HID usage).
constexpr uint32_t kDIDEVCAPS_DX3 = 24, kDIDEVICEINSTANCEA_DX3 = 560;

constexpr uint32_t DISCL_EXCLUSIVE = 0x1;
constexpr uint32_t DIGDD_PEEK = 0x1;
constexpr uint32_t DIPROP_BUFFERSIZE = 1, DIPROP_AXISMODE = 2;
constexpr uint32_t DIPROPAXISMODE_ABS = 0;
constexpr uint32_t DIDC_ATTACHED = 0x1;

// Mouse data offsets (DIMOUSESTATE / DIMOUSESTATE2).
constexpr uint32_t DIMOFS_X = 0, DIMOFS_Y = 4, DIMOFS_Z = 8, DIMOFS_BUTTON0 = 12;

struct DIDEVCAPS {
  uint32_t dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs,
      dwFFSamplePeriod, dwFFMinTimeResolution, dwFirmwareRevision,
      dwHardwareRevision, dwFFDriverVersion;
};
static_assert(sizeof(DIDEVCAPS) == 44);

struct DIDEVICEINSTANCEA {
  uint32_t dwSize;
  GUID guidInstance, guidProduct;
  uint32_t dwDevType;
  char tszInstanceName[260], tszProductName[260];
  GUID guidFFDriver;
  uint16_t wUsagePage, wUsage;
};
static_assert(sizeof(DIDEVICEINSTANCEA) == 580);

// DIDEVICEOBJECTDATA; DirectX 3 apps pass the 16-byte form without uAppData.
struct DIDEVICEOBJECTDATA {
  uint32_t dwOfs, dwData, dwTimeStamp, dwSequence, uAppData;
};

// `version`: DirectInput8Create's 0x800, or what a DirectX 3/5 game gave
// DirectInputCreateA (0x300, 0x500); its devices get it too.
struct Direct8 : com::Object {
  uint32_t version = 0x800;
};

struct InputDevice : com::Object {
  uint32_t version = 0x800;
  bool keyboard = false;
  uint32_t data_size = 0;  // the data format's state size; 0 = not set
  uint32_t coop = 0;
  uint32_t hwnd = 0;
  bool acquired = false;
  bool grabbed = false;  // exclusive mouse: host pointer in relative mode
  uint32_t buffer_size = 0;
  bool overflow = false;
  std::deque<DIDEVICEOBJECTDATA> buffer;
  // Mouse: absolute position (DIPROPAXISMODE_ABS) or motion since the last
  // GetDeviceState, wheel, buttons.
  bool absolute = false;
  int32_t axis[3] = {};
  uint8_t buttons[8] = {};
  // Keyboard: DIK state.
  uint8_t keys[256] = {};
  ~InputDevice() override;
};

std::mutex g_mu;
std::set<InputDevice*> g_acquired;  // devices that get raw input
uint32_t g_sequence = 0;

void Record(InputDevice& d, uint32_t ofs, uint32_t data, uint32_t time) {
  if (!d.buffer_size) return;
  if (d.buffer.size() >= d.buffer_size) {
    d.buffer.pop_front();
    d.overflow = true;
  }
  d.buffer.push_back({ofs, data, time, ++g_sequence, 0});
}

void OnRawInput(const RawInput& in) {
  std::lock_guard<std::mutex> l(g_mu);
  for (InputDevice* d : g_acquired) {
    if (d->keyboard) {
      if (in.kind != RawInput::Key) continue;
      uint8_t v = in.value ? 0x80 : 0;
      if (d->keys[in.code] == v) continue;
      d->keys[in.code] = v;
      Record(*d, in.code, v, in.time_ms);
      continue;
    }
    switch (in.kind) {
      case RawInput::MouseX:
      case RawInput::MouseY:
      case RawInput::Wheel: {
        int a = in.kind == RawInput::MouseX ? 0 : in.kind == RawInput::MouseY ? 1 : 2;
        d->axis[a] += in.value;
        // Buffered axis data is the motion (relative) or the position.
        uint32_t data = uint32_t(d->absolute ? d->axis[a] : in.value);
        Record(*d, a == 0 ? DIMOFS_X : a == 1 ? DIMOFS_Y : DIMOFS_Z, data, in.time_ms);
        break;
      }
      case RawInput::MouseButton:
        if (in.code < 8) {
          d->buttons[in.code] = in.value ? 0x80 : 0;
          Record(*d, DIMOFS_BUTTON0 + in.code, d->buttons[in.code], in.time_ms);
        }
        break;
      default:
        break;
    }
  }
}

// The exclusive mouse hides the pointer and reports motion without
// bounds, as on Windows: SDL's relative mode on the device's window.
void Grab(InputDevice& d, bool on) {
  if (d.grabbed == on || d.keyboard || !(d.coop & DISCL_EXCLUSIVE) || Background())
    return;
  if (SDL_Window* w = SdlWindowFor(d.hwnd)) SDL_SetWindowRelativeMouseMode(w, on);
  d.grabbed = on;
}

void Unacquire(InputDevice& d) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_acquired.erase(&d);
  }
  if (d.acquired) Grab(d, false);
  d.acquired = false;
}

InputDevice::~InputDevice() { Unacquire(*this); }

uint32_t DirectInputVtable(uint32_t version) {
  static const std::vector<const char*> methods = {
      "QueryInterface", "AddRef", "Release", "CreateDevice",
      "EnumDevices", "GetDeviceStatus", "RunControlPanel",
      "Initialize", "FindDevice", "EnumDevicesBySemantics",
      "ConfigureDevices"};
  if (version >= 0x800) return com::Vtable("dinput8.dll", "IDirectInput8A", methods);
  // IDirectInput2A: IDirectInputA's 8 methods and FindDevice; one vtable
  // serves both.
  return com::Vtable("dinput.dll", "IDirectInput2A", {methods.begin(), methods.begin() + 9});
}

uint32_t DeviceVtable(uint32_t version) {
  static const std::vector<const char*> methods = {
      "QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
       "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState",
       "GetDeviceData", "SetDataFormat", "SetEventNotification",
       "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo",
       "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects",
       "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
       "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData",
       "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap",
       "SetActionMap", "GetImageInfo"};
  if (version >= 0x800) return com::Vtable("dinput8.dll", "IDirectInputDevice8A", methods);
  // IDirectInputDevice2A: the first 27 (IDirectInputDeviceA's 18, then the
  // force feedback methods, Poll and SendDeviceData); one vtable serves
  // both.
  return com::Vtable("dinput.dll", "IDirectInputDevice2A", {methods.begin(), methods.begin() + 27});
}

InputDevice* Dev(Ctx& c) {
  InputDevice* d = com::As<InputDevice>(Arg(c, 0));
  if (!d) FatalF("IDirectInputDevice8A call on unknown object %08X", uint32_t(Arg(c, 0)));
  return d;
}

// Fills the DIDEVICEINSTANCEA at `di` for the keyboard or the mouse: its
// first `size` bytes (580, or 560 for DirectX 3 callers), with the device
// types of DirectInput `version`.
void FillInstance(DIDEVICEINSTANCEA* di, bool keyboard, uint32_t version = 0x800,
                  uint32_t size = sizeof(DIDEVICEINSTANCEA)) {
  DIDEVICEINSTANCEA f{};
  f.dwSize = size;
  f.guidInstance = f.guidProduct = keyboard ? kGUID_SysKeyboard : kGUID_SysMouse;
  if (version >= 0x800)
    f.dwDevType = keyboard ? kKeyboardType : kMouseType;
  else
    f.dwDevType = keyboard ? kLegacyKeyboardType : kLegacyMouseType;
  std::snprintf(f.tszInstanceName, sizeof f.tszInstanceName, keyboard ? "Keyboard" : "Mouse");
  std::snprintf(f.tszProductName, sizeof f.tszProductName, keyboard ? "Keyboard" : "Mouse");
  f.wUsagePage = 1;             // generic desktop
  f.wUsage = keyboard ? 6 : 2;  // keyboard / mouse
  std::memcpy(di, &f, size);
}

void FillPadInstance(DIDEVICEINSTANCEA* di, const XInputPad& pad) {
  std::memset(di, 0, sizeof *di);
  di->dwSize = sizeof *di;
  di->guidInstance = {0x9A6F1D20 + pad.index, 0xD5A0, 0x11CF, {'X', 'I', 'N', 'P', 'U', 'T', 0, 0}};
  di->guidProduct = {pad.vid | uint32_t(pad.pid) << 16, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
  di->dwDevType = kGamepadType;
  std::snprintf(di->tszInstanceName, sizeof di->tszInstanceName, "%s", pad.name.c_str());
  std::snprintf(di->tszProductName, sizeof di->tszProductName, "%s", pad.name.c_str());
  di->wUsagePage = 1;  // generic desktop
  di->wUsage = 5;      // game pad
}

uint32_t QueryInterfaceFor(Ctx& c, const GUID& iid) {
  if (com::GuidEq(Arg(c, 1), iid) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return DI_OK;
  }
  Wr32(Arg(c, 2), 0);
  return DIERR_NOINTERFACE;
}

// (hinst, dwVersion, riidltf, ppvOut, punkOuter)
HLE("dinput8.dll", DirectInput8Create, 5) {
  uint32_t out = Arg(c, 3);
  if (!com::GuidEq(Arg(c, 2), kIID_IDirectInput8A)) {
    Wr32(out, 0);
    return DIERR_NOINTERFACE;
  }
  RECOMP_TRACE("  DirectInput8Create version %X\n", uint32_t(Arg(c, 1)));
  Wr32(out, com::Publish(std::make_shared<Direct8>(), DirectInputVtable(0x800)));
  return DI_OK;
}

// DirectInputCreateA(hinst, dwVersion, lplpDirectInput, punkOuter): an
// IDirectInputA (also an IDirectInput2A) for DirectX 3 or 5 callers.
HLE("dinput.dll", DirectInputCreateA, 4) {
  uint32_t version = Arg(c, 1), out = Arg(c, 2);
  RECOMP_TRACE("  DirectInputCreateA version %X\n", version);
  if (Arg(c, 3)) {
    Wr32(out, 0);
    return DIERR_NOINTERFACE;  // no aggregation
  }
  if (version < 0x300 || version >= 0x800) {
    Wr32(out, 0);
    // DIERR_OLDDIRECTINPUTVERSION / DIERR_BETADIRECTINPUTVERSION
    return version < 0x300 ? 0x8007047E : 0x80070481;
  }
  auto di = std::make_shared<Direct8>();
  di->version = version;
  Wr32(out, com::Publish(di, DirectInputVtable(version)));
  return DI_OK;
}

// ---- IDirectInput8A ----------------------------------------------------------------

HLE_NAMED("dinput8.dll", "IDirectInput8A::QueryInterface", DI8_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirectInput8A);
}
// IDirectInputA and IDirectInput2A are the same object.
HLE_NAMED("dinput.dll", "IDirectInput2A::QueryInterface", DI2_QueryInterface, 3) {
  if (com::GuidEq(Arg(c, 1), kIID_IDirectInputA)) return QueryInterfaceFor(c, kIID_IDirectInputA);
  return QueryInterfaceFor(c, kIID_IDirectInput2A);
}

HLE_NAMED("dinput8.dll", "IDirectInput8A::AddRef", DI8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInput8A::Release", DI8_Release, 1) { return com::Release(Arg(c, 0)); }

// (rguid, lplpDirectInputDevice, pUnkOuter)
HLE_NAMED("dinput8.dll", "IDirectInput8A::CreateDevice", DI8_CreateDevice, 4) {
  uint32_t out = Arg(c, 2);
  bool keyboard = com::GuidEq(Arg(c, 1), kGUID_SysKeyboard);
  if (!keyboard && !com::GuidEq(Arg(c, 1), kGUID_SysMouse)) {
    Wr32(out, 0);
    return DIERR_DEVICENOTREG;
  }
  Direct8* di = com::As<Direct8>(Arg(c, 0));
  auto d = std::make_shared<InputDevice>();
  d->keyboard = keyboard;
  d->version = di ? di->version : 0x800;
  Wr32(out, com::Publish(d, DeviceVtable(d->version)));
  SetRawInputSink(&OnRawInput);
  return DI_OK;
}

// (dwDevType, lpCallback, pvRef, dwFlags): the keyboard, the mouse and the
// XInput pads.
HLE_NAMED("dinput8.dll", "IDirectInput8A::EnumDevices", DI8_EnumDevices, 5) {
  uint32_t type = Arg(c, 1), cb = Arg(c, 2), ref = Arg(c, 3);
  uint32_t cls = type & 0xFF;
  bool mouse = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_POINTER || cls == DI8DEVTYPE_MOUSE;
  bool keyboard = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_KEYBOARD || cls == DI8DEVTYPE_KEYBOARD;
  bool pads = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_GAMECTRL || cls == DI8DEVTYPE_GAMEPAD;
  if (cls != DI8DEVCLASS_ALL && cls != DI8DEVCLASS_DEVICE && cls != DI8DEVCLASS_POINTER &&
      cls != DI8DEVCLASS_KEYBOARD && cls != DI8DEVCLASS_GAMECTRL && cls < 0x11)
    return DIERR_INVALIDPARAM;
  uint32_t inst = HeapAllocProcess(sizeof(DIDEVICEINSTANCEA), true);
  auto* di = reinterpret_cast<DIDEVICEINSTANCEA*>(GPtr(inst));
  bool more = true;
  for (bool kb : {true, false}) {
    if (!more || (kb ? !keyboard : !mouse)) continue;
    FillInstance(di, kb);
    more = CallGuest(c, cb, {inst, ref});  // 0 = DIENUM_STOP
  }
  if (pads)
    for (const XInputPad& pad : XInputPads()) {
      if (!more) break;
      FillPadInstance(di, pad);
      more = CallGuest(c, cb, {inst, ref});
    }
  HeapFreeProcess(inst);
  return DI_OK;
}

// IDirectInputA::EnumDevices(dwDevType, lpCallback, pvRef, dwFlags) with
// the older device types: the keyboard and the mouse. No joysticks (the
// devices cannot be created yet).
HLE_NAMED("dinput.dll", "IDirectInput2A::EnumDevices", DI2_EnumDevices, 5) {
  Direct8* self = com::As<Direct8>(Arg(c, 0));
  uint32_t version = self ? self->version : 0x500;
  uint32_t type = Arg(c, 1) & 0xFF, cb = Arg(c, 2), ref = Arg(c, 3);
  if (type > DIDEVTYPE_JOYSTICK) return DIERR_INVALIDPARAM;
  bool mouse = type == 0 || type == DIDEVTYPE_MOUSE;
  bool keyboard = type == 0 || type == DIDEVTYPE_KEYBOARD;
  uint32_t size = version >= 0x500 ? sizeof(DIDEVICEINSTANCEA) : kDIDEVICEINSTANCEA_DX3;
  uint32_t inst = HeapAllocProcess(sizeof(DIDEVICEINSTANCEA), true);
  auto* di = reinterpret_cast<DIDEVICEINSTANCEA*>(GPtr(inst));
  bool more = true;
  for (bool kb : {true, false}) {
    if (!more || (kb ? !keyboard : !mouse)) continue;
    FillInstance(di, kb, version, size);
    more = CallGuest(c, cb, {inst, ref});  // 0 = DIENUM_STOP
  }
  HeapFreeProcess(inst);
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInput8A::GetDeviceStatus", DI8_GetDeviceStatus, 2) {
  return com::GuidEq(Arg(c, 1), kGUID_SysKeyboard) || com::GuidEq(Arg(c, 1), kGUID_SysMouse)
             ? DI_OK
             : DIERR_DEVICENOTREG;
}

// ---- IDirectInputDevice8A ----------------------------------------------------------

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::QueryInterface", DID8_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirectInputDevice8A);
}
// IDirectInputDeviceA and IDirectInputDevice2A are the same object.
HLE_NAMED("dinput.dll", "IDirectInputDevice2A::QueryInterface", DID2_QueryInterface, 3) {
  if (com::GuidEq(Arg(c, 1), kIID_IDirectInputDeviceA))
    return QueryInterfaceFor(c, kIID_IDirectInputDeviceA);
  return QueryInterfaceFor(c, kIID_IDirectInputDevice2A);
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::AddRef", DID8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Release", DID8_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetCapabilities", DID8_GetCapabilities, 2) {
  InputDevice* d = Dev(c);
  uint32_t size = Rd32(Arg(c, 1));
  // DirectX 3 callers pass the DIDEVCAPS without the force feedback fields.
  bool ok = size == sizeof(DIDEVCAPS) || (d->version < 0x800 && size == kDIDEVCAPS_DX3);
  if (!ok) return DIERR_INVALIDPARAM;
  DIDEVCAPS caps{};
  caps.dwSize = size;
  caps.dwFlags = DIDC_ATTACHED;
  if (d->version >= 0x800)
    caps.dwDevType = d->keyboard ? kKeyboardType : kMouseType;
  else
    caps.dwDevType = d->keyboard ? kLegacyKeyboardType : kLegacyMouseType;
  caps.dwAxes = d->keyboard ? 0 : 3;
  caps.dwButtons = d->keyboard ? 128 : 5;
  std::memcpy(GPtr(Arg(c, 1)), &caps, size);
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceInfo", DID8_GetDeviceInfo, 2) {
  InputDevice* d = Dev(c);
  DIDEVICEINSTANCEA* di = reinterpret_cast<DIDEVICEINSTANCEA*>(GPtr(Arg(c, 1)));
  uint32_t size = di->dwSize;
  bool ok = size == sizeof(DIDEVICEINSTANCEA) ||
            (d->version < 0x800 && size == kDIDEVICEINSTANCEA_DX3);
  if (!ok) return DIERR_INVALIDPARAM;
  FillInstance(di, d->keyboard, d->version, size);
  return DI_OK;
}

// (lpdf): the formats are recognised by their state size: c_dfDIKeyboard
// (256), c_dfDIMouse (16) and c_dfDIMouse2 (20).
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetDataFormat", DID8_SetDataFormat, 2) {
  InputDevice* d = Dev(c);
  if (d->acquired) return DIERR_ACQUIRED;
  uint32_t size = Rd32(Arg(c, 1) + 12);  // DIDATAFORMAT.dwDataSize
  bool ok = d->keyboard ? size == 256 : (size == 16 || size == 20);
  if (!ok) FatalF("dinput8: SetDataFormat with a %u-byte %s format", size,
                  d->keyboard ? "keyboard" : "mouse");
  d->data_size = size;
  return DI_OK;
}

// (hwnd, dwFlags)
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetCooperativeLevel", DID8_SetCooperativeLevel, 3) {
  InputDevice* d = Dev(c);
  d->hwnd = Arg(c, 1);
  d->coop = Arg(c, 2);
  return DI_OK;
}

// (rguidProp, pdiph): DIPROPDWORD for the buffer size and axis mode.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetProperty", DID8_SetProperty, 3) {
  InputDevice* d = Dev(c);
  uint32_t prop = Arg(c, 1), ph = Arg(c, 2);
  uint32_t value = Rd32(ph + 16);  // DIPROPDWORD.dwData
  switch (prop) {
    case DIPROP_BUFFERSIZE: {
      std::lock_guard<std::mutex> l(g_mu);
      d->buffer_size = value;
      while (d->buffer.size() > value) d->buffer.pop_front();
      return DI_OK;
    }
    case DIPROP_AXISMODE:
      if (d->keyboard) return DIERR_UNSUPPORTED;
      d->absolute = value == DIPROPAXISMODE_ABS;
      return DI_OK;
    default:
      RECOMP_TRACE("  SetProperty %X: unsupported\n", prop);
      return DIERR_UNSUPPORTED;
  }
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetProperty", DID8_GetProperty, 3) {
  InputDevice* d = Dev(c);
  uint32_t prop = Arg(c, 1), ph = Arg(c, 2);
  switch (prop) {
    case DIPROP_BUFFERSIZE:
      Wr32(ph + 16, d->buffer_size);
      return DI_OK;
    case DIPROP_AXISMODE:
      if (d->keyboard) return DIERR_UNSUPPORTED;
      Wr32(ph + 16, d->absolute ? 0 : 1);
      return DI_OK;
    default:
      RECOMP_TRACE("  GetProperty %X: unsupported\n", prop);
      return DIERR_UNSUPPORTED;
  }
}

// Acquiring starts from the current state: the keys that are down now,
// a mouse at rest (relative mode) or where it was left (absolute mode).
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Acquire", DID8_Acquire, 1) {
  InputDevice* d = Dev(c);
  if (!d->data_size) return DIERR_INVALIDPARAM;
  if (d->acquired) return DI_NOEFFECT;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (d->keyboard) {
      std::memset(d->keys, 0, sizeof d->keys);
      for (int vk = 1; vk < 256; vk++) {
        if (!g_key_down[vk]) continue;
        bool ext;
        uint8_t sc = ScanCodeForVk(uint8_t(vk), &ext);
        if (sc) d->keys[sc | (ext ? 0x80 : 0)] = 0x80;
      }
    }
    g_acquired.insert(d);
  }
  d->acquired = true;
  Grab(*d, true);
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Unacquire", DID8_Unacquire, 1) {
  InputDevice* d = Dev(c);
  if (!d->acquired) return DI_NOEFFECT;
  Unacquire(*d);
  return DI_OK;
}

// Devices with input of their own are not polled.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Poll", DID8_Poll, 1) {
  return Dev(c)->acquired ? DI_NOEFFECT : DIERR_NOTACQUIRED;
}

// (cbData, lpvData)
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceState", DID8_GetDeviceState, 3) {
  InputDevice* d = Dev(c);
  uint32_t size = Arg(c, 1), out = Arg(c, 2);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  if (size != d->data_size) return DIERR_INVALIDPARAM;
  std::lock_guard<std::mutex> l(g_mu);
  if (d->keyboard) {
    std::memcpy(GPtr(out), d->keys, 256);
    return DI_OK;
  }
  for (int a = 0; a < 3; a++) Wr32(out + 4 * uint32_t(a), uint32_t(d->axis[a]));
  if (!d->absolute) std::memset(d->axis, 0, sizeof d->axis);
  std::memcpy(GPtr(out + 12), d->buttons, size - 12);
  return DI_OK;
}

// (cbObjectData, rgdod, pdwInOut, dwFlags): the oldest buffered events;
// a null rgdod flushes them.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceData", DID8_GetDeviceData, 5) {
  InputDevice* d = Dev(c);
  uint32_t obj_size = Arg(c, 1), out = Arg(c, 2), inout = Arg(c, 3), flags = Arg(c, 4);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  if (!d->buffer_size) return DIERR_INVALIDPARAM;  // DIERR_NOTBUFFERED
  if (obj_size != 16 && obj_size != 20) return DIERR_INVALIDPARAM;
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t n = std::min<uint32_t>(Rd32(inout), uint32_t(d->buffer.size()));
  for (uint32_t i = 0; out && i < n; i++)
    std::memcpy(GPtr(out + i * obj_size), &d->buffer[i], obj_size);
  if (!(flags & DIGDD_PEEK)) d->buffer.erase(d->buffer.begin(), d->buffer.begin() + n);
  Wr32(inout, n);
  bool overflow = d->overflow;
  if (!(flags & DIGDD_PEEK)) d->overflow = false;
  return overflow ? DI_BUFFEROVERFLOW : DI_OK;
}

// ---- older interfaces: DirectInput 8 handlers under their names -------------------

struct Alias {
  const char* method;
  int nargs;
  Fn fn;
};

bool RegisterAliases(const char* iface, std::initializer_list<Alias> methods) {
  for (const Alias& a : methods)
    Registrar("dinput.dll", (std::string(iface) + "::" + a.method).c_str(), a.nargs, a.fn);
  return true;
}

[[maybe_unused]] const bool g_di2_aliases = RegisterAliases(
    "IDirectInput2A",
    {{"AddRef", 1, &hle_DI8_AddRef},
     {"Release", 1, &hle_DI8_Release},
     {"CreateDevice", 4, &hle_DI8_CreateDevice},
     {"GetDeviceStatus", 2, &hle_DI8_GetDeviceStatus}});

[[maybe_unused]] const bool g_did2_aliases = RegisterAliases(
    "IDirectInputDevice2A",
    {{"AddRef", 1, &hle_DID8_AddRef},
     {"Release", 1, &hle_DID8_Release},
     {"GetCapabilities", 2, &hle_DID8_GetCapabilities},
     {"GetProperty", 3, &hle_DID8_GetProperty},
     {"SetProperty", 3, &hle_DID8_SetProperty},
     {"Acquire", 1, &hle_DID8_Acquire},
     {"Unacquire", 1, &hle_DID8_Unacquire},
     {"GetDeviceState", 3, &hle_DID8_GetDeviceState},
     {"GetDeviceData", 5, &hle_DID8_GetDeviceData},
     {"SetDataFormat", 2, &hle_DID8_SetDataFormat},
     {"SetCooperativeLevel", 3, &hle_DID8_SetCooperativeLevel},
     {"GetDeviceInfo", 2, &hle_DID8_GetDeviceInfo},
     {"Poll", 1, &hle_DID8_Poll}});

}  // namespace
}  // namespace recomp::hle
