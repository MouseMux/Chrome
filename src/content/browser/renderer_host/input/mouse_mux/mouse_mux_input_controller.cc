// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"

#include <windows.h>
#include <fstream>
#include <tuple>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/utf_string_conversions.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/process/process_handle.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_control_server.h"
#include "components/input/input_router.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/browser_thread.h"
#include "content/browser/renderer_host/render_widget_host_view_aura.h"
#include "content/browser/renderer_host/render_widget_host_view_event_handler.h"
#include "components/input/native_web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/aura/env.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_utils.h"
#include "ui/events/types/scroll_types.h"
#include "ui/latency/latency_info.h"

namespace content {

namespace {

#ifdef MOUSEMUX_DEBUG

// Diagnostic log.  Opens and closes the file per call, which is slow, so it
// is for genuinely exceptional events only — never per input event.
void DiagLog(const std::string& message) {
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.LocalExplode(&exploded);
  std::ofstream file(MOUSEMUX_DIAG_LOG_PATH, std::ios::app);
  if (file.is_open()) {
    file << base::StringPrintf("[%02d:%02d:%02d.%03d|PID:%d] %s\n",
                                exploded.hour, exploded.minute,
                                exploded.second, exploded.millisecond,
                                static_cast<int>(base::GetCurrentProcId()),
                                message.c_str());
    file.close();
  }
}

#else

// Compiles every DiagLog() call away entirely, arguments included — an empty
// inline function would still evaluate the StringPrintf that builds the
// message, since the compiler cannot prove that allocation is side-effect
// free.  Declared as a function-like macro so multi-line call sites need no
// change.
#define DiagLog(message) ((void)0)

#endif  // MOUSEMUX_DEBUG

// Button bitmask values from MouseMux protocol.
constexpr int kLeftDown = 0x01;
constexpr int kLeftUp = 0x02;
constexpr int kRightDown = 0x04;
constexpr int kRightUp = 0x08;
constexpr int kMiddleDown = 0x10;
constexpr int kMiddleUp = 0x20;

}  // namespace

// static
MouseMuxInputController* MouseMuxInputController::GetInstance() {
  static base::NoDestructor<MouseMuxInputController> instance;
  return instance.get();
}

MouseMuxInputController::MouseMuxInputController() {
  // Unconditional, so the log always proves the trace build is live.  Without
  // it an empty file is ambiguous: no events, or logging broken?
  // Deliberately does not name the log path: with tracing off MMTRACE expands
  // to ((void)0) and MOUSEMUX_DEBUG_TRACE_PATH is undefined, so referencing it
  // here would rest on unused macro arguments never being expanded.
  MMTRACE("BOOT", "=== MouseMux trace build alive ===");
  LogDebug("MouseMuxInputController created");

  // Start the control server if --mousemux-control-port=PORT is specified.
  const base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  std::string port_str =
      cmd->GetSwitchValueASCII("mousemux-control-port");
  unsigned port_uint = 0;
  if (!port_str.empty() && base::StringToUint(port_str, &port_uint) &&
      port_uint > 0 && port_uint <= 65535) {
    control_server_ = std::make_unique<MouseMuxControlServer>(this);
    control_server_->Start(static_cast<uint16_t>(port_uint));
    DiagLog(base::StringPrintf("Control server started on port %u", port_uint));

    // Hold this browser's seat for as long as the process lives.  The port
    // identifies the seat, so nothing extra needs passing on the command line.
    ClaimOwnSeat(static_cast<int>(port_uint));
  }
}

MouseMuxInputController::~MouseMuxInputController() = default;

void MouseMuxInputController::SetDebugLogCallback(DebugLogCallback callback) {
  debug_log_callback_ = std::move(callback);
  LogDebug("Debug log callback registered");
}

void MouseMuxInputController::SetOwnershipChangedCallback(
    OwnershipChangedCallback callback) {
  ownership_changed_callback_ = std::move(callback);
}

void MouseMuxInputController::SetConnectionChangedCallback(
    ConnectionChangedCallback callback) {
  connection_changed_callback_ = std::move(callback);
}

void MouseMuxInputController::SetCaptureChangedCallback(
    CaptureChangedCallback callback) {
  capture_changed_callback_ = std::move(callback);
}

void MouseMuxInputController::SetKeyboardEventCallback(
    KeyboardEventCallback callback) {
  keyboard_event_callback_ = std::move(callback);
}

void MouseMuxInputController::SetNativeBlockingChangedCallback(
    NativeBlockingChangedCallback callback) {
  native_blocking_changed_callback_ = std::move(callback);
}

void MouseMuxInputController::SetMenuDismissCallback(
    MenuDismissCallback callback) {
  menu_dismiss_callback_ = std::move(callback);
}

MouseMuxInputController::DeviceState& MouseMuxInputController::StateFor(
    int mouse_hwid) {
  return device_state_[mouse_hwid];
}

int MouseMuxInputController::MouseHwidForKeyboard(int keyboard_hwid) const {
  auto it = keyboard_to_mouse_hwid_.find(keyboard_hwid);
  return it == keyboard_to_mouse_hwid_.end() ? -1 : it->second;
}

void MouseMuxInputController::ForgetViewEverywhere(
    RenderWidgetHostViewAura* view) {
  // A view can be the target of several devices at once, so this must clear
  // ALL of them rather than stopping at the first match.
  for (auto& [hwid, state] : device_state_) {
    if (state.drag_target_view == view) {
      state.drag_target_view = nullptr;
    }
    if (state.keyboard_target_view == view) {
      state.keyboard_target_view = nullptr;
    }
  }
}

void MouseMuxInputController::FlushPendingMotion() {
  if (!HasAnyOwner()) {
    return;
  }
  // One timer serves every device: flush each that has motion pending, rather
  // than only the owner's.
  const base::TimeTicks now = base::TimeTicks::Now();
  for (auto& [hwid, state] : device_state_) {
    if (!state.has_pending_motion) {
      continue;
    }
    state.has_pending_motion = false;
    state.last_motion_inject_time = now;
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseMove,
                              state.pending_motion_x, state.pending_motion_y,
                              state.button_state);
  }
}

void MouseMuxInputController::SetVisibilityChangedCallback(
    VisibilityChangedCallback callback) {
  visibility_changed_callback_ = std::move(callback);
}

void MouseMuxInputController::SetDialogVisible(bool visible) {
  if (visible == dialog_visible_) {
    return;
  }
  dialog_visible_ = visible;
  LogDebug(base::StringPrintf("SetDialogVisible(%s)",
                              visible ? "true" : "false"));
  if (visibility_changed_callback_) {
    visibility_changed_callback_.Run(visible);
  }
}

bool MouseMuxInputController::IsCaptured() const {
  // "Is capture in effect" rather than "is everyone captured": one indicator
  // cannot express a mixed state, and the per-owner truth is in GetOwners().
  for (int hwid : owners_) {
    auto it = device_state_.find(hwid);
    if (it != device_state_.end() && it->second.captured) {
      return true;
    }
  }
  return false;
}

bool MouseMuxInputController::CaptureOwnerHwid(int hwid) {
  if (!IsOwner(hwid) || !client_) {
    return false;
  }
  DeviceState& state = StateFor(hwid);
  if (state.captured) {
    return false;
  }
  client_->SendCaptureRequest(hwid);
  state.captured = true;
  is_captured_ = IsCaptured();
  LogDebug(base::StringPrintf("CaptureOwnerHwid: captured 0x%x", hwid));
  if (capture_changed_callback_) {
    capture_changed_callback_.Run(is_captured_);
  }
  return true;
}

bool MouseMuxInputController::ReleaseCaptureHwid(int hwid) {
  if (!IsOwner(hwid) || !client_) {
    return false;
  }
  DeviceState& state = StateFor(hwid);
  if (!state.captured) {
    return false;
  }
  client_->SendCaptureRelease(hwid);
  state.captured = false;
  is_captured_ = IsCaptured();
  LogDebug(base::StringPrintf("ReleaseCaptureHwid: released 0x%x", hwid));
  if (capture_changed_callback_) {
    capture_changed_callback_.Run(is_captured_);
  }
  return true;
}

namespace {

// ---------------------------------------------------------------------------
// Seats.
//
// A seat is an independent browser: its own process, profile and control port.
// This used to be launcher.exe's job, which existed for one reason — to hold a
// mutex for the lifetime of a browser it could not modify. We can modify the
// browser, so the browser holds its own seat, and the kernel releases it on any
// exit including a hard kill. That is exactly the property the launcher was
// built to obtain, obtained more directly.
//
// launcher.c records having tried this: "Having Chrome claim the mutex itself
// worked, but coupled launcher and browser versions and left the launcher
// guessing when a launch had failed." Both objections are about there being a
// launcher. With none, neither applies.
//
// The naming below deliberately MATCHES launcher.c, so an old launcher and a
// new browser still see each other's seats during an upgrade. (The hash is
// over the ASCII install path; a non-ASCII path would hash differently between
// the two, which is acceptable given the launcher is going away.)
// ---------------------------------------------------------------------------

constexpr int kSeatBasePort = 52000;
constexpr int kMaxSeats = 64;

// Held for the life of the process and deliberately never closed: process exit
// is what must release the seat, and the kernel does that even on a kill.
HANDLE g_seat_lock = nullptr;

// FNV-1a over the case-folded install path, matching launcher.c. Only needs to
// separate one install folder from another.
unsigned InstallPathHash() {
  base::FilePath dir;
  if (!base::PathService::Get(base::DIR_EXE, &dir)) {
    return 0;
  }
  // launcher.c keeps the trailing separator; match it or the hashes differ.
  std::string path = base::WideToUTF8(dir.value()) + "\\";
  unsigned hash = 2166136261u;
  for (char c : path) {
    hash ^= static_cast<unsigned char>(
        ::tolower(static_cast<unsigned char>(c)));
    hash *= 16777619u;
  }
  return hash;
}

std::string SeatLockName(int seat) {
  return base::StringPrintf("Local\\MouseMuxChromeSeat_%08x_%d",
                            InstallPathHash(), seat);
}

// Claims |seat|, or returns null if someone already holds it.  Creating rather
// than testing-then-creating: CreateMutex reports whether the object already
// existed, so claim and check are one uninterruptible step and two callers
// starting at the same instant cannot both win.
HANDLE ClaimSeatLock(int seat) {
  HANDLE lock = ::CreateMutexA(nullptr, FALSE, SeatLockName(seat).c_str());
  if (!lock) {
    return nullptr;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    // A handle comes back either way, so close rather than leak the one that
    // belongs to whoever got there first.
    ::CloseHandle(lock);
    return nullptr;
  }
  return lock;
}

#ifdef MOUSEMUX_KEYBOARD_LAYOUT

// ---------------------------------------------------------------------------
// Keyboard layout translation.
//
// The US-layout path maps a virtual key straight to a character through
// Chromium's own tables.  Letters and digits survive that, because layouts
// broadly agree on them, but punctuation does not, and a dedicated key like
// ABNT2's c-cedilla or any dead-key accent cannot be produced at all.
//
// ToUnicodeEx asks the ACTUAL layout what a key produces, given the modifiers
// held and the hardware scan code.  It is the same function the OS uses to
// turn WM_KEYDOWN into WM_CHAR, so it agrees with what the keyboard is
// painted with.
// ---------------------------------------------------------------------------

// Drains dead-key state out of the layout.
//
// ToUnicodeEx keeps composition state per THREAD, not per call: press an acute
// accent and the layout remembers it, so the next call composes against it.
// The browser has one thread, so one user's half-finished accent would
// silently combine with another user's next keystroke.  Every translation
// therefore starts from a known-empty buffer, and each device's own pending
// accent is re-fed deliberately.
void ClearLayoutDeadKeyState(HKL layout) {
  BYTE state[256] = {0};
  const UINT vk = VK_SPACE;
  const UINT sc = ::MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, layout);
  wchar_t buf[8];
  // Bounded: a layout that never stops reporting dead keys would otherwise
  // spin here forever.
  for (int i = 0; i < 8; ++i) {
    if (::ToUnicodeEx(vk, sc, state, buf, std::size(buf), 0, layout) >= 0) {
      break;
    }
  }
}

// Builds the 256-byte key state ToUnicodeEx wants from ONE device's held keys.
// Per device: a shared state would let one user's Shift capitalise another
// user's typing, which is exactly the class of bug multi-owner had to fix.
std::array<uint8_t, 256> BuildKeyState(const std::set<int>& held) {
  std::array<uint8_t, 256> state{};
  auto down = [&](int vk) { state[static_cast<size_t>(vk)] = 0x80; };

  const bool shift = held.count(VK_SHIFT) || held.count(VK_LSHIFT) ||
                     held.count(VK_RSHIFT);
  const bool ctrl = held.count(VK_CONTROL) || held.count(VK_LCONTROL) ||
                    held.count(VK_RCONTROL);
  const bool alt =
      held.count(VK_MENU) || held.count(VK_LMENU) || held.count(VK_RMENU);

  if (shift) {
    down(VK_SHIFT);
  }
  if (ctrl) {
    down(VK_CONTROL);
  }
  if (alt) {
    down(VK_MENU);
  }
  // AltGr is Ctrl+Alt on Windows, and it is how ABNT2 and most European
  // layouts reach their third level.  Right Alt alone must therefore present
  // as both.
  if (held.count(VK_RMENU)) {
    down(VK_CONTROL);
    down(VK_MENU);
  }
  // Caps Lock is a machine-wide toggle with no per-device equivalent, so it is
  // read from the system.  The low bit is the toggle, not the pressed bit.
  if (::GetKeyState(VK_CAPITAL) & 1) {
    state[VK_CAPITAL] = 0x01;
  }
  return state;
}

#endif  // MOUSEMUX_KEYBOARD_LAYOUT

// The toplevel window a view lives in, or null.  Locking uses this rather than
// the view itself because a cross-process navigation replaces the view while
// the window stays put.
gfx::AcceleratedWidget ToplevelWindowOf(RenderWidgetHostViewAura* view) {
  if (!view) {
    return gfx::kNullAcceleratedWidget;
  }
  aura::Window* native = view->GetNativeView();
  if (!native) {
    return gfx::kNullAcceleratedWidget;
  }
  aura::Window* toplevel = native->GetToplevelWindow();
  if (!toplevel || !toplevel->GetHost()) {
    return gfx::kNullAcceleratedWidget;
  }
  return toplevel->GetHost()->GetAcceleratedWidget();
}

// The title of the window a view lives in, for the operator to read.  Empty
// when the view has no window yet, which reads better in the dialog than a
// placeholder would.
std::u16string ToplevelTitleOf(RenderWidgetHostViewAura* view) {
  if (!view) {
    return std::u16string();
  }
  aura::Window* native = view->GetNativeView();
  if (!native) {
    return std::u16string();
  }
  aura::Window* toplevel = native->GetToplevelWindow();
  return toplevel ? toplevel->GetTitle() : std::u16string();
}

}  // namespace

void MouseMuxInputController::ClaimOwnSeat(int control_port) {
  if (g_seat_lock) {
    return;  // Already holding one.
  }
  const int seat = control_port - kSeatBasePort;
  if (seat < 1 || seat > kMaxSeats) {
    // A port outside the seat range means this browser was started by hand or
    // by a config that does not use seats.  Nothing to claim, and nothing
    // wrong — it simply will not appear as an occupied seat.
    return;
  }
  g_seat_lock = ClaimSeatLock(seat);
  DiagLog(base::StringPrintf("Seat %d: %s", seat,
                             g_seat_lock ? "claimed" : "already taken"));
}

bool MouseMuxInputController::LaunchAdditionalSeat() {
  base::FilePath exe;
  base::FilePath dir;
  if (!base::PathService::Get(base::FILE_EXE, &exe) ||
      !base::PathService::Get(base::DIR_EXE, &dir)) {
    DiagLog("LaunchAdditionalSeat: cannot resolve executable path");
    return false;
  }

  // Find the lowest free seat.  The probe releases immediately, so there is a
  // window between choosing a seat and the new browser claiming it in which a
  // second launch could choose the same one.  The consequence is benign: two
  // browsers with the same --user-data-dir means ProcessSingleton hands the
  // second one's command line to the first and it exits, so a click appears to
  // do nothing.  No profile is corrupted, which is what matters.
  int seat = 0;
  for (int candidate = 1; candidate <= kMaxSeats; ++candidate) {
    HANDLE probe = ClaimSeatLock(candidate);
    if (probe) {
      ::CloseHandle(probe);
      seat = candidate;
      break;
    }
  }
  if (seat == 0) {
    DiagLog("LaunchAdditionalSeat: all seats occupied");
    return false;
  }

  const int port = kSeatBasePort + seat;
  const base::FilePath profile =
      dir.Append(base::UTF8ToWide(base::StringPrintf("user-data-%d", seat)));
  // Cascade, matching launcher.c, so seats do not land exactly on top of one
  // another.
  const int offset = 40 + (seat - 1) * 60;

  base::CommandLine cmd(exe);
  cmd.AppendSwitchASCII("enable-features", "MouseMuxIntegration");
  cmd.AppendSwitchASCII("mousemux-control-port", base::NumberToString(port));
  cmd.AppendSwitchPath("user-data-dir", profile);
  cmd.AppendSwitch("no-first-run");
  // Without this an elevated launch makes Chrome relaunch itself and let the
  // original go, which muddies which process owns the seat.
  cmd.AppendSwitch("do-not-de-elevate");
  cmd.AppendSwitchASCII("window-position",
                        base::StringPrintf("%d,%d", offset, offset));
  cmd.AppendSwitchASCII("window-size", "1200,800");

  base::LaunchOptions options;
  if (!base::LaunchProcess(cmd, options).IsValid()) {
    DiagLog(base::StringPrintf("LaunchAdditionalSeat: seat %d failed to start",
                               seat));
    return false;
  }
  DiagLog(base::StringPrintf("Launched seat %d on port %d", seat, port));
  return true;
}

void MouseMuxInputController::SetHardLock(bool enabled) {
  if (hard_lock_ == enabled) {
    return;
  }
  hard_lock_ = enabled;
  if (!enabled) {
    // Forget assignments on the way out, so turning it back on re-locks people
    // to where they ARE rather than to a window they left long ago.
    for (auto& [hwid, state] : device_state_) {
      state.locked_window = gfx::kNullAcceleratedWidget;
    }
  }
  LogDebug(base::StringPrintf("SetHardLock(%s)", enabled ? "true" : "false"));
}

bool MouseMuxInputController::ShouldSuppressBlur(
    RenderWidgetHostViewAura* view) const {
  if (!view || !registered_views_.count(view)) {
    return false;
  }
  // Only while someone is captured — see the header for why capture is the
  // right condition and OS focus is not.
  return IsCaptured();
}

std::vector<MouseMuxInputController::OwnerInfo>
MouseMuxInputController::GetOwners() const {
  std::vector<OwnerInfo> out;
  // owners_ is a std::set, so iteration is already hwid-ordered and rows keep
  // their position between refreshes.
  for (int hwid : owners_) {
    OwnerInfo info;
    info.hwid = hwid;
    info.is_primary = (hwid == owner_hwid_);

    auto user_it = user_info_.find(hwid);
    if (user_it != user_info_.end()) {
      info.name = user_it->second.name;
      info.keyboard_hwid = user_it->second.hwid_keyboard;
    }
    info.keyboard_typed = info.keyboard_hwid != 0 &&
                          keyboards_seen_.count(info.keyboard_hwid) > 0;

    auto state_it = device_state_.find(hwid);
    if (state_it != device_state_.end()) {
      info.captured = state_it->second.captured;

      // Report the window this user is TYPING in — the keyboard target — which
      // is what "where is this user working" means to an operator.
      RenderWidgetHostViewAura* view = state_it->second.keyboard_target_view;
      if (view && registered_views_.count(view)) {
        if (aura::Window* native = view->GetNativeView()) {
          if (aura::Window* toplevel = native->GetToplevelWindow()) {
            info.window_title = toplevel->GetTitle();
            info.has_window = true;
            if (aura::WindowTreeHost* host = toplevel->GetHost()) {
              info.window = host->GetAcceleratedWidget();
            }
          }
        }
      }
    }
    out.push_back(std::move(info));
  }
  return out;
}

RenderWidgetHostViewAura* MouseMuxInputController::WebViewInWindow(
    gfx::AcceleratedWidget window) const {
  if (!window) {
    return nullptr;
  }
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (view && ToplevelWindowOf(view) == window) {
      return view;
    }
  }
  return nullptr;
}

std::vector<MouseMuxInputController::KeyRoute>
MouseMuxInputController::GetKeyRoutes() const {
  return key_routes_;
}

void MouseMuxInputController::RecordKeyRoute(int keyboard_hwid,
                                             int mouse_hwid,
                                             const std::u16string& title,
                                             bool dropped) {
  // Collapse repeats.  A sentence of typing is ONE routing decision taken a
  // hundred times, and a list that shows it a hundred times shows nothing —
  // the count is the useful part, not the repetition.
  if (!key_routes_.empty()) {
    KeyRoute& last = key_routes_.back();
    if (last.keyboard_hwid == keyboard_hwid && last.mouse_hwid == mouse_hwid &&
        last.dropped == dropped && last.window_title == title) {
      ++last.count;
      return;
    }
  }
  key_routes_.push_back({keyboard_hwid, mouse_hwid, title, dropped, 1});
  if (key_routes_.size() > kMaxKeyRoutes) {
    key_routes_.erase(key_routes_.begin());
  }
}

bool MouseMuxInputController::CaptureOwner() {
  if (!HasAnyOwner()) {
    LogDebug("CaptureOwner: No owner to capture");
    return false;
  }
  if (is_captured_) {
    LogDebug("CaptureOwner: Already captured");
    return false;
  }
  if (client_) {
    // Capture EVERY owner.  Capture is what stops a device producing native
    // Windows input, and native input is what forces one window to be focused
    // at a time — so a single uncaptured owner would blur everyone else's
    // window on every click.  This is the "capture all" control; the dialog's
    // per-owner control is CaptureOwnerHwid().
    for (int hwid : owners_) {
      if (!StateFor(hwid).captured) {
        client_->SendCaptureRequest(hwid);
        StateFor(hwid).captured = true;
      }
    }
    is_captured_ = true;
    // Capture tells the server to stop sending native input for the captured
    // device.  SDK WebSocket events keep flowing as before, so we do NOT
    // change native-input blocking here.
    LogDebug(base::StringPrintf("CaptureOwner: Captured hwid=0x%x", owner_hwid_));
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(true);
    }
    return true;
  }
  return false;
}

bool MouseMuxInputController::ReleaseCapture() {
  if (!is_captured_) {
    LogDebug("ReleaseCapture: Not captured");
    return false;
  }
  if (!HasAnyOwner()) {
    // Edge case: owner was released but capture state wasn't cleared.
    is_captured_ = false;
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(false);
    }
    return false;
  }
  if (client_) {
    for (int hwid : owners_) {
      if (StateFor(hwid).captured) {
        client_->SendCaptureRelease(hwid);
        StateFor(hwid).captured = false;
      }
    }
    is_captured_ = false;
    // Release tells the server to resume native input for the device.
    // SDK WebSocket events keep flowing throughout, so no blocking changes.
    LogDebug(base::StringPrintf("ReleaseCapture: Released hwid=0x%x", owner_hwid_));
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(false);
    }
    return true;
  }
  return false;
}

void MouseMuxInputController::AddOwner(int hwid) {
  if (hwid == -1 || IsOwner(hwid)) {
    return;
  }
#ifndef MOUSEMUX_MULTI_OWNER
  // Single-owner: claiming replaces whoever held it.
  owners_.clear();
#endif
  owners_.insert(hwid);
  if (owner_hwid_ == -1) {
    owner_hwid_ = hwid;
  }
  // Clear only the incoming owner's button state: another device's held
  // buttons are none of this owner's business.
  StateFor(hwid).button_state = 0;
  LogDebug(base::StringPrintf("AddOwner: hwid=0x%x (owners now %zu, primary 0x%x)",
                              hwid, owners_.size(), owner_hwid_));
  NotifyOwnershipChanged();
}

void MouseMuxInputController::RemoveOwner(int hwid) {
  if (!IsOwner(hwid)) {
    return;
  }
  owners_.erase(hwid);
  StateFor(hwid).button_state = 0;
  // The primary leaving promotes someone else rather than reporting "no
  // owner" while other users are still working.
  if (owner_hwid_ == hwid) {
    owner_hwid_ = owners_.empty() ? -1 : *owners_.begin();
  }
  LogDebug(base::StringPrintf(
      "RemoveOwner: hwid=0x%x (owners now %zu, primary 0x%x)", hwid,
      owners_.size(), owner_hwid_));
  NotifyOwnershipChanged();
}

void MouseMuxInputController::SetOwner(int hwid) {
  if (hwid == owner_hwid_) return;
  LogDebug(base::StringPrintf("SetOwner: hwid=0x%x (was 0x%x)", hwid, owner_hwid_));
  AddOwner(hwid);
  // Explicit external selection (dialog, control server) means THIS device,
  // so make it primary even when it had already claimed.
  owner_hwid_ = hwid;
}

bool MouseMuxInputController::SetOwnerByName(const std::string& name) {
  for (const auto& [hwid, info] : user_info_) {
    if (info.name == name) {
      SetOwner(hwid);
      return true;
    }
  }
  LogDebug("SetOwnerByName: user '" + name + "' not found in user_info");
  return false;
}

void MouseMuxInputController::ReleaseOwnerHwid(int hwid) {
  if (!IsOwner(hwid)) {
    return;
  }
  // Give the device back to Windows before dropping it: an owner removed
  // while still captured would leave that mouse held by the server with
  // nothing driving it.
  ReleaseCaptureHwid(hwid);
  RemoveOwner(hwid);
}

void MouseMuxInputController::ReleaseOwnership() {
  LogDebug(base::StringPrintf("ReleaseOwnership: hwid=0x%x", owner_hwid_));

  // Release capture first if captured.
  if (is_captured_) {
    ReleaseCapture();
  }

  // Releases EVERY owner: this is the dialog's "release" and the control
  // server's owner:null, both of which mean "hand Chrome back", not "drop one
  // of several users".  Per-owner release is RemoveOwner().
  for (int hwid : owners_) {
    StateFor(hwid).button_state = 0;
  }
  owners_.clear();
  owner_hwid_ = -1;
  NotifyOwnershipChanged();
}

std::string MouseMuxInputController::GetOwnerName() const {
  if (owner_hwid_ == -1) {
    return "";
  }
  auto it = user_info_.find(owner_hwid_);
  if (it != user_info_.end()) {
    return it->second.name;
  }
  return "";
}

void MouseMuxInputController::NotifyOwnershipChanged() {
  if (ownership_changed_callback_) {
    std::string name = GetOwnerName();
    ownership_changed_callback_.Run(owner_hwid_, name);
  }
}

void MouseMuxInputController::LogDebug(const std::string& message) {
  if (debug_log_callback_) {
    debug_log_callback_.Run(
        base::StringPrintf("[Ctrl|PID:%d] %s",
                           static_cast<int>(base::GetCurrentProcId()),
                           message.c_str()));
  }
}

void MouseMuxInputController::SetNativeInputBlocked(bool blocked) {
  native_input_blocked_ = blocked;

  if (native_blocking_changed_callback_) {
    native_blocking_changed_callback_.Run(blocked);
  }

#ifdef MOUSEMUX_NATIVE_BLOCK
  // Defined in desktop_window_tree_host_win.cc inside namespace content —
  // blocks native mouse button messages at the views/aura layer.
  extern bool g_mousemux_native_input_blocked;
  g_mousemux_native_input_blocked = blocked;
#endif

  LogDebug(base::StringPrintf("SetNativeInputBlocked(%s) - %zu views registered",
                               blocked ? "true" : "false",
                               registered_views_.size()));

  // Update all registered views - block both mouse and keyboard.
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (view && view->event_handler()) {
      view->event_handler()->SetNativeMouseInputBlocked(blocked);
      view->event_handler()->SetNativeKeyboardInputBlocked(blocked);
#ifdef MOUSEMUX_DEBUG
      LogDebug("  - Updated view event handler (mouse + keyboard)");
#endif
    }
  }
}

void MouseMuxInputController::SetMouseMuxEnabled(bool enabled) {
  LogDebug(base::StringPrintf("SetMouseMuxEnabled(%s)", enabled ? "true" : "false"));

  // Record intent before touching the client: OnConnectionStateChanged(false)
  // consults this to tell a fault apart from a deliberate disconnect, and
  // Disconnect() below will trigger exactly that callback.
  MMTRACE("CONN/SetEnabled", "enabled=%d (client=%s)", enabled ? 1 : 0,
          client_ ? "exists" : "none");

  should_be_connected_ = enabled;
  reconnect_attempts_ = 0;
  reconnect_timer_.Stop();

  if (enabled) {
    if (!client_) {
      LogDebug("Creating new MouseMuxClient...");
      client_ = std::make_unique<MouseMuxClient>();
      // Pass our debug callback to the client.
      if (debug_log_callback_) {
        client_->SetDebugLogCallback(debug_log_callback_);
        LogDebug("Debug callback passed to client");
      }
      // No matching RemoveObserver: client_ is created once and never reset
      // (Disconnect() only closes the pipe), and the controller is a
      // NoDestructor singleton — so the observer never outlives the list.
      client_->AddObserver(this);
      LogDebug("MouseMuxClient created and observer added");
    }
    LogDebug("Calling client_->Connect()...");
    client_->Connect();
  } else {
    if (client_) {
      LogDebug("Calling client_->Disconnect()...");
      client_->Disconnect();
    }
  }
}

bool MouseMuxInputController::IsMouseMuxEnabled() const {
  return client_ && client_->IsConnected();
}

void MouseMuxInputController::RegisterView(RenderWidgetHostViewAura* view) {
  if (!view)
    return;

  registered_views_.insert(view);

  // Don't call GetViewBounds() here - view may not be fully initialized.
  // Just log the count.
  LogDebug(base::StringPrintf("RegisterView: now %zu views", registered_views_.size()));

  // Apply current blocking state for both mouse and keyboard.
  if (view->event_handler()) {
    view->event_handler()->SetNativeMouseInputBlocked(native_input_blocked_);
    view->event_handler()->SetNativeKeyboardInputBlocked(native_input_blocked_);
  }
}

void MouseMuxInputController::UnregisterView(RenderWidgetHostViewAura* view) {
  registered_views_.erase(view);
  // Clear pointers that reference the unregistered view to prevent
  // dangling pointer access.
  ForgetViewEverywhere(view);
  if (pending_view_ == view) {
    pending_view_ = nullptr;
  }
  LogDebug(base::StringPrintf("UnregisterView: now %zu views", registered_views_.size()));
}

void MouseMuxInputController::OnMouseMotion(int hwid, float x, float y) {
  // MouseMuxClient dispatches observers on its own sequence, which is the UI
  // thread — it asserts that itself with DCHECK_CALLED_ON_VALID_SEQUENCE.
  // Assert the invariant rather than silently reposting: a repost here would
  // reorder input events relative to each other, which is far worse than a
  // loud failure.
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Update position tracking for this hwid.
  user_positions_[hwid] = {x, y};
  motion_count_++;

  // If no owner yet, don't inject motion events.
  if (!HasAnyOwner()) {
    return;
  }

  // Only process events from the owner.
  if (!IsOwner(hwid)) {
    return;
  }

  // Throttle motion injection to max 60fps (16ms between events).
  // This prevents flooding the UI thread with motion events.
  base::TimeTicks now = base::TimeTicks::Now();
  constexpr base::TimeDelta kMinMotionInterval = base::Milliseconds(16);
  DeviceState& state = StateFor(hwid);
  // Throttled PER DEVICE: a shared clock would let a fast-moving mouse
  // starve a slow one, and a shared pending slot would mix their positions.
  if (now - state.last_motion_inject_time < kMinMotionInterval) {
    // Store position for next injection, but don't inject now.
    state.pending_motion_x = x;
    state.pending_motion_y = y;
    state.has_pending_motion = true;
    // Deliver it anyway if no further motion arrives — otherwise the cursor
    // comes to rest at a position the renderer never saw.
    motion_flush_timer_.Start(
        FROM_HERE, kMinMotionInterval,
        base::BindOnce(&MouseMuxInputController::FlushPendingMotion,
                       base::Unretained(this)));
    return;
  }

  // Inject motion event.
  state.last_motion_inject_time = now;
  state.has_pending_motion = false;
  InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseMove, x, y,
                            state.button_state);
}

#ifdef MOUSEMUX_PEN_TOUCH_INJECT
void MouseMuxInputController::OnPenMotion(int hwid,
                                          float x,
                                          float y,
                                          int pressure,
                                          int tilt_x,
                                          int tilt_y,
                                          int rotation) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  MMTRACE("CTRL/PenMotion", "hwid=%d pos=(%.0f,%.0f) press=%d tilt=(%d,%d) "
                            "rot=%d owner=%d views=%zu",
          hwid, x, y, pressure, tilt_x, tilt_y, rotation, owner_hwid_,
          registered_views_.size());

  // Record metadata even for non-owner devices, so it is already correct if
  // this hwid later claims ownership.
  PenState& pen = pen_state_[hwid];
  pen.pressure = pressure;
  pen.tilt_x = tilt_x;
  pen.tilt_y = tilt_y;
  pen.rotation = rotation;

  // From here this mirrors OnMouseMotion exactly — the pen metadata above is
  // applied later, at injection time, by ApplyPointerProperties.
  user_positions_[hwid] = {x, y};
  motion_count_++;

  // If no owner yet, don't inject motion events.
  if (!HasAnyOwner()) {
    MMTRACE("CTRL/PenMotion", "DROPPED: no owner claimed yet (hwid=%d)",
            hwid);
    return;
  }

  // Only process events from the owner.
  if (!IsOwner(hwid)) {
    MMTRACE("CTRL/PenMotion", "DROPPED: hwid=%d != owner=%d", hwid,
            owner_hwid_);
    return;
  }

  // Pens sample far faster than mice — 120-240Hz is typical — and pressure
  // curves lose their shape when decimated to 60fps, so allow roughly double
  // the mouse rate.  Still throttled, because the UI thread does the
  // injection and an unbounded stream would flood it.
  base::TimeTicks now = base::TimeTicks::Now();
  constexpr base::TimeDelta kMinPenInterval = base::Milliseconds(8);
  DeviceState& state = StateFor(hwid);
  if (now - state.last_motion_inject_time < kMinPenInterval) {
    state.pending_motion_x = x;
    state.pending_motion_y = y;
    state.has_pending_motion = true;
    motion_flush_timer_.Start(
        FROM_HERE, kMinPenInterval,
        base::BindOnce(&MouseMuxInputController::FlushPendingMotion,
                       base::Unretained(this)));
    return;
  }

  state.last_motion_inject_time = now;
  state.has_pending_motion = false;
  InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseMove, x, y,
                            state.button_state);
}
#endif

void MouseMuxInputController::OnMouseButton(int hwid,
                                            float x,
                                            float y,
                                            int data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Flush THIS device's pending motion before its button event, so the click
  // lands at the position the cursor actually reached.  Other devices' pending
  // motion is left alone — flushing it here would inject their movement at an
  // arbitrary moment driven by someone else's click.
  {
    DeviceState& state = StateFor(hwid);
    if (state.has_pending_motion) {
      state.has_pending_motion = false;
      InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseMove,
                                state.pending_motion_x, state.pending_motion_y,
                                state.button_state);
    }
  }

#ifdef MOUSEMUX_DEBUG
  // Log all button events with full context including user name.
  std::string btn_user = "?";
  auto btn_ui = user_info_.find(hwid);
  if (btn_ui != user_info_.end()) {
    btn_user = btn_ui->second.name;
  }
  LogDebug(base::StringPrintf(
      "BTN: user=%s hwid=0x%x data=0x%x pos=(%.0f,%.0f) owner=0x%x views=%zu",
      btn_user.c_str(), hwid, data, x, y, owner_hwid_,
      registered_views_.size()));
#endif

  // Update position tracking.
  user_positions_[hwid] = {x, y};

  // Check if this is a click that should claim ownership.
  // Only left-down claims ownership.
  //
  // With MULTI_OWNER, ANY unclaimed device claims by clicking, so each user
  // joins by clicking their own window.  Without it, only the very first
  // device can claim, exactly as before.
#ifdef MOUSEMUX_MULTI_OWNER
  const bool may_claim = !IsOwner(hwid);
#else
  const bool may_claim = !HasAnyOwner();
#endif
  if (may_claim && (data & kLeftDown)) {
    MMTRACE("CTRL/Claim", "attempting claim hwid=%d at (%.0f,%.0f) views=%zu",
            hwid, x, y, registered_views_.size());
    if (registered_views_.empty()) {
      MMTRACE("CTRL/Claim", "DROPPED: no views registered");
      LogDebug("BTN IGNORED: No views registered - cannot claim ownership");
      return;
    }

    // Check if cursor is over Chrome using hit-test.
    RenderWidgetHostViewAura* hit_view = FindViewAtPoint(x, y);
    MMTRACE("CTRL/Claim", "hit-test at (%.0f,%.0f) -> %s", x, y,
            hit_view ? "view found" : "NO VIEW (will use fallback)");
    if (hit_view) {
      AddOwner(hwid);
      LogDebug(base::StringPrintf("OWNER SET via hit-test: hwid=0x%x", hwid));
    } else {
      // Hit-test failed, but we have views. Log why and try alternative.
      LogDebug("Hit-test failed. Trying coordinate-agnostic ownership claim...");

      // Alternative: Check if we should claim ownership anyway.
      // If the user has enabled MouseMux and is clicking, they probably want it to work.
      // Claim ownership and use the first view.
      AddOwner(hwid);
      LogDebug(base::StringPrintf(
          "OWNER SET via fallback (hit-test failed but views exist): hwid=0x%x", hwid));
    }
  }

  // If no owner, ignore.
  if (!HasAnyOwner()) {
    MMTRACE("CTRL/Button", "DROPPED: no owner set (hwid=%d data=0x%x)", hwid,
            data);
#ifdef MOUSEMUX_DEBUG
    LogDebug("BTN IGNORED: No owner set");
#endif
    return;
  }

  // Only process events from the owner.
  if (!IsOwner(hwid)) {
    MMTRACE("CTRL/Button", "DROPPED: hwid=%d != owner=%d (data=0x%x)", hwid,
            owner_hwid_, data);
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf("BTN IGNORED: hwid=0x%x is not owner=0x%x", hwid, owner_hwid_));
#endif
    return;
  }

  MMTRACE("CTRL/Button", "ACCEPTED hwid=%d data=0x%x pos=(%.0f,%.0f)", hwid,
          data, x, y);

  // Process button state changes from owner.
  if (data & kLeftDown) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting LEFT DOWN");
#endif
    StateFor(hwid).button_state |= blink::WebMouseEvent::kLeftButtonDown;
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kLeftUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting LEFT UP");
#endif
    StateFor(hwid).button_state &= ~blink::WebMouseEvent::kLeftButtonDown;
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kRightDown) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting RIGHT DOWN");
#endif
    StateFor(hwid).button_state |= blink::WebMouseEvent::kRightButtonDown;
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kRightUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting RIGHT UP");
#endif
    StateFor(hwid).button_state &= ~blink::WebMouseEvent::kRightButtonDown;
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kMiddleDown) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting MIDDLE DOWN");
#endif
    StateFor(hwid).button_state |= blink::WebMouseEvent::kMiddleButtonDown;
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
  if (data & kMiddleUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting MIDDLE UP");
#endif
    StateFor(hwid).button_state &= ~blink::WebMouseEvent::kMiddleButtonDown;
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
}

void MouseMuxInputController::OnMouseWheel(int hwid, float x, float y, int delta, bool horizontal) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Update position tracking.
  user_positions_[hwid] = {x, y};

  // If no owner, ignore wheel events.
  if (!HasAnyOwner()) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("WHEEL IGNORED: No owner set");
#endif
    return;
  }

  // Only process events from the owner.
  if (!IsOwner(hwid)) {
    return;
  }

#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf("WHEEL: delta=%d horizontal=%d pos=(%.0f,%.0f)",
                               delta, horizontal ? 1 : 0, x, y));
#endif

  // Find view and inject wheel event.
  RenderWidgetHostViewAura* view = FindViewAtPoint(x, y);
  if (!view && !registered_views_.empty()) {
    view = *registered_views_.begin();
  }
  if (view) {
    InjectWheelEvent(hwid, view, x, y, delta, horizontal);
  } else {
    LogDebug("WHEEL FAILED: No view available");
  }
}

void MouseMuxInputController::OnConnectionStateChanged(bool connected) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  MMTRACE("CONN/State", "connected=%d wanted=%d views=%zu owner=%d",
          connected ? 1 : 0, should_be_connected_ ? 1 : 0,
          registered_views_.size(), owner_hwid_);

  LogDebug(base::StringPrintf("OnConnectionStateChanged: %s (views=%zu)",
                               connected ? "CONNECTED" : "DISCONNECTED",
                               registered_views_.size()));

  // Notify dialog of connection state change.
  if (connection_changed_callback_) {
    connection_changed_callback_.Run(connected);
  }

  if (connected) {
    // Back off from scratch next time — this connection proved the server is
    // reachable, so a later drop should retry quickly rather than resuming a
    // long delay from a previous outage.
    reconnect_attempts_ = 0;
    reconnect_timer_.Stop();

    // Reset owner when reconnecting.
    owners_.clear();
    owner_hwid_ = -1;
    // Drops every device's buttons, held keys, targets and pending motion:
    // after a reconnect none of it can be trusted to match the hardware.
    device_state_.clear();
    is_captured_ = false;
    user_positions_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
#ifdef MOUSEMUX_PEN_TOUCH_INJECT
    pen_state_.clear();
#endif
    motion_count_ = 0;
    LogDebug("Reset owner, button state, capture, keyboard, and user tracking on connect");
    NotifyOwnershipChanged();
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(false);
    }

    // Request user list from server.
    if (client_) {
      LogDebug("Requesting user list...");
      client_->RequestUserList();
    }
  } else {
    // Clear all state on disconnect.
    owners_.clear();
    owner_hwid_ = -1;
    device_state_.clear();
    is_captured_ = false;
    user_positions_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
#ifdef MOUSEMUX_PEN_TOUCH_INJECT
    pen_state_.clear();
#endif
    pending_view_ = nullptr;
    // Unblock native input so the user isn't stuck with blocked input.
    if (native_input_blocked_) {
      SetNativeInputBlocked(false);
    }
    NotifyOwnershipChanged();
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(false);
    }

    // Only chase a connection that was actually wanted.  A drop after the
    // user switched MouseMux off is the expected outcome, not a fault.
    if (should_be_connected_) {
      ScheduleReconnect();
    }
  }
}

void MouseMuxInputController::ScheduleReconnect() {
  if (!should_be_connected_ || !client_) {
    return;
  }

  // 1, 2, 4, 8, 16 then 30 seconds.  Capped so an outage that lasts hours
  // still recovers promptly once the server returns, rather than sitting in
  // an ever-growing delay.
  constexpr int kMaxBackoffSeconds = 30;
  // Shift 5 gives 32s, which the cap then trims to 30.  Stopping at 4 would
  // plateau at 16s and make kMaxBackoffSeconds unreachable — measured against
  // a dead server, the sixth retry came at 16s instead of 30s.
  constexpr int kMaxShift = 5;
  const int shift =
      reconnect_attempts_ < kMaxShift ? reconnect_attempts_ : kMaxShift;
  int delay_seconds = 1 << shift;
  if (delay_seconds > kMaxBackoffSeconds) {
    delay_seconds = kMaxBackoffSeconds;
  }
  reconnect_attempts_++;

  LogDebug(base::StringPrintf("Reconnect attempt %d in %ds",
                              reconnect_attempts_, delay_seconds));

  // Unretained is safe: the controller is a NoDestructor singleton, and the
  // timer is a member so it stops if this object ever does go away.
  reconnect_timer_.Start(
      FROM_HERE, base::Seconds(delay_seconds),
      base::BindOnce(&MouseMuxInputController::AttemptReconnect,
                     base::Unretained(this)));
}

void MouseMuxInputController::AttemptReconnect() {
  // Intent can change while the timer is pending — the user may have switched
  // MouseMux off, or the control server may have connected us already.
  if (!should_be_connected_ || !client_ || client_->IsConnected()) {
    return;
  }
  LogDebug("Reconnecting to MouseMux server...");
  client_->Connect();
}

void MouseMuxInputController::OnUserList(
    const std::vector<MouseMuxClient::UserInfo>& users) {
#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf("UserList: %zu users", users.size()));
#endif
  user_info_.clear();
  keyboard_to_mouse_hwid_.clear();
  for (const auto& user : users) {
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf("  User: id=%d name=%s mouse=0x%x kb=0x%x",
                                 user.user_id, user.name.c_str(),
                                 user.hwid_mouse, user.hwid_keyboard));
#endif
    user_info_[user.hwid_mouse] = user;
    if (user.hwid_keyboard != 0) {
      keyboard_to_mouse_hwid_[user.hwid_keyboard] = user.hwid_mouse;
    }
  }
  // If we have an owner, notify again in case we now have a name.
  if (owner_hwid_ != -1) {
    NotifyOwnershipChanged();
  }
}

void MouseMuxInputController::OnUserCreated(
    const MouseMuxClient::UserInfo& user) {
#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf("UserCreated: id=%d mouse=0x%x kb=0x%x name=%s",
                               user.user_id, user.hwid_mouse,
                               user.hwid_keyboard, user.name.c_str()));
#endif
  user_info_[user.hwid_mouse] = user;
  if (user.hwid_keyboard != 0) {
    keyboard_to_mouse_hwid_[user.hwid_keyboard] = user.hwid_mouse;
  }
  // If this user is already the owner, notify to update the name.
  if (user.hwid_mouse == owner_hwid_) {
    NotifyOwnershipChanged();
  }
}

void MouseMuxInputController::OnUserDisposed(int hwid_mouse, int hwid_keyboard) {
  LogDebug(base::StringPrintf("UserDisposed: mouse=0x%x kb=0x%x",
                               hwid_mouse, hwid_keyboard));

  // A disposed device's state goes with it, whether or not it was the owner —
  // the hardware is gone, so its held buttons and keys are stale, and leaving
  // them would resurrect on an hwid reuse.
  device_state_.erase(hwid_mouse);

  // Drop just this device's ownership.  Unplugging one user's mouse must not
  // evict the others; RemoveOwner promotes a new primary if this was it.
  if (IsOwner(hwid_mouse)) {
    LogDebug("OWNER DISPOSED - removing from owners");
    RemoveOwner(hwid_mouse);
  }

  // Remove from position tracking.
  user_positions_.erase(hwid_mouse);
  user_positions_.erase(hwid_keyboard);

  // Remove from user info cache.
  user_info_.erase(hwid_mouse);

  // Remove from keyboard mapping.
  keyboard_to_mouse_hwid_.erase(hwid_keyboard);
  keyboards_seen_.erase(hwid_keyboard);
}

void MouseMuxInputController::OnKeyboardKey(int hwid, int vkey, int message,
                                            int scan, int flags) {
#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf(
      "SDK KEY IN: hwid=0x%x vkey=0x%x msg=0x%x scan=%d flags=0x%x",
      hwid, vkey, message, scan, flags));
#endif

  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // If no owner, ignore keyboard events.
  if (!HasAnyOwner()) {
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "KEY SKIP: no owner yet, kb_hwid=0x%x vkey=0x%x", hwid, vkey));
#endif
    return;
  }

  // Resolve which device PAIR this keystroke belongs to.  All per-device state
  // is keyed by mouse hwid, so the keyboard hwid has to be translated first.
  //
  // With one owner this was unnecessary and the code deliberately accepted any
  // keyboard: MouseMux's own hook guarantees that during capture only the
  // captured user's keys arrive, so there was exactly one candidate. That
  // stops being true the moment several users are captured at once — every
  // keyboard then delivers, and a keystroke that cannot be attributed would go
  // to somebody else's window.
  //
  // Unknown pairing falls back to the owner, preserving the old behaviour for
  // a single user and for keyboards the user list has not described yet.
  int pair_hwid = MouseHwidForKeyboard(hwid);
  bool deliverable = true;
  if (pair_hwid == -1) {
    // No user in the SDK's list holds this keyboard.  In practice that means
    // it was never assigned to a user in MouseMux: it is detected, it types,
    // and it belongs to nobody.  No routing rule here can invent the pairing.
    //
    // Ask the server again — cheaply, and rarely — in case the operator has
    // just fixed it.  user.changed normally tells us, but a mapping made
    // before we connected, or one the server does not announce, would
    // otherwise never be noticed while the browser stays up.
    const base::TimeTicks now = base::TimeTicks::Now();
    if (client_ && (last_user_list_request_.is_null() ||
                    now - last_user_list_request_ > base::Seconds(5))) {
      last_user_list_request_ = now;
      client_->RequestUserList();
    }

    if (owners_.size() > 1) {
      // Several owners, and nothing says which one typed.  Handing the
      // keystroke to the primary owner was right when there was one owner and
      // therefore one possible destination; with several it types into a
      // COLLEAGUE'S window, which destroys their work rather than merely
      // losing a keystroke.  Refuse to guess, and let the dialog say so.
      //
      // The keystroke is still tracked below under the keyboard's own hwid —
      // device hwids are unique across mice and keyboards, so it cannot
      // collide with a pair's state — so held keys stay consistent and the
      // release hotkey, which is the way out of capture, keeps working from
      // any keyboard including this one.
      pair_hwid = hwid;
      deliverable = false;
    } else {
      pair_hwid = owner_hwid_;
    }
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "KEY UNPAIRED: kb_hwid=0x%x not in user list, %s", hwid,
        deliverable ? "falling back to owner" : "dropped (several owners)"));
#endif
  }
  keyboards_seen_.insert(hwid);

  // A keyboard whose user does not own a window does not drive Chrome.
  //
  // Mouse events have always been gated on ownership; keyboard events never
  // were, and deliberately so: with one owner, MouseMux's own hook meant only
  // the captured user's keys could reach us, so there was nothing to gate.
  // Capture several users at once and every keyboard arrives, including ones
  // belonging to people who have not claimed a window — and those have
  // nowhere of their own to go.
  //
  // Only with several owners.  With one, an unowned keyboard typing into the
  // single window is the long-standing single-user behaviour and still what
  // somebody sitting down at the machine expects.
  if (deliverable && owners_.size() > 1 && !IsOwner(pair_hwid)) {
    deliverable = false;
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "KEY NOT AN OWNER: kb_hwid=0x%x pairs to 0x%x, which owns no window",
        hwid, pair_hwid));
#endif
  }

#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf(
      "KEY RECV: kb_hwid=0x%x vkey=0x%x owner=0x%x",
      hwid, vkey, owner_hwid_));
#endif

  // Determine if key down or up based on Windows message.
  // WM_KEYDOWN = 0x100, WM_KEYUP = 0x101
  // WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105
  bool is_down = (message == 0x100 || message == 0x104);
  bool is_up = (message == 0x101 || message == 0x105);

  if (!is_down && !is_up) {
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf("KEY IGNORED: unknown message=0x%x", message));
#endif
    return;
  }



  // Track key state, per device pair.
  DeviceState& kb_state = StateFor(pair_hwid);
  std::set<int>& pressed_keys = kb_state.pressed_keys;
  if (is_down) {
    if (pressed_keys.count(vkey)) {
#ifdef MOUSEMUX_DEBUG
      LogDebug(base::StringPrintf(
          "KEY ACCEPT REPEAT: kb=0x%x vkey=0x%x owner=0x%x",
          hwid, vkey, owner_hwid_));
#endif
    } else {
      pressed_keys.insert(vkey);
#ifdef MOUSEMUX_DEBUG
      LogDebug(base::StringPrintf(
          "KEY ACCEPT DOWN: kb=0x%x vkey=0x%x scan=%d owner=0x%x views=%zu",
          hwid, vkey, scan, owner_hwid_,
          registered_views_.size()));
#endif
    }
  } else {
    pressed_keys.erase(vkey);
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "KEY ACCEPT UP: kb=0x%x vkey=0x%x scan=%d owner=0x%x",
        hwid, vkey, scan, owner_hwid_));
#endif
  }

  // Check for hotkey (only on key down).
  if (is_down && keyboard_event_callback_) {
    // Check modifiers from both SDK tracked keys AND Win32 GetAsyncKeyState.
    // Win32 fallback ensures hotkeys work even if the SDK missed a modifier
    // key event (e.g. key was pressed before connection or focus change).
    bool sdk_shift = pressed_keys.count(VK_SHIFT) || pressed_keys.count(VK_LSHIFT) ||
                     pressed_keys.count(VK_RSHIFT);
    bool sdk_ctrl = pressed_keys.count(VK_CONTROL) || pressed_keys.count(VK_LCONTROL) ||
                    pressed_keys.count(VK_RCONTROL);
    bool sdk_alt = pressed_keys.count(VK_MENU) || pressed_keys.count(VK_LMENU) ||
                   pressed_keys.count(VK_RMENU);
    bool win32_shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool win32_ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool win32_alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool shift = sdk_shift || win32_shift;
    bool ctrl = sdk_ctrl || win32_ctrl;
    bool alt = sdk_alt || win32_alt;
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "HOTKEY CHECK: vkey=0x%x sdk_shift=%d sdk_ctrl=%d sdk_alt=%d "
        "win32_shift=%d win32_ctrl=%d win32_alt=%d",
        vkey, sdk_shift, sdk_ctrl, sdk_alt, win32_shift, win32_ctrl, win32_alt));
#endif
    if (keyboard_event_callback_.Run(vkey, shift, ctrl, alt, is_down)) {
#ifdef MOUSEMUX_DEBUG
      LogDebug("KEY CONSUMED by hotkey callback");
#endif
      return;
    }
  }

  // A keystroke that could not be attributed goes no further.  It has been
  // tracked and offered to the hotkey; delivering it anywhere now would be a
  // guess at whose window it belongs in.
  if (!deliverable) {
    if (is_down) {
      RecordKeyRoute(hwid, -1, std::u16string(), true);
    }
    return;
  }

  // Inject the keyboard event.
  if (registered_views_.empty()) {
    LogDebug("KEY INJECT FAILED: No views registered!");
    return;
  }

  // Route to the view THIS device last clicked in, not the one any device
  // clicked last.  This is what lets several people type at once: each pair's
  // keystrokes follow its own mouse.
  RenderWidgetHostViewAura* view = nullptr;
  if (kb_state.keyboard_target_view &&
      registered_views_.count(kb_state.keyboard_target_view)) {
    view = kb_state.keyboard_target_view;
  } else if (owners_.size() > 1) {
    // This device has not claimed a window yet, and with several users there
    // is no harmless guess: the first showing view belongs to one of the
    // others, so falling back to it types this user's keys into a colleague's
    // page.  Their row in the dialog says "not in a window yet"; a click
    // fixes it.
    if (is_down) {
      RecordKeyRoute(hwid, pair_hwid, std::u16string(), true);
    }
    return;
  } else {
    // Single user: the old fallback, which is safe because there is only one
    // place the keystroke could go.  First showing view, then any view.
    for (RenderWidgetHostViewAura* v : registered_views_) {
      if (v && v->IsShowing()) {
        view = v;
        break;
      }
    }
    if (!view) {
      view = *registered_views_.begin();
    }
  }
#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf(
      "KEY INJECT -> view=%p views_total=%zu",
      static_cast<void*>(view), registered_views_.size()));
#endif
  if (is_down) {
    RecordKeyRoute(hwid, pair_hwid, ToplevelTitleOf(view), false);
  }
  InjectKeyboardEvent(pair_hwid, view, vkey, scan, is_down);
}

void MouseMuxInputController::OnTimeoutWarning(int minutes) {
  LogDebug(base::StringPrintf("Timeout warning: %d minutes", minutes));
  std::wstring msg = L"MouseMux server will timeout in " +
                     std::to_wstring(minutes) +
                     (minutes == 1 ? L" minute." : L" minutes.");
  ::MessageBoxW(nullptr, msg.c_str(), L"MouseMux", MB_OK | MB_ICONWARNING);
}

void MouseMuxInputController::OnTimeoutStopped(const std::string& reason) {
  LogDebug("Session stopped: " + reason);
  std::wstring msg = L"MouseMux session ended: " +
                     std::wstring(reason.begin(), reason.end());
  ::MessageBoxW(nullptr, msg.c_str(), L"MouseMux", MB_OK | MB_ICONERROR);
}

int MouseMuxInputController::GetOwnerKeyboardHwid() const {
  if (owner_hwid_ == -1) {
    return -1;
  }
  auto it = user_info_.find(owner_hwid_);
  if (it != user_info_.end()) {
    return it->second.hwid_keyboard;
  }
  return -1;
}

RenderWidgetHostViewAura* MouseMuxInputController::FindViewAtPoint(
    float screen_x,
    float screen_y,
    bool verbose_log) {
  if (registered_views_.empty()) {
    if (verbose_log) {
      LogDebug("FindViewAtPoint: No views registered!");
    }
    return nullptr;
  }

  // Use the first view's scale factor for coordinate conversion.
  // This works well for single-monitor setups. For multi-monitor with
  // different DPI, the fallback ownership claim handles mismatches.
  float display_scale = 1.0f;
  RenderWidgetHostViewAura* first_view = *registered_views_.begin();
  if (first_view) {
    display_scale = first_view->GetDeviceScaleFactor();
  }

  // Convert physical screen coordinates to DIP.
  float dip_x = screen_x / display_scale;
  float dip_y = screen_y / display_scale;

  // Collect all visible views whose bounds contain the point.
  std::vector<RenderWidgetHostViewAura*> candidates;
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (!view || !view->IsShowing())
      continue;
    gfx::Rect bounds = view->GetViewBounds();
    if (bounds.Contains(static_cast<int>(dip_x), static_cast<int>(dip_y))) {
      candidates.push_back(view);
    }
  }

  if (candidates.empty())
    return nullptr;
  if (candidates.size() == 1)
    return candidates[0];

  // Multiple overlapping views — pick the one in the topmost window.
  // Build HWND → view map, then walk Win32 Z-order (top to bottom).
  std::map<HWND, RenderWidgetHostViewAura*> hwnd_to_view;
  for (auto* view : candidates) {
    if (view->GetNativeView() && view->GetNativeView()->GetHost()) {
      HWND hwnd = view->GetNativeView()->GetHost()->GetAcceleratedWidget();
      hwnd_to_view[hwnd] = view;
    }
  }
  HWND current = ::GetTopWindow(nullptr);
  while (current) {
    auto it = hwnd_to_view.find(current);
    if (it != hwnd_to_view.end()) {
      return it->second;
    }
    current = ::GetNextWindow(current, GW_HWNDNEXT);
  }

  // Fallback (shouldn't happen).
  return candidates[0];
}

bool MouseMuxInputController::IsPointOverChrome(float screen_x, float screen_y) {
  return FindViewAtPoint(screen_x, screen_y) != nullptr;
}

#ifdef MOUSEMUX_PEN_TOUCH_INJECT
void MouseMuxInputController::ApplyPointerProperties(
    int hwid,
    blink::WebMouseEvent* event) {
  event->id = 0;  // Primary pointer.

  // Subtype comes from the user list, so it is known before the first event
  // arrives — no need to infer pen-ness from the event stream.
  MouseMuxClient::PointerSubtype subtype =
      MouseMuxClient::PointerSubtype::kMouse;
  auto user_it = user_info_.find(hwid);
  if (user_it != user_info_.end()) {
    subtype = user_it->second.subtype;
  }

  if (subtype == MouseMuxClient::PointerSubtype::kMouse) {
    event->pointer_type = blink::WebPointerProperties::PointerType::kMouse;
    return;
  }

  event->pointer_type =
      (subtype == MouseMuxClient::PointerSubtype::kPen)
          ? blink::WebPointerProperties::PointerType::kPen
          : blink::WebPointerProperties::PointerType::kTouch;

  auto pen_it = pen_state_.find(hwid);
  if (pen_it == pen_state_.end()) {
    // No pen event seen yet — leave force at its NaN default, which is how
    // blink represents "this device does not report pressure".
    return;
  }
  const PenState& pen = pen_it->second;

  // MouseMux reports pressure as 0-1024; blink wants force in [0,1].
  float force = static_cast<float>(pen.pressure) / 1024.0f;
  event->force = (force > 1.0f) ? 1.0f : force;

  // Tilt is already in degrees within [-90,90].
  event->tilt_x = static_cast<double>(pen.tilt_x);
  event->tilt_y = static_cast<double>(pen.tilt_y);

  // twist must land in [0,359].
  int twist = pen.rotation % 360;
  event->twist = (twist < 0) ? twist + 360 : twist;
}
#endif

void MouseMuxInputController::InjectMouseEvent(
    int hwid,
    RenderWidgetHostViewAura* view,
    blink::WebInputEvent::Type type,
    float screen_x,
    float screen_y,
    int button_flags) {
  if (!view) {
    LogDebug("InjectMouseEvent: view is null!");
    return;
  }

  RenderWidgetHostImpl* host = RenderWidgetHostImpl::From(
      view->GetRenderWidgetHost());
  if (!host) {
    LogDebug("InjectMouseEvent: host is null!");
    return;
  }

  // Dismiss any active context menu before injecting a mouse down event.
  // Context menus are browser-layer views widgets that don't receive injected
  // WebMouseEvents, so they would otherwise stay open indefinitely.
  // The callback is provided by the chrome layer which has access to
  // views::MenuController.
  if (type == blink::WebInputEvent::Type::kMouseDown && menu_dismiss_callback_) {
    menu_dismiss_callback_.Run(hwid);
  }

  // Set aura mouse capture on mouse down, release on mouse up.
  // This is needed for selection/drag to work — native mouse handling calls
  // SetCapture which Chrome's selection logic depends on.
  if (type == blink::WebInputEvent::Type::kMouseDown) {
    aura::Window* native_view = view->GetNativeView();
    if (native_view) {
      native_view->SetCapture();
    }
  } else if (type == blink::WebInputEvent::Type::kMouseUp) {
    aura::Window* native_view = view->GetNativeView();
    if (native_view && native_view->HasCapture()) {
      native_view->ReleaseCapture();
    }
  }

  // Focus only on button events — NOT mousemove.  Focusing on mousemove
  // activates the window (changes Z-order) which makes a background window
  // jump to the front when the SDK cursor merely passes over it.
  if (type == blink::WebInputEvent::Type::kMouseDown) {
#ifdef MOUSEMUX_MULTI_OWNER
    // Renderer page focus only — see the matching block in
    // InjectKeyboardEvent.  view->Focus() is the OS window and would activate
    // this window, blurring whichever window another user is working in.
    //
    // Re-assert on EVERY registered view, not just the clicked one: a click
    // must not take page focus away from a window somebody else is typing in.
    // Doing it only for `view` here was an asymmetry with the keyboard path —
    // a click blurred the other window and only the next keystroke restored it.
    for (RenderWidgetHostViewAura* v : registered_views_) {
      if (!v) {
        continue;
      }
      if (RenderWidgetHostImpl* h =
              RenderWidgetHostImpl::From(v->GetRenderWidgetHost())) {
        h->Focus();
      }
    }
#else
    if (!view->HasFocus()) {
      view->Focus();
    }
    // Also set page-level focus directly (sends SetFocus IPC to renderer),
    // matching what DevTools Input.dispatchMouseEvent does.
    host->Focus();
#endif  // MOUSEMUX_MULTI_OWNER
  }

  // Get device scale factor for coordinate transformation.
  float device_scale = view->GetDeviceScaleFactor();

  // Convert physical screen coordinates to DIP.
  float dip_screen_x = screen_x / device_scale;
  float dip_screen_y = screen_y / device_scale;

  // Get view bounds to calculate widget-relative coordinates.
  gfx::Rect view_bounds = view->GetViewBounds();
  float widget_x = dip_screen_x - view_bounds.x();
  float widget_y = dip_screen_y - view_bounds.y();

  // Create the WebMouseEvent.
  // CRITICAL: kFromDebugger tells Chrome this is a synthetic/injected event.
  // Without it, certain event processing paths may not work correctly.
  // Also include the button-down flags in modifiers for button events.
  int modifiers = blink::WebInputEvent::kFromDebugger;
  if (type == blink::WebInputEvent::Type::kMouseDown ||
      type == blink::WebInputEvent::Type::kMouseUp) {
    modifiers |= button_flags;  // Include kLeftButtonDown/kRightButtonDown
  } else if (type == blink::WebInputEvent::Type::kMouseMove) {
    modifiers |= StateFor(hwid).button_state;  // Include held button state for drags
  }

  blink::WebMouseEvent event(type, modifiers, base::TimeTicks::Now());

  // Set positions.
  event.SetPositionInWidget(widget_x, widget_y);
  event.SetPositionInScreen(dip_screen_x, dip_screen_y);

  // Set button and click count.
  if (type == blink::WebInputEvent::Type::kMouseDown) {
    if (button_flags & blink::WebMouseEvent::kLeftButtonDown) {
      event.button = blink::WebPointerProperties::Button::kLeft;
    } else if (button_flags & blink::WebMouseEvent::kRightButtonDown) {
      event.button = blink::WebPointerProperties::Button::kRight;
    } else if (button_flags & blink::WebMouseEvent::kMiddleButtonDown) {
      event.button = blink::WebPointerProperties::Button::kMiddle;
    } else {
      event.button = blink::WebPointerProperties::Button::kNoButton;
    }
    event.click_count = 1;
  } else if (type == blink::WebInputEvent::Type::kMouseUp) {
    if (button_flags & blink::WebMouseEvent::kLeftButtonDown) {
      event.button = blink::WebPointerProperties::Button::kLeft;
    } else if (button_flags & blink::WebMouseEvent::kRightButtonDown) {
      event.button = blink::WebPointerProperties::Button::kRight;
    } else if (button_flags & blink::WebMouseEvent::kMiddleButtonDown) {
      event.button = blink::WebPointerProperties::Button::kMiddle;
    } else {
      event.button = blink::WebPointerProperties::Button::kNoButton;
    }
    event.click_count = 1;  // Click count should be 1 for up too
  } else {
    // For move events, set button to reflect held button for drag/selection.
    // Blink's selection handler checks event.button during mousemove.
    if (StateFor(hwid).button_state & blink::WebMouseEvent::kLeftButtonDown) {
      event.button = blink::WebPointerProperties::Button::kLeft;
    } else if (StateFor(hwid).button_state & blink::WebMouseEvent::kRightButtonDown) {
      event.button = blink::WebPointerProperties::Button::kRight;
    } else if (StateFor(hwid).button_state & blink::WebMouseEvent::kMiddleButtonDown) {
      event.button = blink::WebPointerProperties::Button::kMiddle;
    } else {
      event.button = blink::WebPointerProperties::Button::kNoButton;
    }
    event.click_count = 0;
  }

#ifdef MOUSEMUX_PEN_TOUCH_INJECT
  // Set pointer type, plus pressure/tilt/twist when the owner is a pen or
  // touch device.  Done for button events as well as motion — if the two
  // disagreed, Blink would see contact and movement as separate pointers.
  ApplyPointerProperties(hwid, &event);
#else
  // Set pointer type to mouse.
  event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  event.id = 0;  // Primary pointer
#endif

#ifdef MOUSEMUX_DEBUG
  // Log ALL injection details for button events.
  if (type == blink::WebInputEvent::Type::kMouseDown ||
      type == blink::WebInputEvent::Type::kMouseUp) {
    const char* type_str = (type == blink::WebInputEvent::Type::kMouseDown) ? "DOWN" : "UP";
    const char* btn_str = "?";
    if (event.button == blink::WebPointerProperties::Button::kLeft) btn_str = "LEFT";
    else if (event.button == blink::WebPointerProperties::Button::kRight) btn_str = "RIGHT";
    else if (event.button == blink::WebPointerProperties::Button::kMiddle) btn_str = "MIDDLE";
    else if (event.button == blink::WebPointerProperties::Button::kNoButton) btn_str = "NONE";

    LogDebug(base::StringPrintf(">>> INJECT %s %s: widget(%.1f,%.1f) screen(%.1f,%.1f) mods=0x%x click=%d",
                                 type_str, btn_str,
                                 widget_x, widget_y,
                                 dip_screen_x, dip_screen_y,
                                 event.GetModifiers(),
                                 event.click_count));
    // Log host state for click debugging.
    LogDebug(base::StringPrintf("    host: focused=%d active=%d process_ready=%d view_bounds=(%d,%d,%d,%d) scale=%.2f",
                                 host->is_focused(),
                                 host->is_active(),
                                 host->GetProcess()->IsReady(),
                                 view_bounds.x(), view_bounds.y(),
                                 view_bounds.width(), view_bounds.height(),
                                 device_scale));
  }

  // Log move events during drag (when a button is held).
  if (type == blink::WebInputEvent::Type::kMouseMove && StateFor(hwid).button_state != 0) {
    LogDebug(base::StringPrintf(
        ">>> DRAG MOVE: widget(%.1f,%.1f) screen(%.1f,%.1f) mods=0x%x btn_state=0x%x",
        widget_x, widget_y, dip_screen_x, dip_screen_y,
        event.GetModifiers(), StateFor(hwid).button_state));
  }
#endif

  // Detect and recover from stuck InputRouter (un-acked pending events).
  // When the InputRouter has had pending events for >300ms, it means the
  // renderer stopped acking events (typically after a view transition).
  // Reset the InputRouter to clear the stuck state, mirroring what
  // ResetStateForCreatedRenderWidget() does during widget creation.
  bool has_pending = host->input_router()->HasPendingEvents();
  if (has_pending) {
    if (pending_view_ != view) {
      // New view with pending state - start tracking.
      pending_view_ = view;
      pending_start_time_ = base::TimeTicks::Now();
    } else {
      base::TimeDelta pending_duration =
          base::TimeTicks::Now() - pending_start_time_;
      if (pending_duration > base::Milliseconds(300)) {
        DiagLog(base::StringPrintf(
            "*** InputRouter STUCK for %lldms - resetting. view=%p",
            pending_duration.InMilliseconds(),
            static_cast<void*>(view)));
        host->ResetInputRouterForInjection();
        pending_view_ = nullptr;
        has_pending = false;  // Cleared now.
      }
    }
  } else {
    // Not pending anymore - clear tracking.
    if (pending_view_ == view) {
      pending_view_ = nullptr;
    }
  }

  // Diagnostic: check if the host will silently drop this event.
  bool is_ignoring = host->IsIgnoringWebInputEvents(event);

  // Log diagnostics for button events always, motion every 120th (~2s at 60fps).
  bool should_log_diag = (type == blink::WebInputEvent::Type::kMouseDown ||
                          type == blink::WebInputEvent::Type::kMouseUp ||
                          (type == blink::WebInputEvent::Type::kMouseMove &&
                           motion_count_ % 120 == 0));
  if (should_log_diag) {
    DiagLog(base::StringPrintf(
        "DIAG MOUSE: ignoring=%d pending=%d views=%zu view=%p",
        is_ignoring, has_pending,
        registered_views_.size(), static_cast<void*>(view)));
  }

  if (is_ignoring) {
    DiagLog(base::StringPrintf(
        "*** DROPPING: IsIgnoring=TRUE type=%d",
        static_cast<int>(type)));
  }

  // Forward the event. Use ForwardMouseEvent (not ForwardMouseEventWithLatencyInfo)
  // to ensure RenderWidgetDidForwardMouseEvent is called on the owner delegate.
  host->ForwardMouseEvent(event);
}

void MouseMuxInputController::InjectMouseEventToAnyView(
    int hwid,
    blink::WebInputEvent::Type type,
    float screen_x,
    float screen_y,
    int button_flags) {
  RenderWidgetHostViewAura* view = nullptr;
  DeviceState& state = StateFor(hwid);

  // During drag (button held), route all events to THIS device's drag target.
  // Critical for text selection — Chrome's selection handler requires every
  // event in a drag sequence to reach the same view — and per device, or one
  // user's drag would capture another user's movement.
  if (state.button_state != 0 && state.drag_target_view &&
      registered_views_.count(state.drag_target_view)) {
    view = state.drag_target_view;
  } else {
#ifdef MOUSEMUX_AURA_UI_CLICK_THROUGH
    // For button events, check if there's an overlay window (context menu,
    // popup, dropdown) at the coordinates BEFORE checking web content.
    // Overlay windows sit above web content; without this check, clicks on
    // menu items would hit the web content view behind the menu, dismiss
    // the menu via menu_dismiss_callback_, and lose the menu action.
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp) {
      if (TryDispatchToOverlayWindow(type, screen_x, screen_y, button_flags)) {
        return;
      }
    }
#endif

    // Normal case: find the view under the cursor.
    view = FindViewAtPoint(screen_x, screen_y);
  }

  if (!view) {
    MMTRACE("CTRL/Inject", "no web view at (%.0f,%.0f) type=%d - aura fallback",
            screen_x, screen_y, static_cast<int>(type));
#ifdef MOUSEMUX_AURA_UI_CLICK_THROUGH
    // No web content view under the cursor — try dispatching through
    // the aura event system so Chrome UI (tabs, toolbar, etc.) can respond.
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp) {
      const gfx::AcceleratedWidget target =
          FindAuraTargetWindow(screen_x, screen_y);
      if (!target) {
        return;
      }

      // The hard lock has to hold here too.  A click on another user's tab
      // strip, toolbar or bookmark bar is still a click in their window, and
      // this path — anything that is not page content — was the one way
      // round it.
      if (hard_lock_) {
        if (state.locked_window && !::IsWindow(state.locked_window)) {
          state.locked_window = gfx::kNullAcceleratedWidget;
        }
        if (!state.locked_window) {
          state.locked_window = target;
        } else if (target != state.locked_window) {
          MMTRACE("CTRL/HardLock",
                  "DROPPED hwid=%d clicked outside its window (chrome UI)",
                  hwid);
          return;
        }
      }

      // Clicking a tab, the toolbar or a blank strip of a window is how
      // people pick a window up, and until now it did not tell that user's
      // KEYBOARD anything: only clicks that landed on page content set a
      // keyboard target.  So a user whose first click was on Chrome's own UI
      // had no target at all, and their typing fell through to whichever
      // view happened to be first in the list — which, with several users,
      // is somebody else's window.
      if (type == blink::WebInputEvent::Type::kMouseDown) {
        if (RenderWidgetHostViewAura* web_view = WebViewInWindow(target)) {
          state.keyboard_target_view = web_view;
        }
      }

      DispatchToAuraHost(target, type, screen_x, screen_y, button_flags);
    }
#else
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp) {
      LogDebug("INJECT FAILED: No view available!");
    }
#endif
    return;
  }

  // Hard lock: confine this device to its own window.
  //
  // Applied here, after the view is resolved, so the test is against the
  // window the click actually landed in.  Movement is deliberately NOT
  // blocked — only button events — so a user's cursor can still travel across
  // the screen and they can see where they are; it simply cannot act outside
  // their window.
  if (hard_lock_ && type != blink::WebInputEvent::Type::kMouseMove) {
    const gfx::AcceleratedWidget hit_window = ToplevelWindowOf(view);
    // A window that has gone away must not keep its user locked out.
    if (state.locked_window && !::IsWindow(state.locked_window)) {
      state.locked_window = gfx::kNullAcceleratedWidget;
    }
    if (!state.locked_window) {
      // First click claims a window for this device.
      state.locked_window = hit_window;
      LogDebug(base::StringPrintf("HARD LOCK: hwid=0x%x claimed window %p",
                                  hwid, state.locked_window));
    } else if (hit_window != state.locked_window) {
      MMTRACE("CTRL/HardLock", "DROPPED hwid=%d clicked outside its window",
              hwid);
      return;
    }
  }

  // Track this device's drag target: set on mousedown, clear on mouseup.
  if (type == blink::WebInputEvent::Type::kMouseDown) {
    state.drag_target_view = view;
    // This device's keyboard follows its own click — not every device's.
    state.keyboard_target_view = view;
  } else if (type == blink::WebInputEvent::Type::kMouseUp &&
             state.button_state == 0) {
    state.drag_target_view = nullptr;
  }

  InjectMouseEvent(hwid, view, type, screen_x, screen_y, button_flags);
}

#ifdef MOUSEMUX_AURA_UI_CLICK_THROUGH
bool MouseMuxInputController::TryDispatchToOverlayWindow(
    blink::WebInputEvent::Type type,
    float screen_x,
    float screen_y,
    int button_flags) {
  aura::Env* env = aura::Env::GetInstance();
  if (!env) return false;

  // Collect the WindowTreeHosts that own our registered web content views.
  std::set<aura::WindowTreeHost*> web_content_hosts;
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (view && view->GetNativeView() && view->GetNativeView()->GetHost()) {
      web_content_hosts.insert(view->GetNativeView()->GetHost());
    }
  }
  if (web_content_hosts.empty()) return false;

  gfx::Point screen_pt(static_cast<int>(screen_x), static_cast<int>(screen_y));

  // Collect all visible hosts at this point, then use Z-order to find topmost.
  const auto& hosts = env->window_tree_hosts();
  std::map<HWND, aura::WindowTreeHost*> candidates;
  for (aura::WindowTreeHost* host : hosts) {
    if (!host) continue;
    HWND hwnd = host->GetAcceleratedWidget();
    if (!hwnd || !::IsWindowVisible(hwnd)) continue;
#ifdef MOUSEMUX_EXPERIMENT_NC_HANDLING
    // Use GetWindowRect (full window including title bar) not
    // GetBoundsInPixels (client area only) so title bar clicks match.
    RECT win_rect;
    if (!::GetWindowRect(hwnd, &win_rect)) continue;
    POINT pt = {screen_pt.x(), screen_pt.y()};
    if (!::PtInRect(&win_rect, pt)) continue;
#else
    gfx::Rect bounds = host->GetBoundsInPixels();
    if (!bounds.Contains(screen_pt)) continue;
#endif
    candidates[hwnd] = host;
  }
  if (candidates.empty()) return false;

  // Walk Win32 Z-order to find the topmost candidate.
  aura::WindowTreeHost* topmost_host = nullptr;
  HWND topmost_hwnd = nullptr;
  HWND current = ::GetTopWindow(nullptr);
  while (current) {
    auto it = candidates.find(current);
    if (it != candidates.end()) {
      topmost_hwnd = it->first;
      topmost_host = it->second;
      break;
    }
    current = ::GetNextWindow(current, GW_HWNDNEXT);
  }
  if (!topmost_host) return false;

  if (web_content_hosts.count(topmost_host)) {
    // Topmost is a web content host — let normal injection handle it.
    return false;
  }

  // It's an overlay window (context menu, popup, dialog).
  // Always PostMessage and return true to prevent InjectMouseEvent from
  // dismissing the menu and forwarding a stale click to the renderer.
  POINT client_pt = {static_cast<LONG>(screen_x), static_cast<LONG>(screen_y)};
  ::ScreenToClient(topmost_hwnd, &client_pt);
  LPARAM lparam = MAKELPARAM(client_pt.x, client_pt.y);

  UINT msg = 0;
  WPARAM wparam = 0;
  if (type == blink::WebInputEvent::Type::kMouseDown) {
    if (button_flags & blink::WebMouseEvent::kLeftButtonDown) {
      msg = WM_MOUSEMUX_LBUTTONDOWN; wparam = MK_LBUTTON;
    } else if (button_flags & blink::WebMouseEvent::kRightButtonDown) {
      msg = WM_MOUSEMUX_RBUTTONDOWN; wparam = MK_RBUTTON;
    } else if (button_flags & blink::WebMouseEvent::kMiddleButtonDown) {
      msg = WM_MOUSEMUX_MBUTTONDOWN; wparam = MK_MBUTTON;
    }
  } else {
    if (button_flags & blink::WebMouseEvent::kLeftButtonDown) {
      msg = WM_MOUSEMUX_LBUTTONUP;
    } else if (button_flags & blink::WebMouseEvent::kRightButtonDown) {
      msg = WM_MOUSEMUX_RBUTTONUP;
    } else if (button_flags & blink::WebMouseEvent::kMiddleButtonDown) {
      msg = WM_MOUSEMUX_MBUTTONUP;
    }
  }

  if (msg) {
    DiagLog(base::StringPrintf(
        "OVERLAY DISPATCH: msg=0x%x to hwnd=%p client(%ld,%ld)",
        msg, static_cast<void*>(topmost_hwnd), client_pt.x, client_pt.y));
    ::PostMessage(topmost_hwnd, msg, wparam, lparam);
  }
  return true;
}

gfx::AcceleratedWidget MouseMuxInputController::FindAuraTargetWindow(
    float screen_x,
    float screen_y) {
  // Enumerate ALL aura WindowTreeHosts and collect visible ones at the point.
  aura::Env* env = aura::Env::GetInstance();
  if (!env) {
    DiagLog("FindAuraTargetWindow: no aura::Env");
    return gfx::kNullAcceleratedWidget;
  }

  gfx::Point screen_pt(static_cast<int>(screen_x), static_cast<int>(screen_y));
  const auto& hosts = env->window_tree_hosts();

  // Collect candidate HWNDs that contain the point and are visible.
  std::map<HWND, aura::WindowTreeHost*> candidates;
  for (aura::WindowTreeHost* host : hosts) {
    if (!host) continue;
    HWND hwnd = host->GetAcceleratedWidget();
    if (!hwnd || !::IsWindowVisible(hwnd)) continue;
#ifdef MOUSEMUX_EXPERIMENT_NC_HANDLING
    // Use GetWindowRect (full window including title bar) not
    // GetBoundsInPixels (client area only) so title bar clicks match.
    RECT win_rect;
    if (!::GetWindowRect(hwnd, &win_rect)) continue;
    POINT pt = {screen_pt.x(), screen_pt.y()};
    if (!::PtInRect(&win_rect, pt)) continue;
#else
    gfx::Rect bounds = host->GetBoundsInPixels();
    if (!bounds.Contains(screen_pt)) continue;
#endif
    candidates[hwnd] = host;
  }

  if (candidates.empty()) {
    DiagLog("FindAuraTargetWindow: NO HOST MATCHED");
    return gfx::kNullAcceleratedWidget;
  }

  // Walk Win32 Z-order (top to bottom) to find the topmost candidate.
  HWND current = ::GetTopWindow(nullptr);
  while (current) {
    if (candidates.count(current)) {
      return current;
    }
    current = ::GetNextWindow(current, GW_HWNDNEXT);
  }

  // Fallback — pick any candidate.
  return candidates.begin()->first;
}

void MouseMuxInputController::DispatchToAuraHost(
    gfx::AcceleratedWidget target,
    blink::WebInputEvent::Type type,
    float screen_x,
    float screen_y,
    int button_flags) {
  // Determine custom Win32 message to send (WM_MOUSEMUX_* range).
  // Using custom messages avoids collision with native WM_LBUTTONDOWN etc.
  // PreHandleMSG in DesktopWindowTreeHostWin converts them back.
  UINT msg = 0;
  WPARAM wparam = 0;
  if (type == blink::WebInputEvent::Type::kMouseDown) {
    if (button_flags & blink::WebMouseEvent::kLeftButtonDown) {
      msg = WM_MOUSEMUX_LBUTTONDOWN; wparam = MK_LBUTTON;
    } else if (button_flags & blink::WebMouseEvent::kRightButtonDown) {
      msg = WM_MOUSEMUX_RBUTTONDOWN; wparam = MK_RBUTTON;
    } else if (button_flags & blink::WebMouseEvent::kMiddleButtonDown) {
      msg = WM_MOUSEMUX_MBUTTONDOWN; wparam = MK_MBUTTON;
    }
  } else if (type == blink::WebInputEvent::Type::kMouseUp) {
    if (button_flags & blink::WebMouseEvent::kLeftButtonDown) {
      msg = WM_MOUSEMUX_LBUTTONUP;
    } else if (button_flags & blink::WebMouseEvent::kRightButtonDown) {
      msg = WM_MOUSEMUX_RBUTTONUP;
    } else if (button_flags & blink::WebMouseEvent::kMiddleButtonDown) {
      msg = WM_MOUSEMUX_MBUTTONUP;
    }
  }
  if (!msg) return;

  HWND target_hwnd = target;
  if (!target_hwnd) return;

  POINT client_pt = {static_cast<LONG>(screen_x), static_cast<LONG>(screen_y)};
  ::ScreenToClient(target_hwnd, &client_pt);
  LPARAM lparam = MAKELPARAM(client_pt.x, client_pt.y);

  DiagLog(base::StringPrintf(
      "  POSTING msg=0x%x to hwnd=%p client(%ld,%ld)",
      msg, static_cast<void*>(target_hwnd), client_pt.x, client_pt.y));
  ::PostMessage(target_hwnd, msg, wparam, lparam);
}
#endif  // MOUSEMUX_AURA_UI_CLICK_THROUGH

void MouseMuxInputController::InjectWheelEvent(
    int hwid,
    RenderWidgetHostViewAura* view,
    float screen_x,
    float screen_y,
    int delta,
    bool horizontal) {
  if (!view)
    return;

  RenderWidgetHostImpl* host = RenderWidgetHostImpl::From(
      view->GetRenderWidgetHost());
  if (!host)
    return;

  // Get device scale factor for coordinate transformation.
  float device_scale = view->GetDeviceScaleFactor();

  // Convert physical screen coordinates to DIP.
  float dip_screen_x = screen_x / device_scale;
  float dip_screen_y = screen_y / device_scale;

  // Get view bounds to calculate widget-relative coordinates.
  gfx::Rect view_bounds = view->GetViewBounds();
  float widget_x = dip_screen_x - view_bounds.x();
  float widget_y = dip_screen_y - view_bounds.y();

  // Create the WebMouseWheelEvent following ChromeDriver's approach.
  // MouseMux delta is in raw units (typically 120 per notch).
  // ChromeDriver negates the delta values, we do the same.
  float scroll_delta = static_cast<float>(delta) / 120.0f * 40.0f;

  // CRITICAL: kFromDebugger marks this as a synthetic/injected event.
  int modifiers = blink::WebInputEvent::kFromDebugger;
  modifiers |= StateFor(hwid).button_state;

  blink::WebMouseWheelEvent event(
      blink::WebInputEvent::Type::kMouseWheel,
      modifiers,
      base::TimeTicks::Now());

  event.SetPositionInWidget(widget_x, widget_y);
  event.SetPositionInScreen(dip_screen_x, dip_screen_y);

  // Set scroll delta based on direction (horizontal or vertical).
  if (horizontal) {
    event.delta_x = scroll_delta;
    event.delta_y = 0;
    event.wheel_ticks_y = 0;
    if (scroll_delta != 0.0f) {
      event.wheel_ticks_x = scroll_delta > 0.0f ? 1.0f : -1.0f;
    } else {
      event.wheel_ticks_x = 0;
    }
  } else {
    event.delta_x = 0;
    event.delta_y = scroll_delta;
    event.wheel_ticks_x = 0;
    if (scroll_delta != 0.0f) {
      event.wheel_ticks_y = scroll_delta > 0.0f ? 1.0f : -1.0f;
    } else {
      event.wheel_ticks_y = 0;
    }
  }

  // ChromeDriver's wheel event settings - these are critical!
  event.phase = blink::WebMouseWheelEvent::kPhaseBegan;
  event.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
  event.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;

  // Detect stuck InputRouter for wheel events too.
  bool wheel_pending = host->input_router()->HasPendingEvents();
  if (wheel_pending) {
    if (pending_view_ != view) {
      pending_view_ = view;
      pending_start_time_ = base::TimeTicks::Now();
    } else {
      base::TimeDelta pending_duration =
          base::TimeTicks::Now() - pending_start_time_;
      if (pending_duration > base::Milliseconds(300)) {
        DiagLog(base::StringPrintf(
            "*** InputRouter STUCK (wheel) for %lldms - resetting. view=%p",
            pending_duration.InMilliseconds(),
            static_cast<void*>(view)));
        host->ResetInputRouterForInjection();
        pending_view_ = nullptr;
      }
    }
  } else if (pending_view_ == view) {
    pending_view_ = nullptr;
  }

  // Check if the host will drop this event.
  bool wheel_ignoring = host->IsIgnoringWebInputEvents(event);
  if (wheel_ignoring) {
    DiagLog("*** DROPPING WHEEL: IsIgnoring=TRUE");
  }

  // Forward the event.
  host->ForwardWheelEventWithLatencyInfo(event, ui::LatencyInfo());
}

void MouseMuxInputController::FocusKeyboardTargetView() {
#ifdef MOUSEMUX_MULTI_OWNER
  // Single-owner by construction: it picks ONE view and drags the OS
  // foreground window to it, then posts delayed re-focus attempts at 50, 150
  // and 300ms to beat the dialog.  With several users that is focus churn
  // arriving a third of a second after a capture toggle, stealing whatever
  // window somebody else was working in.
  //
  // Renderer page focus for every view instead, and nothing at OS level.
  for (RenderWidgetHostViewAura* v : registered_views_) {
    if (!v) {
      continue;
    }
    if (RenderWidgetHostImpl* h =
            RenderWidgetHostImpl::From(v->GetRenderWidgetHost())) {
      h->Focus();
    }
  }
  return;
#else
  // Single-owner path: the owner's own keyboard target.
  RenderWidgetHostViewAura* view =
      (owner_hwid_ == -1) ? nullptr
                          : StateFor(owner_hwid_).keyboard_target_view.get();
  if (!view || !registered_views_.count(view)) {
    // Fall back to first showing view.
    for (RenderWidgetHostViewAura* v : registered_views_) {
      if (v && v->IsShowing()) {
        view = v;
        break;
      }
    }
  }
  if (!view) return;

  HWND browser_hwnd = nullptr;
  aura::Window* native_view = view->GetNativeView();
  if (native_view) {
    aura::Window* toplevel = native_view->GetToplevelWindow();
    if (toplevel) {
      browser_hwnd = toplevel->GetHost()->GetAcceleratedWidget();
    }
  }

  // Immediate focus attempt.
  if (browser_hwnd) {
    ::SetForegroundWindow(browser_hwnd);
  }
  view->Focus();
  RenderWidgetHostImpl* host = RenderWidgetHostImpl::From(
      view->GetRenderWidgetHost());
  if (host) {
    host->Focus();
  }

#ifdef MOUSEMUX_DEBUG
  HWND fg_now = ::GetForegroundWindow();
  LogDebug(base::StringPrintf(
      "FocusKeyboardTargetView: browser_hwnd=%p fg_before=%p fg_after=%p",
      browser_hwnd, fg_now, ::GetForegroundWindow()));
#endif

  // The dialog's button-click processing can steal focus back after we return.
  // Post delayed re-focus attempts to overcome this.
  if (browser_hwnd) {
    for (int delay_ms : {50, 150, 300}) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](MouseMuxInputController* self, HWND target,
                 int delay_val) {
                if (!::IsWindow(target))
                  return;
                HWND current_fg = ::GetForegroundWindow();
                if (current_fg == target)
                  return;  // Already focused, nothing to do.

                ::SetForegroundWindow(target);

                // Also re-focus the view and host for renderer IPC.
                RenderWidgetHostViewAura* v =
                    (self->owner_hwid_ == -1)
                        ? nullptr
                        : self->StateFor(self->owner_hwid_)
                              .keyboard_target_view.get();
                if (v && self->registered_views_.count(v)) {
                  v->Focus();
                  RenderWidgetHostImpl* h = RenderWidgetHostImpl::From(
                      v->GetRenderWidgetHost());
                  if (h) {
                    h->Focus();
                  }
                }

#ifdef MOUSEMUX_DEBUG
                self->LogDebug(base::StringPrintf(
                    "FocusKeyboardTargetView delayed refocus (%dms): "
                    "was=%p now=%p target=%p",
                    delay_val, current_fg, ::GetForegroundWindow(), target));
#else
                (void)delay_val;
#endif
              },
              base::Unretained(this), browser_hwnd, delay_ms),
          base::Milliseconds(delay_ms));
    }
  }
#endif  // MOUSEMUX_MULTI_OWNER
}

void MouseMuxInputController::InjectKeyboardEvent(
    int hwid,
    RenderWidgetHostViewAura* view,
    int vkey,
    int scan,
    bool is_down) {
  if (!view) {
    LogDebug("InjectKeyboardEvent: view is null!");
    return;
  }

  RenderWidgetHostImpl* host = RenderWidgetHostImpl::From(
      view->GetRenderWidgetHost());
  if (!host) {
    LogDebug("InjectKeyboardEvent: host is null!");
    return;
  }

#ifdef MOUSEMUX_MULTI_OWNER
  // No OS focus manipulation at all.  Under capture there is no native input
  // to compete with, so the only thing that ever moved focus was us — and with
  // several users typing at once, SetForegroundWindow is several keystroke
  // streams fighting over one foreground window.
  //
  // Instead tell EVERY registered view's renderer that its page is focused.
  // RenderWidgetHostImpl::Focus() ends in SetPageFocus(true), which is a mojo
  // SetFocus(kFocused) to that one renderer and touches nothing in the OS.
  // Separate browser windows are separate WebContents, so they are separate
  // delegates: there is no arbitration that unfocuses one when another is
  // focused.  Re-asserting on every keystroke also repairs the blur that
  // OnWindowFocused fired when a window was last activated.
  //
  // SPIKE (A6.1): what this is here to answer is whether Blink actually
  // renders and blinks a caret on renderer page-focus alone, in a window that
  // is not the OS foreground.  If it does not, multi-owner is not viable and
  // this block comes out again.
  for (RenderWidgetHostViewAura* v : registered_views_) {
    if (!v) {
      continue;
    }
    if (RenderWidgetHostImpl* h =
            RenderWidgetHostImpl::From(v->GetRenderWidgetHost())) {
      h->Focus();
    }
  }
#else
  // Ensure the native window is active and the view has focus.
  // view->Focus() alone doesn't work if the OS window isn't foreground.
  aura::Window* native_view = view->GetNativeView();
  if (native_view) {
    aura::Window* toplevel = native_view->GetToplevelWindow();
    if (toplevel) {
      HWND hwnd = toplevel->GetHost()->GetAcceleratedWidget();
      if (hwnd && ::GetForegroundWindow() != hwnd) {
        ::SetForegroundWindow(hwnd);
      }
    }
  }
  if (!view->HasFocus()) {
    view->Focus();
  }
  // Also set renderer-level page focus (sends SetFocus IPC), matching what
  // InjectMouseEvent does for button events. Without this, the renderer may
  // not recognize keyboard focus even though the OS-level window has focus.
  host->Focus();
#endif  // MOUSEMUX_MULTI_OWNER

  // Build modifiers from the keys THIS device pair is holding.  Shared state
  // here would mean one user's held Shift capitalising another user's typing.
  const std::set<int>& held = StateFor(hwid).pressed_keys;
  int modifiers = blink::WebInputEvent::kFromDebugger;
  if (held.count(ui::VKEY_SHIFT) || held.count(ui::VKEY_LSHIFT) ||
      held.count(ui::VKEY_RSHIFT)) {
    modifiers |= blink::WebInputEvent::kShiftKey;
  }
  if (held.count(ui::VKEY_CONTROL) || held.count(ui::VKEY_LCONTROL) ||
      held.count(ui::VKEY_RCONTROL)) {
    modifiers |= blink::WebInputEvent::kControlKey;
  }
  if (held.count(ui::VKEY_MENU) || held.count(ui::VKEY_LMENU) ||
      held.count(ui::VKEY_RMENU)) {
    modifiers |= blink::WebInputEvent::kAltKey;
  }

  // Map VK → DomCode → DomKey using Chromium's US-layout tables.
  ui::DomCode dom_code = ui::UsLayoutKeyboardCodeToDomCode(
      static_cast<ui::KeyboardCode>(vkey));

  int ui_flags = 0;
  if (modifiers & blink::WebInputEvent::kShiftKey)
    ui_flags |= ui::EF_SHIFT_DOWN;
  if (modifiers & blink::WebInputEvent::kControlKey)
    ui_flags |= ui::EF_CONTROL_DOWN;
  if (modifiers & blink::WebInputEvent::kAltKey)
    ui_flags |= ui::EF_ALT_DOWN;

  ui::DomKey dom_key;
  ui::KeyboardCode dummy_keycode;
  std::ignore = ui::DomCodeToUsLayoutDomKey(dom_code, ui_flags, &dom_key, &dummy_keycode);

  // Characters this key produces under the real layout.  Empty means either a
  // non-character key (arrows, F-keys) or a dead key still awaiting its base
  // letter — both correctly insert nothing.
  std::u16string layout_text;

#ifdef MOUSEMUX_KEYBOARD_LAYOUT
  // DomCode stays as derived above.  It describes a PHYSICAL key position,
  // which does not change with layout, and deriving it from the vkey through
  // the US table is what Chromium itself does for synthetic events.  Only the
  // CHARACTER is layout-dependent, so only the character is re-derived here.
  if (is_down) {
    DeviceState& kb = StateFor(hwid);
    // The thread's current layout.  One layout for everyone, which is right
    // when the users share a machine and a keyboard type; per-device layouts
    // would need the operator to say which is which.
    const HKL layout = ::GetKeyboardLayout(0);
    const std::array<uint8_t, 256> key_state = BuildKeyState(kb.pressed_keys);

    ClearLayoutDeadKeyState(layout);

    // Re-feed this device's pending accent so the layout composes against the
    // right one — not against whatever another user last pressed.
    if (kb.pending_dead_vkey) {
      wchar_t discard[8];
      ::ToUnicodeEx(kb.pending_dead_vkey, kb.pending_dead_scan,
                    kb.pending_dead_key_state.data(), discard,
                    std::size(discard), 0, layout);
    }

    // std::array, not a raw array: Chromium 151 applies -Wunsafe-buffer-usage
    // here, and subscripting a raw array trips it even where the index is
    // provably within what ToUnicodeEx reported.
    std::array<wchar_t, 8> chars{};
    const int rc =
        ::ToUnicodeEx(vkey, scan, key_state.data(), chars.data(),
                      static_cast<int>(chars.size()), 0, layout);

    if (rc < 0) {
      // A dead key: remember it and produce nothing yet.  The accent appears
      // when the base letter follows.
      kb.pending_dead_vkey = vkey;
      kb.pending_dead_scan = scan;
      kb.pending_dead_key_state = key_state;
      ClearLayoutDeadKeyState(layout);
    } else {
      kb.pending_dead_vkey = 0;
      for (int i = 0; i < rc; ++i) {
        layout_text.push_back(static_cast<char16_t>(chars[static_cast<size_t>(i)]));
      }
      // Report what was actually produced rather than the US guess, so JS
      // sees the character the user typed.
      if (!layout_text.empty()) {
        dom_key = ui::DomKey::FromCharacter(layout_text[0]);
      }
    }
  }
#endif  // MOUSEMUX_KEYBOARD_LAYOUT

  // On Windows, the normal keyboard flow is:
  //   1. WM_KEYDOWN → aura → OnKeyEvent → ForwardKeyboardEvent(kRawKeyDown)
  //   2. WM_CHAR    → InsertChar → ForwardKeyboardEvent(kChar) → text insertion
  // Text insertion happens ONLY via InsertChar, NOT via ForwardKeyboardEvent.
  //
  // For SDK injection, we replicate both steps using the same APIs:
  //   1. Send kRawKeyDown via ForwardKeyboardEvent (fires JS keydown)
  //   2. Call view->InsertChar() with a synthetic ui::KeyEvent (text insertion)
  // Native WM_CHAR messages are blocked in InsertChar (they check a flag
  // that is NOT set for SDK-injected events which use kFromDebugger).

  blink::WebInputEvent::Type type = is_down
      ? blink::WebInputEvent::Type::kRawKeyDown
      : blink::WebInputEvent::Type::kKeyUp;

  blink::WebKeyboardEvent event(type, modifiers, base::TimeTicks::Now());
  event.windows_key_code = vkey;
  event.native_key_code = vkey;
  event.dom_code = static_cast<int>(dom_code);
  event.dom_key = static_cast<int>(dom_key);

#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf(
      ">>> INJECT KEY %s: vkey=0x%x mods=0x%x dom_code=%d dom_key=0x%x "
      "native_key=%d has_focus=%d",
      is_down ? "DOWN" : "UP", vkey, modifiers,
      static_cast<int>(event.dom_code),
      event.dom_key,
      event.native_key_code,
      view->HasFocus()));
#endif

  // Forward the kRawKeyDown/kKeyUp event (fires JS keydown/keyup).
  input::NativeWebKeyboardEvent native_event(event, gfx::NativeView());
  native_event.skip_if_unhandled = true;
  host->ForwardKeyboardEvent(native_event);

  // For character keys on keydown, call InsertChar to trigger text insertion.
  // This is the exact same path that native WM_CHAR uses. We create a
  // ui::KeyEvent with the character and call view->InsertChar() directly,
  // bypassing the native_keyboard_input_blocked_ check (which only blocks
  // events NOT marked with kFromDebugger).
  if (is_down) {
    int char_flags = ui::EF_IS_SYNTHESIZED;
    if (modifiers & blink::WebInputEvent::kShiftKey)
      char_flags |= ui::EF_SHIFT_DOWN;
    if (modifiers & blink::WebInputEvent::kControlKey)
      char_flags |= ui::EF_CONTROL_DOWN;
    if (modifiers & blink::WebInputEvent::kAltKey)
      char_flags |= ui::EF_ALT_DOWN;

    // What to insert.  The layout may produce more than one character from a
    // single key — a composed accent, or a ligature — so this is a string,
    // not a char.
    std::u16string text;
#ifdef MOUSEMUX_KEYBOARD_LAYOUT
    text = layout_text;
#else
    if (dom_key.IsCharacter()) {
      text.push_back(static_cast<char16_t>(dom_key.ToCharacter()));
    }
#endif

    for (char16_t ch : text) {
      ui::KeyEvent char_event = ui::KeyEvent::FromCharacter(
          ch, static_cast<ui::KeyboardCode>(vkey), dom_code, char_flags);

#ifdef MOUSEMUX_DEBUG
      LogDebug(base::StringPrintf(
          ">>> INJECT CHAR via InsertChar: vkey=0x%x U+%04X",
          vkey, static_cast<unsigned>(ch)));
#endif

      view->InsertChar(char_event);
    }
  }
}

}  // namespace content
