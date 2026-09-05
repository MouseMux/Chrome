// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"

#include <windows.h>
#include <fstream>
#include <tuple>
#include <algorithm>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/utf_string_conversions.h"
#include "base/auto_reset.h"
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
#include "base/task/single_thread_task_runner.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/render_frame_host.h"
#include "third_party/blink/public/mojom/input/input_event_result.mojom.h"
#include "ui/aura/client/screen_position_client.h"
#include "ui/aura/env.h"
#include "ui/display/win/screen_win.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "content/public/common/widget_type.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_utils.h"
#include "ui/events/types/scroll_types.h"
#include "ui/latency/latency_info.h"

namespace content {

// Defined in desktop_window_tree_host_win.cc, set by the dialog.  The
// dialog and the help window belong to nobody: every user must be able to
// click them, whatever window they own.
extern HWND g_mousemux_dialog_hwnd;
extern HWND g_mousemux_help_hwnd;

// Defined in desktop_window_tree_host_win.cc.  While true, the views side
// treats focus changes as ours: Activate() and ClearNativeFocus() do
// nothing, so giving a page focus does not activate its OS window.
extern bool g_mousemux_synthetic_key;

// Defined in ui/display/win/screen_win.cc; see the comment there.  The
// injected pointer position Chrome reports as "the mouse", per window.
void MouseMuxVirtualCursorMoved(HWND window, int x, int y);
void MouseMuxVirtualCursorForget(HWND window);
void MouseMuxVirtualCursorsClear();

// One rounding for every SDK position: floor.  Truncation rounds toward
// zero, so on a monitor left of or above the primary (negative coordinates)
// a point half a pixel from an edge landed on different sides of it in
// different tests.
gfx::Point ScreenPixelPoint(float x, float y) {
  return gfx::ToFlooredPoint(gfx::PointF(x, y));
}
POINT ScreenPixelPOINT(float x, float y) {
  const gfx::Point p = ScreenPixelPoint(x, y);
  return {p.x(), p.y()};
}

// A MouseMux screen position (physical pixels, Windows' virtual screen) in
// Chrome's scaled screen coordinates - the space GetViewBounds() reports.
//
// Not "pixels / scale".  Chrome lays monitors out edge to edge in scaled
// units, so a monitor whose scaling differs from the primary's has a scaled
// origin that is not its pixel origin divided by anything.  Dividing put a
// pointer on a 100% monitor next to a 125% primary a few hundred units to
// the right of where Chrome had it: the page hovered a menu the pointer was
// nowhere near (customer screenshot, 2026-09-03), and the wheel scrolled
// whatever sat at the shifted spot.  Right with one monitor, or with all
// monitors at one scaling - which is why it took a third user on a third
// monitor to show.
static gfx::PointF ScreenPixelsToDIP(float screen_x, float screen_y) {
  return display::win::GetScreenWin()->ScreenToDIPPoint(
      gfx::PointF(screen_x, screen_y));
}


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
  //
  // Two different events arrive here looking identical: a tab closing, and a
  // whole window closing.  They deserve opposite treatment - the first should
  // leave that person working in their window, the second should hand their
  // seat back - and the only thing that tells them apart is whether any web
  // view is left in the window they claimed.
  std::vector<int> orphaned;
  for (auto& [hwid, state] : device_state_) {
    if (state.drag_target_view == view) {
      state.drag_target_view = nullptr;
    }
    if (state.last_wheel_view == view) {
      state.last_wheel_view = nullptr;
    }
    if (state.keyboard_target_view != view) {
      continue;
    }
    state.keyboard_target_view = nullptr;

    if (RenderWidgetHostViewAura* survivor =
            OtherWebViewInWindow(state.claimed_window, view)) {
      // A tab went, the window stayed: keep this user where they are, on
      // whatever is left of it.
      state.keyboard_target_view = survivor;
      continue;
    }
    orphaned.push_back(hwid);
  }

  // Released after the loop: releasing an owner mutates owners_ and notifies
  // the dialog, neither of which is safe while walking device_state_.
  for (int hwid : orphaned) {
    if (!IsOwner(hwid)) {
      continue;
    }
    // The window is closing, but its HWND is still there: Chrome takes the
    // pages down first and the frame after.  Judging the window by IsWindow
    // here would keep the owner on an empty frame, so drop it explicitly and
    // let the prune pick whatever else they own.
    DeviceState& closing = StateFor(hwid);
    std::erase(closing.owned_windows, closing.claimed_window);
    closing.claimed_window = gfx::kNullAcceleratedWidget;
    // Another window of theirs survives: that one becomes current.
    if (PruneOwnedWindows(hwid)) {
      continue;
    }
    LogDebug(base::StringPrintf(
        "Window closed - releasing owner 0x%x", hwid));
    ReleaseOwnerHwid(hwid);
    DeviceState& state = StateFor(hwid);
    state.claimed_window = gfx::kNullAcceleratedWidget;
  }
}

bool MouseMuxInputController::RecoverStuckInputRouter(
    RenderWidgetHostViewAura* view,
    RenderWidgetHostImpl* host,
    bool has_pending) {
  if (!has_pending) {
    pending_since_.erase(view);
    return false;
  }

  const base::TimeTicks now = base::TimeTicks::Now();
  auto [it, inserted] = pending_since_.insert({view, now});
  if (inserted) {
    return true;  // First time we have seen this view waiting.
  }

  // 300ms of un-acked events means the renderer stopped acking - typically
  // after a view transition - and it will not start again on its own.
  const base::TimeDelta waiting = now - it->second;
  if (waiting <= base::Milliseconds(300)) {
    return true;
  }

  DiagLog(base::StringPrintf(
      "*** InputRouter STUCK for %lldms - resetting. view=%p",
      waiting.InMilliseconds(), static_cast<void*>(view)));
  host->ResetInputRouterForInjection();
  pending_since_.erase(it);
  return false;
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

void MouseMuxInputController::SetNoticeCallback(NoticeCallback callback) {
  notice_callback_ = std::move(callback);
}

void MouseMuxInputController::ClearUiCallbacks() {
  notice_callback_.Reset();
  debug_log_callback_.Reset();
  ownership_changed_callback_.Reset();
  connection_changed_callback_.Reset();
  capture_changed_callback_.Reset();
  keyboard_event_callback_.Reset();
  menu_dismiss_callback_.Reset();
  visibility_changed_callback_.Reset();
  diagnostics_callback_.Reset();
  active_view_for_window_callback_.Reset();
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
  LogDebug(base::StringPrintf("CaptureOwnerHwid: captured 0x%x", hwid));
  if (capture_changed_callback_) {
    capture_changed_callback_.Run(IsCaptured());
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
  LogDebug(base::StringPrintf("ReleaseCaptureHwid: released 0x%x", hwid));
  if (capture_changed_callback_) {
    capture_changed_callback_.Run(IsCaptured());
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

// The Win32 caption of a window: what the taskbar shows.  Empty when the
// window is gone.
std::u16string WindowCaptionOf(HWND hwnd) {
  if (!hwnd || !::IsWindow(hwnd)) {
    return std::u16string();
  }
  const int length = ::GetWindowTextLengthW(hwnd);
  if (length <= 0) {
    return std::u16string();
  }
  std::wstring caption(static_cast<size_t>(length) + 1, L'\0');
  const int copied = ::GetWindowTextW(hwnd, caption.data(),
                                      static_cast<int>(caption.size()));
  if (copied <= 0) {
    return std::u16string();
  }
  caption.resize(static_cast<size_t>(copied));
  return base::WideToUTF16(caption);
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
  if (!toplevel) {
    return std::u16string();
  }

  std::u16string title = toplevel->GetTitle();
  if (!title.empty()) {
    return title;
  }

  // Browser windows leave the aura title empty - Chrome puts the page name on
  // the native window instead - so every row read "(untitled window)" and the
  // column that should say which page a user is on said nothing.  The Win32
  // caption is what the taskbar shows, and it is never empty.
  aura::WindowTreeHost* host = toplevel->GetHost();
  if (!host) {
    return title;
  }
  return WindowCaptionOf(host->GetAcceleratedWidget());
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

bool MouseMuxInputController::ShouldSuppressBlur(
    RenderWidgetHostViewAura* view) const {
  if (!view || !registered_views_.count(view)) {
    return false;
  }
  // While someone is captured, or when the view sits in an owned window.
  // Under ownership routing, OS activation carries no information about who
  // is working where - it moves with whichever real mouse last clicked
  // something - and every change of it blurred the deactivated window's page
  // and killed a caret somebody was typing behind (2026-09-04 21:12, nobody
  // captured: thirteen blurs in three minutes).
  return IsCaptured() || OwnerOfWindow(ToplevelWindowOf(view)) != -1;
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

      // Typing activity, for the glyph on the row.  Recent enough to read as
      // "now"; long enough that a pause between words is not silence.
      if (!state_it->second.last_key_time.is_null()) {
        constexpr base::TimeDelta kRecent = base::Milliseconds(1500);
        if (base::TimeTicks::Now() - state_it->second.last_key_time <
            kRecent) {
          info.typing = state_it->second.last_key_dropped ? 2 : 1;
        }
      }

      info.block_native = state_it->second.block_native;

      // The window this user's input goes to - the current one - which is
      // what "where is this user working" means to an operator.  Read from
      // the window itself: the view they last clicked is a tab, and a tab
      // can be gone or in another window while the user is still here.
      const gfx::AcceleratedWidget window = state_it->second.claimed_window;
      if (window && ::IsWindow(window)) {
        info.window = window;
        info.window_title = WindowCaptionOf(window);
        info.has_window = true;
        int alive = 0;
        for (gfx::AcceleratedWidget owned : state_it->second.owned_windows) {
          if (owned && ::IsWindow(owned)) {
            ++alive;
          }
        }
        info.extra_windows = alive > 1 ? alive - 1 : 0;
      }
    }
    out.push_back(std::move(info));
  }
  return out;
}

RenderWidgetHostViewAura* MouseMuxInputController::OtherWebViewInWindow(
    gfx::AcceleratedWidget window,
    RenderWidgetHostViewAura* except) const {
  if (!window || !::IsWindow(window)) {
    return nullptr;
  }
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (!view || view == except) {
      continue;
    }
    if (ToplevelWindowOf(view) == window) {
      return view;
    }
  }
  return nullptr;
}

RenderWidgetHostViewAura* MouseMuxInputController::ActiveWebViewInWindow(
    gfx::AcceleratedWidget window) const {
  if (!window || !::IsWindow(window)) {
    return nullptr;
  }

  // Ask the browser first.  It knows which tab is active; everything below is
  // inference and exists only for windows the browser does not own, or before
  // the dialog has installed the callback.
  if (active_view_for_window_callback_) {
    if (RenderWidgetHostViewAura* active =
            active_view_for_window_callback_.Run(window)) {
      if (registered_views_.count(active)) {
        return active;
      }
    }
  }

  // The LARGEST showing view in the window, not the first one found.
  //
  // "Only one view is showing per window" is false, and assuming it cost a
  // day: every browser window here has a second showing view of 34x34 pixels
  // alongside the page.  registered_views_ is a std::set ordered by POINTER,
  // so which one came first was down to allocation addresses - and it was the
  // small one, so every injected keystroke went into a 34-pixel widget with no
  // page in it.  Nothing looked wrong from outside: the routing was right, the
  // window was right, the keystroke was delivered, and it vanished.
  //
  // Area is the honest discriminator.  A page fills its window; the incidental
  // widgets - popups, dropdowns, whatever these are - are small by nature.  It
  // needs no assumption about how many views a window has.
  RenderWidgetHostViewAura* best = nullptr;
  int best_area = 0;
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (!view || !view->IsShowing() || ToplevelWindowOf(view) != window) {
      continue;
    }
    const gfx::Rect bounds = view->GetViewBounds();
    const int area = bounds.width() * bounds.height();
    if (area > best_area) {
      best_area = area;
      best = view;
    }
  }
  return best;
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

std::string MouseMuxInputController::GetViewInventory() const {
  std::string out;
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (!view) {
      continue;
    }
    const gfx::Rect bounds = view->GetViewBounds();
    // Type and name as well as size: a heuristic based on how big something
    // is deserves to be replaced by one based on what it IS.
    const char* kind =
        view->GetWidgetType() == WidgetType::kPopup ? "popup" : "frame";
    std::string name;
    if (aura::Window* native = view->GetNativeView()) {
      name = native->GetName();
    }
    out += base::StringPrintf(
        " | view %p %s '%s' showing=%d focus=%d childframe=%d win=%p "
        "bounds=%dx%d@%d,%d",
        static_cast<void*>(view), kind, name.c_str(),
        view->IsShowing() ? 1 : 0, view->HasFocus() ? 1 : 0,
        view->IsRenderWidgetHostViewChildFrame() ? 1 : 0,
        static_cast<void*>(ToplevelWindowOf(view)), bounds.width(),
        bounds.height(), bounds.x(), bounds.y());
  }
  return out;
}

std::string MouseMuxInputController::GetInjectionStats() const {
  std::string out;
  for (const auto& [hwid, s] : injection_stats_) {
    out += base::StringPrintf(
        " | inject 0x%x keys=%d chars=%d no_host=%d dead=%d "
        "view_focus=%d page_focus=%d",
        hwid, s.posted_keys, s.posted_chars, s.no_host, s.renderer_dead,
        s.last_view_focused ? 1 : 0, s.last_page_focused ? 1 : 0);
  }
  return out;
}

void MouseMuxInputController::RecordKeyRoute(int keyboard_hwid,
                                             int mouse_hwid,
                                             const std::u16string& title,
                                             bool dropped) {
  // Against the device as well as the list: the row shows activity, the list
  // is for reading the history out of a running browser.
  if (mouse_hwid != -1) {
    DeviceState& state = StateFor(mouse_hwid);
    state.last_key_time = base::TimeTicks::Now();
    state.last_key_dropped = dropped;
  }
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
  if (IsCaptured()) {
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
  if (!IsCaptured()) {
    LogDebug("ReleaseCapture: Not captured");
    return false;
  }
  if (client_) {
    for (int hwid : owners_) {
      if (StateFor(hwid).captured) {
        client_->SendCaptureRelease(hwid);
        StateFor(hwid).captured = false;
      }
    }
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
  owners_.insert(hwid);
  if (owner_hwid_ == -1) {
    owner_hwid_ = hwid;
  }
  // Clear only the incoming owner's button state: another device's held
  // buttons are none of this owner's business.
  StateFor(hwid).button_state = 0;
  NoteSdkButtonState();
  LogDebug(base::StringPrintf("AddOwner: hwid=0x%x (owners now %zu, primary 0x%x)",
                              hwid, owners_.size(), owner_hwid_));
  NotifyOwnershipChanged();
}

void MouseMuxInputController::RemoveOwner(int hwid) {
  if (!IsOwner(hwid)) {
    return;
  }
  owners_.erase(hwid);
  for (gfx::AcceleratedWidget owned : StateFor(hwid).owned_windows) {
    MouseMuxVirtualCursorForget(owned);
  }
  if (owners_.empty()) {
    MouseMuxVirtualCursorsClear();
  }
  StateFor(hwid).button_state = 0;
  NoteSdkButtonState();
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

bool MouseMuxInputController::AssignWindow(const std::string& name,
                                           gfx::AcceleratedWidget window,
                                           bool captured,
                                           bool block_native) {
  if (!window || !::IsWindow(window)) {
    LogDebug("AssignWindow: no such window for '" + name + "'");
    return false;
  }
  int hwid = -1;
  for (const auto& [mouse_hwid, info] : user_info_) {
    if (info.name == name) {
      hwid = mouse_hwid;
      break;
    }
  }
  if (hwid == -1) {
    LogDebug("AssignWindow: user '" + name + "' is not in the user list");
    return false;
  }
  return AssignWindowToHwid(hwid, window, captured, block_native);
}

bool MouseMuxInputController::AssignWindowToHwid(int hwid,
                                                 gfx::AcceleratedWidget window,
                                                 bool captured,
                                                 bool block_native) {
  if (!window || !::IsWindow(window) || hwid == -1) {
    return false;
  }
  const int other = OwnerOfWindow(window);
  if (other != -1 && other != hwid) {
    LogDebug(base::StringPrintf(
        "AssignWindow: window %p is owned by 0x%x, not given to 0x%x",
        static_cast<void*>(window), other, hwid));
    return false;
  }
  AddOwner(hwid);
  if (!ClaimForPress(hwid, window, nullptr)) {
    return false;
  }
  LogDebug(base::StringPrintf("AssignWindow: '%s' (0x%x) -> %p captured=%d block=%d",
                              UserNameOf(hwid).c_str(), hwid,
                              static_cast<void*>(window), captured ? 1 : 0,
                              block_native ? 1 : 0));
  SetOwnerBlockNative(hwid, block_native);
  if (captured) {
    CaptureOwnerHwid(hwid);
  } else {
    ReleaseCaptureHwid(hwid);
  }
  return true;
}

std::vector<gfx::AcceleratedWidget> MouseMuxInputController::KnownWindows()
    const {
  std::vector<gfx::AcceleratedWidget> out;
  for (RenderWidgetHostViewAura* view : registered_views_) {
    const gfx::AcceleratedWidget window = ToplevelWindowOf(view);
    if (window && ::IsWindow(window) &&
        std::find(out.begin(), out.end(), window) == out.end()) {
      out.push_back(window);
    }
  }
  return out;
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
  DeviceState& state = StateFor(hwid);
  state.owned_windows.clear();
  state.claimed_window = gfx::kNullAcceleratedWidget;
  state.keyboard_target_view = nullptr;
}

void MouseMuxInputController::ReleaseOwnership() {
  LogDebug(base::StringPrintf("ReleaseOwnership: hwid=0x%x", owner_hwid_));

  // Release capture first if captured.
  if (IsCaptured()) {
    ReleaseCapture();
  }

  // Releases EVERY owner: this is the dialog's "release" and the control
  // server's owner:null, both of which mean "hand Chrome back", not "drop one
  // of several users".  Per-owner release is RemoveOwner().
  for (int hwid : owners_) {
    DeviceState& state = StateFor(hwid);
    state.button_state = 0;
    state.owned_windows.clear();
    state.claimed_window = gfx::kNullAcceleratedWidget;
    state.keyboard_target_view = nullptr;
    NoteSdkButtonState();
  }
  owners_.clear();
  MouseMuxVirtualCursorsClear();
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
  // Ownership decides which windows drop native input.
  ApplyNativeBlocking();
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

#ifdef MOUSEMUX_NATIVE_BLOCK
  // Defined in desktop_window_tree_host_win.cc inside namespace content —
  // blocks native mouse button messages at the views/aura layer.
  extern bool g_mousemux_native_input_blocked;
  g_mousemux_native_input_blocked = blocked;
#endif

  LogDebug(base::StringPrintf("SetNativeInputBlocked(%s) - %zu views registered",
                               blocked ? "true" : "false",
                               registered_views_.size()));

  ApplyNativeBlocking();
}

void MouseMuxInputController::SetOwnerBlockNative(int hwid, bool block) {
  StateFor(hwid).block_native = block;
  LogDebug(base::StringPrintf("SetOwnerBlockNative: hwid=0x%x block=%d", hwid,
                              block ? 1 : 0));
  NotifyOwnershipChanged();  // applies the blocking
}

void MouseMuxInputController::ApplyNativeBlocking() {
#ifdef MOUSEMUX_NATIVE_BLOCK
  // Both defined in desktop_window_tree_host_win.cc inside namespace content.
  extern bool g_mousemux_native_input_blocked;
  extern std::set<HWND>* g_mousemux_blocked_windows;
  if (!g_mousemux_blocked_windows) {
    g_mousemux_blocked_windows = new std::set<HWND>();
  }
  g_mousemux_blocked_windows->clear();
  for (const auto& entry : device_state_) {
    if (!IsOwner(entry.first) || !entry.second.block_native) {
      continue;
    }
    for (gfx::AcceleratedWidget owned : entry.second.owned_windows) {
      if (owned && ::IsWindow(owned)) {
        g_mousemux_blocked_windows->insert(owned);
      }
    }
  }
  const bool everything = g_mousemux_native_input_blocked;
#else
  const bool everything = native_input_blocked_;
#endif
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (!view || !view->event_handler()) {
      continue;
    }
    bool blocked = everything;
#ifdef MOUSEMUX_NATIVE_BLOCK
    if (!blocked) {
      const gfx::AcceleratedWidget window = ToplevelWindowOf(view);
      blocked = window && g_mousemux_blocked_windows->count(window) > 0;
    }
#endif
    view->event_handler()->SetNativeMouseInputBlocked(blocked);
    view->event_handler()->SetNativeKeyboardInputBlocked(blocked);
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "BLOCKING: view=%p host=%p window=%p blocked=%d",
        static_cast<void*>(view), static_cast<void*>(view->GetRenderWidgetHost()),
        static_cast<void*>(ToplevelWindowOf(view)), blocked ? 1 : 0));
#endif
  }
#if defined(MOUSEMUX_DEBUG) && defined(MOUSEMUX_NATIVE_BLOCK)
  std::string windows;
  for (HWND w : *g_mousemux_blocked_windows) {
    windows += base::StringPrintf(" %p(%s)", static_cast<void*>(w),
                                  OwnerNameOfWindow(w).c_str());
  }
  LogDebug(base::StringPrintf("BLOCKING: global=%d blocked windows:%s",
                              everything ? 1 : 0,
                              windows.empty() ? " none" : windows.c_str()));
#endif
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

void MouseMuxInputController::SetDiagnosticsCallback(
    DiagnosticsCallback callback) {
  diagnostics_callback_ = std::move(callback);
}

void MouseMuxInputController::SetActiveViewForWindowCallback(
    ActiveViewForWindowCallback callback) {
  active_view_for_window_callback_ = std::move(callback);
}

std::string MouseMuxInputController::GetDialogDiagnostics() const {
  return diagnostics_callback_ ? diagnostics_callback_.Run() : std::string();
}

std::string MouseMuxInputController::GetServerVersion() const {
  return client_ ? client_->server_version() : std::string();
}

void MouseMuxInputController::RegisterView(RenderWidgetHostViewAura* view) {
  if (!view)
    return;

  registered_views_.insert(view);
  if (RenderWidgetHost* host = view->GetRenderWidgetHost()) {
    host->AddInputEventObserver(this);
  }
  // Adopts the window if it is new and somebody's, and applies the blocking
  // state to this view either way - now if its window exists, else when the
  // retry finds it.
  MaybeAdoptNewWindow(view, /*retry=*/true);

  LogDebug(base::StringPrintf("RegisterView: now %zu views view=%p host=%p",
                              registered_views_.size(),
                              static_cast<void*>(view),
                              static_cast<void*>(view->GetRenderWidgetHost())));
}

void MouseMuxInputController::UnregisterView(RenderWidgetHostViewAura* view) {
  registered_views_.erase(view);
  if (view) {
    if (RenderWidgetHost* host = view->GetRenderWidgetHost()) {
      host->RemoveInputEventObserver(this);
    }
  }
  // Clear pointers that reference the unregistered view to prevent
  // dangling pointer access.
  ForgetViewEverywhere(view);
  pending_since_.erase(view);
  adopt_pending_.erase(view);
  LogDebug(base::StringPrintf("UnregisterView: now %zu views", registered_views_.size()));
}

int MouseMuxInputController::OwnerOfWindow(
    gfx::AcceleratedWidget window) const {
  if (!window) {
    return -1;
  }
  for (const auto& entry : device_state_) {
    if (!IsOwner(entry.first)) {
      continue;
    }
    for (gfx::AcceleratedWidget owned : entry.second.owned_windows) {
      // A handle whose window is gone owns nothing: Windows reuses handles,
      // and a new window with an old one must not report the old owner.
      if (owned == window && ::IsWindow(owned)) {
        return entry.first;
      }
    }
  }
  return -1;
}

std::string MouseMuxInputController::OwnerNameOfWindow(
    gfx::AcceleratedWidget window) const {
  const int hwid = OwnerOfWindow(window);
  return hwid == -1 ? std::string() : UserNameOf(hwid);
}

void MouseMuxInputController::AdoptWindow(int hwid,
                                          gfx::AcceleratedWidget window) {
  if (!window) {
    return;
  }
  DeviceState& state = StateFor(hwid);
  for (gfx::AcceleratedWidget owned : state.owned_windows) {
    if (owned == window) {
      return;
    }
  }
  state.owned_windows.push_back(window);
  LogDebug(base::StringPrintf("ADOPT: hwid=0x%x user=%s window=%p (now %zu)",
                              hwid, UserNameOf(hwid).c_str(),
                              static_cast<void*>(window),
                              state.owned_windows.size()));
  // The dialog refreshes rows and window captions on this.
  NotifyOwnershipChanged();
}

bool MouseMuxInputController::ClaimForPress(int hwid,
                                            gfx::AcceleratedWidget window,
                                            RenderWidgetHostViewAura* page) {
  if (!window) {
    return false;
  }
  const int other = OwnerOfWindow(window);
  if (other != -1 && other != hwid) {
    return false;
  }
  DeviceState& state = StateFor(hwid);
  if (!page) {
    page = ActiveWebViewInWindow(window);
  }
  if (page) {
    state.keyboard_target_view = page;
  }
  state.claimed_window = window;
  AdoptWindow(hwid, window);
  return true;
}

bool MouseMuxInputController::PruneOwnedWindows(int hwid) {
  DeviceState& state = StateFor(hwid);
  std::vector<gfx::AcceleratedWidget> alive;
  for (gfx::AcceleratedWidget owned : state.owned_windows) {
    if (owned && ::IsWindow(owned)) {
      alive.push_back(owned);
    }
  }
  state.owned_windows = alive;
  if (state.claimed_window && ::IsWindow(state.claimed_window)) {
    return true;
  }
  const gfx::AcceleratedWidget was = state.claimed_window;
  state.claimed_window = gfx::kNullAcceleratedWidget;
  if (alive.empty()) {
    return false;
  }
  state.claimed_window = alive.back();
  if (RenderWidgetHostViewAura* page = ActiveWebViewInWindow(alive.back())) {
    state.keyboard_target_view = page;
  }
  LogDebug(base::StringPrintf(
      "CURRENT: hwid=0x%x window %p closed, now %p", hwid,
      static_cast<void*>(was), static_cast<void*>(alive.back())));
  return true;
}

void MouseMuxInputController::RetryPendingAdoptions() {
  const auto pending = std::move(adopt_pending_);
  adopt_pending_.clear();
  for (RenderWidgetHostViewAura* view : pending) {
    if (registered_views_.count(view)) {
      MaybeAdoptNewWindow(view, /*retry=*/false);
    }
  }
}

void MouseMuxInputController::MaybeAdoptNewWindow(
    RenderWidgetHostViewAura* view,
    bool retry) {
  if (!view || !registered_views_.count(view)) {
    return;
  }
  const gfx::AcceleratedWidget window = ToplevelWindowOf(view);
  if (!window) {
    // The HWND is not there yet; look again shortly.  The view is remembered
    // in a set the unregister path erases from, rather than bound into the
    // task: a pointer held across the delay could be freed and reused by a
    // different view in the meantime.
    if (retry) {
      const bool first = adopt_pending_.empty();
      adopt_pending_.insert(view);
      if (first) {
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&MouseMuxInputController::RetryPendingAdoptions,
                           base::Unretained(this)),
            base::Milliseconds(200));
      }
    }
    return;
  }
  // The view has a window now, which it did not at RegisterView time when
  // the view of a new tab is created before it is attached; the blocking
  // state was computed then with window=(nil) and left the tab open to the
  // real mouse (measured 2026-09-04 20:41: native wheel reaching the
  // second tab of a blocked window).  Recompute now that it is known -
  // here when nothing is adopted below; adoption recomputes on its own.
  if (OwnerOfWindow(window) != -1) {
    ApplyNativeBlocking();
    return;
  }
  int owner = -1;
  const char* reason = "";
  // 1. The window that opened this one has an owner: it is theirs.  Login
  //    popups, links into a new window, print windows.
  if (RenderWidgetHostImpl* host =
          RenderWidgetHostImpl::From(view->GetRenderWidgetHost())) {
    if (WebContentsImpl* contents =
            WebContentsImpl::FromRenderWidgetHostImpl(host)) {
      if (RenderFrameHost* opener = contents->GetOpener()) {
        if (RenderWidgetHostView* opener_view = opener->GetView()) {
          owner = OwnerOfWindow(ToplevelWindowOf(
              static_cast<RenderWidgetHostViewAura*>(opener_view)));
          reason = "opener";
        }
      }
    }
  }
  // 2. No opener (Ctrl+N, the menu): the user whose input arrived last, and
  //    recently, made it.
  if (owner == -1) {
    const base::TimeTicks now = base::TimeTicks::Now();
    base::TimeTicks best;
    for (const auto& entry : device_state_) {
      if (!IsOwner(entry.first) || entry.second.last_input_time.is_null()) {
        continue;
      }
      if (now - entry.second.last_input_time > base::Seconds(2)) {
        continue;
      }
      if (entry.second.last_input_time > best) {
        best = entry.second.last_input_time;
        owner = entry.first;
        reason = "recent-input";
      }
    }
  }
  if (owner == -1) {
    LogDebug(base::StringPrintf("ADOPT: window %p is nobody's (no opener, "
                                "no recent input)",
                                static_cast<void*>(window)));
    ApplyNativeBlocking();
    return;
  }
  LogDebug(base::StringPrintf("ADOPT: window %p -> hwid=0x%x (%s)",
                              static_cast<void*>(window), owner, reason));
  AdoptWindow(owner, window);
}

std::string MouseMuxInputController::UserNameOf(int hwid) const {
  for (const auto& entry : user_info_) {
    if (entry.second.hwid_mouse == hwid || entry.second.hwid_keyboard == hwid) {
      return entry.second.name.empty() ? "(unnamed)" : entry.second.name;
    }
  }
  return "?";
}

#ifdef MOUSEMUX_DEBUG
namespace {

// Every 50th mouse move per key, so the hot path stays cheap and the log
// stays readable while still showing that moves flow.
bool EveryFiftieth(std::map<intptr_t, int>& counters, intptr_t key) {
  return (++counters[key] % 50) == 1;
}
std::map<intptr_t, int>& MoveLogCounters() {
  static base::NoDestructor<std::map<intptr_t, int>> counters;
  return *counters;
}

const char* AckStateName(blink::mojom::InputEventResultState state) {
  switch (state) {
    case blink::mojom::InputEventResultState::kConsumed:
      return "consumed";
    case blink::mojom::InputEventResultState::kNotConsumed:
      return "not-consumed";
    case blink::mojom::InputEventResultState::kNoConsumerExists:
      return "NO-CONSUMER";
    case blink::mojom::InputEventResultState::kIgnored:
      return "IGNORED";
    case blink::mojom::InputEventResultState::kSetNonBlocking:
      return "non-blocking";
    case blink::mojom::InputEventResultState::kUnknown:
      return "unknown";
    default:
      return "other";
  }
}

const char* AckSourceName(blink::mojom::InputEventResultSource source) {
  switch (source) {
    case blink::mojom::InputEventResultSource::kCompositorThread:
      return "compositor";
    case blink::mojom::InputEventResultSource::kMainThread:
      return "main-thread";
    case blink::mojom::InputEventResultSource::kBrowser:
      return "browser";
    default:
      return "unknown";
  }
}

// Cross-process frames in the page this host belongs to: input handed to
// the main frame's host cannot reach content rendered by another process.
int CrossProcessFrameCount(RenderWidgetHostImpl* host) {
  WebContentsImpl* contents = WebContentsImpl::FromRenderWidgetHostImpl(host);
  if (!contents) {
    return -1;
  }
  int count = 0;
  contents->ForEachRenderFrameHost([&count](RenderFrameHost* frame) {
    if (frame->IsCrossProcessSubframe()) {
      ++count;
    }
  });
  return count;
}

}  // namespace
#endif  // MOUSEMUX_DEBUG

void MouseMuxInputController::OnInputEvent(const RenderWidgetHost& host,
                                           const blink::WebInputEvent& event,
                                           InputEventSource /*source*/) {
#ifdef MOUSEMUX_DEBUG
  extern bool g_mousemux_in_custom_dispatch;
  const blink::WebInputEvent::Type type = event.GetType();
  if (type == blink::WebInputEvent::Type::kMouseMove &&
      !EveryFiftieth(MoveLogCounters(),
                     reinterpret_cast<intptr_t>(&host) ^ 0x1)) {
    return;
  }
  const char* origin =
      (event.GetModifiers() & blink::WebInputEvent::kFromDebugger)
          ? "injected-page"
          : (g_mousemux_in_custom_dispatch ? "injected-ui" : "NATIVE");
  std::string where;
  if (blink::WebInputEvent::IsMouseEventType(type) ||
      type == blink::WebInputEvent::Type::kMouseWheel) {
    const auto& mouse = static_cast<const blink::WebMouseEvent&>(event);
    where = base::StringPrintf(" widget=(%.1f,%.1f)",
                               mouse.PositionInWidget().x(),
                               mouse.PositionInWidget().y());
  }
  LogDebug(base::StringPrintf("FWD %s: %s host=%p%s",
                              blink::WebInputEvent::GetName(type), origin,
                              static_cast<const void*>(&host), where.c_str()));
#endif
}

void MouseMuxInputController::OnInputEventAck(
    const RenderWidgetHost& host,
    blink::mojom::InputEventResultSource source,
    blink::mojom::InputEventResultState state,
    const blink::WebInputEvent& event) {
#ifdef MOUSEMUX_DEBUG
  const blink::WebInputEvent::Type type = event.GetType();
  const bool ours =
      (event.GetModifiers() & blink::WebInputEvent::kFromDebugger) != 0;
  if (type == blink::WebInputEvent::Type::kMouseMove &&
      !EveryFiftieth(MoveLogCounters(), reinterpret_cast<intptr_t>(&host))) {
    return;
  }
  std::string where;
  if (blink::WebInputEvent::IsMouseEventType(type)) {
    const auto& mouse = static_cast<const blink::WebMouseEvent&>(event);
    where = base::StringPrintf(" widget=(%.1f,%.1f)", mouse.PositionInWidget().x(),
                               mouse.PositionInWidget().y());
  } else if (type == blink::WebInputEvent::Type::kMouseWheel) {
    const auto& wheel = static_cast<const blink::WebMouseWheelEvent&>(event);
    where = base::StringPrintf(" widget=(%.1f,%.1f) delta=(%.1f,%.1f) phase=%d",
                               wheel.PositionInWidget().x(),
                               wheel.PositionInWidget().y(), wheel.delta_x,
                               wheel.delta_y, static_cast<int>(wheel.phase));
  }
  LogDebug(base::StringPrintf(
      "ACK %s: %s via %s host=%p ours=%d%s",
      blink::WebInputEvent::GetName(type), AckStateName(state),
      AckSourceName(source), static_cast<const void*>(&host), ours ? 1 : 0,
      where.c_str()));
#endif
}

void MouseMuxInputController::OnMouseMotion(int hwid, float x, float y) {
  // MouseMuxClient dispatches observers on its own sequence, which is the UI
  // thread — it asserts that itself with DCHECK_CALLED_ON_VALID_SEQUENCE.
  // Assert the invariant rather than silently reposting: a repost here would
  // reorder input events relative to each other, which is far worse than a
  // loud failure.
  DCHECK_CURRENTLY_ON(BrowserThread::UI);


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
    //
    // Only if the timer is not already pending.  Start() on a running
    // OneShotTimer RESTARTS it, and one timer serves every device, so a second
    // user moving their mouse pushed the deadline back indefinitely: the first
    // user's cursor came to rest and its final position was never delivered
    // while anybody else was still moving.  Hover is exactly the thing that
    // needs the resting position, which is why menus opened sometimes and not
    // others, and only with more than one person working.
    if (!motion_flush_timer_.IsRunning()) {
      motion_flush_timer_.Start(
          FROM_HERE, kMinMotionInterval,
          base::BindOnce(&MouseMuxInputController::FlushPendingMotion,
                         base::Unretained(this)));
    }
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
    // Not restarted while pending - see the mouse path for why that starves
    // a resting cursor when more than one device is moving.
    if (!motion_flush_timer_.IsRunning()) {
      motion_flush_timer_.Start(
          FROM_HERE, kMinPenInterval,
          base::BindOnce(&MouseMuxInputController::FlushPendingMotion,
                         base::Unretained(this)));
    }
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


  // Check if this is a click that should claim ownership.
  // Only left-down claims ownership.
  // Any device that is not yet an owner claims by clicking, so each user
  // joins by clicking their own window.
  const bool may_claim = !IsOwner(hwid);
  if (may_claim && (data & kLeftDown)) {
    MMTRACE("CTRL/Claim", "attempting claim hwid=%d at (%.0f,%.0f) views=%zu",
            hwid, x, y, registered_views_.size());
    if (registered_views_.empty()) {
      MMTRACE("CTRL/Claim", "DROPPED: no views registered");
      LogDebug("BTN IGNORED: No views registered - cannot claim ownership");
      return;
    }

    // Same rule as the routing below: the window Windows would give a real
    // click to.  A window of ours that nobody owns goes to this user; one
    // that someone else owns, the desktop, another program, or our own
    // dialog claims nothing.  The old rectangle test claimed on a miss too,
    // which made an owner with no window who then hovered everyone's pages.
    const POINT pt = ScreenPixelPOINT(x, y);
    HWND under = ::WindowFromPoint(pt);
    if (under) {
      under = ::GetAncestor(under, GA_ROOT);
    }
    if (!under || !WebViewInWindow(under)) {
      LogDebug(base::StringPrintf(
          "CLAIM IGNORED: hwid=0x%x pressed outside any Chrome window (%p)",
          hwid, static_cast<void*>(under)));
      return;
    }
    const int other = OwnerOfWindow(under);
    if (other != -1 && other != hwid) {
      LogDebug(base::StringPrintf(
          "CLAIM REFUSED: hwid=0x%x clicked a window owned by 0x%x", hwid,
          other));
      return;
    }
    AddOwner(hwid);
    LogDebug(base::StringPrintf("OWNER SET: hwid=0x%x window %p", hwid,
                                static_cast<void*>(under)));
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
    NoteSdkButtonState();
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kLeftUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting LEFT UP");
#endif
    StateFor(hwid).button_state &= ~blink::WebMouseEvent::kLeftButtonDown;
    NoteSdkButtonState();
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kRightDown) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting RIGHT DOWN");
#endif
    StateFor(hwid).button_state |= blink::WebMouseEvent::kRightButtonDown;
    NoteSdkButtonState();
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kRightUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting RIGHT UP");
#endif
    StateFor(hwid).button_state &= ~blink::WebMouseEvent::kRightButtonDown;
    NoteSdkButtonState();
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kMiddleDown) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting MIDDLE DOWN");
#endif
    StateFor(hwid).button_state |= blink::WebMouseEvent::kMiddleButtonDown;
    NoteSdkButtonState();
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
  if (data & kMiddleUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting MIDDLE UP");
#endif
    StateFor(hwid).button_state &= ~blink::WebMouseEvent::kMiddleButtonDown;
    NoteSdkButtonState();
    InjectMouseEventToAnyView(hwid, blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
}

void MouseMuxInputController::OnMouseWheel(int hwid, float x, float y, int delta, bool horizontal) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);


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
  LogDebug(base::StringPrintf(
      "IN wheel: hwid=0x%x user=%s px=(%.0f,%.0f) delta=%d horizontal=%d "
      "owned=%p",
      hwid, UserNameOf(hwid).c_str(), x, y, delta, horizontal ? 1 : 0,
      static_cast<void*>(StateFor(hwid).claimed_window)));
#endif

  // The owned window's page, wherever the pointer is - the same rule as
  // clicks (2026-09-04).  Position picks a page only for a user who owns
  // nothing yet.
  RenderWidgetHostViewAura* view = nullptr;
  {
    DeviceState& state = StateFor(hwid);
    state.last_input_time = base::TimeTicks::Now();
    if (state.claimed_window && ::IsWindow(state.claimed_window)) {
      view = ActiveWebViewInWindow(state.claimed_window);
    }
  }
  if (!view) {
    view = FindViewAtPoint(x, y);
    // ...but never another user's page: without a window of their own the
    // wheel has nothing to scroll, and the old fallback to the first
    // registered view scrolled whichever user that happened to be.
    if (view) {
      const int other = OwnerOfWindow(ToplevelWindowOf(view));
      if (other != -1 && other != hwid) {
        view = nullptr;
      }
    }
  }
  if (view) {
    InjectWheelEvent(hwid, view, x, y, delta, horizontal);
  } else {
    LogDebug("WHEEL DROPPED: no page of this user's under the pointer");
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
    MouseMuxVirtualCursorsClear();
    owner_hwid_ = -1;
    // Drops every device's buttons, held keys, targets and pending motion:
    // after a reconnect none of it can be trusted to match the hardware.
    device_state_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
#ifdef MOUSEMUX_PEN_TOUCH_INJECT
    pen_state_.clear();
#endif
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
    MouseMuxVirtualCursorsClear();
    owner_hwid_ = -1;
    device_state_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
#ifdef MOUSEMUX_PEN_TOUCH_INJECT
    pen_state_.clear();
#endif
    pending_since_.clear();
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

  // Drop just this device's ownership.  Unplugging one user's mouse must not
  // evict the others; RemoveOwner promotes a new primary if this was it.
  // Before the state goes: RemoveOwner touches it through StateFor, which
  // would re-create an entry erased a line earlier.
  if (IsOwner(hwid_mouse)) {
    LogDebug("OWNER DISPOSED - removing from owners");
    RemoveOwner(hwid_mouse);
  }

  // A disposed device's state goes with it, whether or not it was the owner —
  // the hardware is gone, so its held buttons and keys are stale, and leaving
  // them would resurrect on an hwid reuse.
  device_state_.erase(hwid_mouse);


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
      hwid, MouseMuxLogVkey(vkey), message, MouseMuxLogScan(vkey, scan),
      flags));
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
    // The pairing may simply be stale.  The server sends the user list once,
    // at connect, and said nothing when a keyboard was moved from one user
    // to another during the session (2026-09-05: a keyboard re-mapped to
    // Aqua kept arriving as Lime's, 128 keys dropped).  Ask again, at most
    // every two seconds while this keeps happening; the reply refreshes the
    // pairing and the next key routes.
    const base::TimeTicks now = base::TimeTicks::Now();
    if (client_ && now - last_user_list_request_ > base::Seconds(2)) {
      last_user_list_request_ = now;
      LogDebug("KEY NOT AN OWNER: re-requesting the user list");
      client_->RequestUserList();
    }
  }

#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf(
      "KEY RECV: kb_hwid=0x%x vkey=0x%x owner=0x%x",
      hwid, MouseMuxLogVkey(vkey), owner_hwid_));
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
          hwid, MouseMuxLogVkey(vkey), owner_hwid_));
#endif
    } else {
      pressed_keys.insert(vkey);
#ifdef MOUSEMUX_DEBUG
      LogDebug(base::StringPrintf(
          "KEY ACCEPT DOWN: kb=0x%x vkey=0x%x scan=%d owner=0x%x views=%zu",
          hwid, MouseMuxLogVkey(vkey), MouseMuxLogScan(vkey, scan),
          owner_hwid_,
          registered_views_.size()));
#endif
    }
  } else {
    pressed_keys.erase(vkey);
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "KEY ACCEPT UP: kb=0x%x vkey=0x%x scan=%d owner=0x%x",
        hwid, MouseMuxLogVkey(vkey), MouseMuxLogScan(vkey, scan),
        owner_hwid_));
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
        MouseMuxLogVkey(vkey), sdk_shift, sdk_ctrl, sdk_alt, win32_shift,
        win32_ctrl, win32_alt));
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

  // The window this user claimed decides where their typing goes, and the
  // active tab inside it is looked up fresh every time.
  //
  // It used to be the view they clicked in, which is a TAB.  Sites that open
  // the next part of themselves in a new tab - which is ordinary behaviour,
  // not an edge case - gave that user a tab their keyboard had never heard
  // of, and it stopped answering them until they clicked in it. Switching
  // tabs by hand had the same effect.
  if (RenderWidgetHostViewAura* active =
          ActiveWebViewInWindow(kb_state.claimed_window)) {
    view = active;
    // Keep the fallback current, for the paths that still read it.
    kb_state.keyboard_target_view = active;
  } else if (kb_state.keyboard_target_view &&
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
    // Single user: the largest showing view, for the same reason as
    // ActiveWebViewInWindow - "the first showing view" can be a 34x34 widget
    // that swallows every keystroke.
    int best_area = 0;
    for (RenderWidgetHostViewAura* v : registered_views_) {
      if (!v || !v->IsShowing()) {
        continue;
      }
      const gfx::Rect b = v->GetViewBounds();
      const int area = b.width() * b.height();
      if (area > best_area) {
        best_area = area;
        view = v;
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
  if (notice_callback_) {
    notice_callback_.Run(
        base::StringPrintf("The MouseMux server will time out in %d %s.",
                           minutes, minutes == 1 ? "minute" : "minutes"),
        /*error=*/false);
  }
}

void MouseMuxInputController::OnTimeoutStopped(const std::string& reason) {
  LogDebug("Session stopped: " + reason);
  if (notice_callback_) {
    notice_callback_.Run("The MouseMux session has ended: " + reason,
                         /*error=*/true);
  }
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

  // Chrome's scaled screen coordinates, per monitor (see ScreenPixelsToDIP).
  const gfx::PointF dip = ScreenPixelsToDIP(screen_x, screen_y);
  const float dip_x = dip.x();
  const float dip_y = dip.y();

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
    menu_dismiss_callback_.Run(hwid, /*page_press=*/true);
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
    // Renderer page focus for the clicked page, and only that.  Other
    // users' pages are not touched: their focus is held by ShouldSuppressBlur
    // while their window is owned, and a focus pushed at them from here put
    // a caret back in a page whose user was typing in the omnibox.
    host->Focus();
    // And Chrome's own focus.  A real click reaches the page through aura,
    // which focuses the page's window on the way; an injected click goes
    // straight to the renderer and skips that.  Where the window's focus had
    // gone elsewhere - the omnibox, or nowhere: Chrome restores focus after
    // a tab closes only when the window is active, and an owned window never
    // is - keys posted to the window then found no focused view and were
    // dropped (2026-09-05 15:11, 114 keys posted, none forwarded).  With
    // the synthetic flag set, the views side does not activate the OS
    // window for this, so nobody else's window is blurred.
    {
      base::AutoReset<bool> synthetic(&g_mousemux_synthetic_key, true);
      view->Focus();
    }
  }

  // Get device scale factor for coordinate transformation.
  float device_scale = view->GetDeviceScaleFactor();

  // Chrome's scaled screen coordinates, per monitor (see ScreenPixelsToDIP).
  const gfx::PointF dip_screen = ScreenPixelsToDIP(screen_x, screen_y);
  float dip_screen_x = dip_screen.x();
  float dip_screen_y = dip_screen.y();

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
  // Every button event; moves every 50th per view.
  if (type != blink::WebInputEvent::Type::kMouseMove ||
      EveryFiftieth(MoveLogCounters(), reinterpret_cast<intptr_t>(view))) {
    const char* type_str = (type == blink::WebInputEvent::Type::kMouseDown)
                               ? "DOWN"
                               : (type == blink::WebInputEvent::Type::kMouseUp)
                                     ? "UP"
                                     : "MOVE";
    const char* btn_str = "?";
    if (event.button == blink::WebPointerProperties::Button::kLeft) btn_str = "LEFT";
    else if (event.button == blink::WebPointerProperties::Button::kRight) btn_str = "RIGHT";
    else if (event.button == blink::WebPointerProperties::Button::kMiddle) btn_str = "MIDDLE";
    else if (event.button == blink::WebPointerProperties::Button::kNoButton) btn_str = "NONE";

    LogDebug(base::StringPrintf(
        "OUT mouse %s %s: hwid=0x%x user=%s view=%p host=%p px=(%.0f,%.0f) "
        "dip=(%.1f,%.1f) scale=%.2f bounds=(%d,%d,%dx%d) widget=(%.1f,%.1f) "
        "inside=%d mods=0x%x click=%d focused=%d active=%d ready=%d "
        "pending=%d ignoring=%d%s",
        type_str, btn_str, hwid, UserNameOf(hwid).c_str(),
        static_cast<void*>(view), static_cast<void*>(host), screen_x, screen_y,
        dip_screen_x, dip_screen_y, device_scale, view_bounds.x(),
        view_bounds.y(), view_bounds.width(), view_bounds.height(), widget_x,
        widget_y,
        (widget_x >= 0 && widget_y >= 0 && widget_x < view_bounds.width() &&
         widget_y < view_bounds.height()) ? 1 : 0,
        event.GetModifiers(), event.click_count, host->is_focused() ? 1 : 0,
        host->is_active() ? 1 : 0, host->GetProcess()->IsReady() ? 1 : 0,
        host->input_router()->HasPendingEvents() ? 1 : 0,
        host->IsIgnoringWebInputEvents(event) ? 1 : 0,
        type == blink::WebInputEvent::Type::kMouseMove
            ? ""
            : base::StringPrintf(" cross_process_frames=%d",
                                 CrossProcessFrameCount(host))
                  .c_str()));
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
  RecoverStuckInputRouter(view, host,
                          host->input_router()->HasPendingEvents());

  // Diagnostic: check if the host will silently drop this event.
  bool is_ignoring = host->IsIgnoringWebInputEvents(event);

  // Log diagnostics for button events always, motion every 120th (~2s at 60fps).
  static int diag_motion_count = 0;
  bool should_log_diag = (type == blink::WebInputEvent::Type::kMouseDown ||
                          type == blink::WebInputEvent::Type::kMouseUp ||
                          (type == blink::WebInputEvent::Type::kMouseMove &&
                           ++diag_motion_count % 120 == 0));
  if (should_log_diag) {
    DiagLog(base::StringPrintf(
        "DIAG MOUSE: ignoring=%d pending=%d views=%zu view=%p",
        is_ignoring, host->input_router()->HasPendingEvents(),
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

// The Chrome UI window the injected pointer last hovered; see
// PrepareUiHoverMove() below.
static HWND g_ui_hover_hwnd = nullptr;

// Whether the page is what is on top at this screen point.  A child widget
// drawn over the page - the find bar, a permission panel - sits above the
// web view in the window's aura tree, and the page's rectangle cannot show
// that.  Ask the tree.
static bool PageIsTopmostAt(RenderWidgetHostViewAura* view,
                            float screen_x,
                            float screen_y) {
  aura::Window* window = view->GetNativeView();
  if (!window) {
    return true;
  }
  aura::Window* root = window->GetRootWindow();
  if (!root || !root->GetHost()) {
    return true;
  }
  // Host-local scaled coordinates, which are root-window coordinates.
  gfx::Point point = ScreenPixelPoint(screen_x, screen_y);
  root->GetHost()->ConvertScreenInPixelsToDIP(&point);
  aura::Window* top = root->GetEventHandlerForPoint(point);
  return !top || window->Contains(top);
}

// Whether the page view contains this screen point (pixels).
static bool PageContainsPoint(RenderWidgetHostViewAura* view,
                              float screen_x,
                              float screen_y) {
  return view->GetViewBounds().Contains(
      gfx::ToFlooredPoint(ScreenPixelsToDIP(screen_x, screen_y)));
}

void MouseMuxInputController::InjectMouseEventToAnyView(
    int hwid,
    blink::WebInputEvent::Type type,
    float screen_x,
    float screen_y,
    int button_flags) {
  RenderWidgetHostViewAura* view = nullptr;
  DeviceState& state = StateFor(hwid);

  // Every press, before routing, so the dialog knows who opened the menu
  // that is about to appear.  Menus open from Chrome-UI presses (the ...
  // button) which never reach the page path, so the attribution there
  // named the last user to press a PAGE - somebody else - and that user's
  // next click closed a menu that was not theirs (2026-09-05 15:30).  A
  // press here never cancels; the page path does that.
  if (type == blink::WebInputEvent::Type::kMouseDown && menu_dismiss_callback_) {
    menu_dismiss_callback_.Run(hwid, /*page_press=*/false);
  }

  // During drag (button held), route all events to THIS device's drag target.
  // Critical for text selection — Chrome's selection handler requires every
  // event in a drag sequence to reach the same view — and per device, or one
  // user's drag would capture another user's movement.
  const bool is_button_event =
      type == blink::WebInputEvent::Type::kMouseDown ||
      type == blink::WebInputEvent::Type::kMouseUp ||
      type == blink::WebInputEvent::Type::kMouseMove;

#ifdef MOUSEMUX_DEBUG
  // ROUTE: which branch this event takes and why.  Moves every 50th per
  // device.
  const bool log_this =
      type != blink::WebInputEvent::Type::kMouseMove ||
      EveryFiftieth(MoveLogCounters(), static_cast<intptr_t>(hwid));
  auto route_log = [&](const char* branch, gfx::AcceleratedWidget target) {
    if (!log_this) {
      return;
    }
    LogDebug(base::StringPrintf(
        "ROUTE %s: hwid=0x%x user=%s type=%d px=(%.0f,%.0f) owned=%p "
        "target=%p buttons=0x%x",
        branch, hwid, UserNameOf(hwid).c_str(), static_cast<int>(type),
        screen_x, screen_y, static_cast<void*>(state.claimed_window),
        static_cast<void*>(target), state.button_state));
  };
#else
  auto route_log = [](const char*, gfx::AcceleratedWidget) {};
#endif
  const bool dragging = state.button_state != 0 && state.drag_target_view &&
                        registered_views_.count(state.drag_target_view);
  state.last_input_time = base::TimeTicks::Now();

  // Ownership routes (2026-09-04).  A user who owns a window sends every
  // event to that window, and position only decides where inside it: one of
  // its own popups, its page, or its Chrome UI.  Nothing else is consulted,
  // so what overlaps what no longer matters - the old search by position
  // put a click on this window's toolbar into the page of the window
  // behind it.  Outside the window, moves and releases still reach the page,
  // as under Windows mouse capture; a press outside has nothing of this
  // user's to press.  The search over all windows survives below only for a
  // user who owns nothing yet.
  // Where the point is, as Windows sees it: the top-level window under it,
  // the one a real click would go to.  One call, and the answer already
  // accounts for what is on top of what - the thing a rectangle test cannot
  // know, and the reason two overlapping windows of one user took two clicks
  // per click (2026-09-04).  A menu popup resolves to the browser window
  // that owns it; our dialog and help window resolve to themselves.
  // Nothing about position is consulted for moves, wheel or keys beyond
  // this; they go to the current window.
  const POINT pt = ScreenPixelPOINT(screen_x, screen_y);
  HWND under = nullptr;
  HWND under_owner = nullptr;
  if (!dragging) {
    under = ::WindowFromPoint(pt);
    if (under) {
      under = ::GetAncestor(under, GA_ROOT);
      under_owner = ::GetAncestor(under, GA_ROOTOWNER);
    }
  }

  // Where "the mouse" is, for code in the window this event is headed for
  // that asks Windows: this pointer.  Keyed by that window, so two users'
  // pointers do not answer for each other's menus and tooltips.
  {
    HWND cursor_window = state.claimed_window;
    if (dragging && state.drag_target_view) {
      cursor_window = ToplevelWindowOf(state.drag_target_view);
    }
    if (under &&
        (under == g_mousemux_dialog_hwnd || under == g_mousemux_help_hwnd)) {
      cursor_window = under;
    }
    if (!cursor_window) {
      cursor_window = under_owner;
    }
    const gfx::Point pixel = ScreenPixelPoint(screen_x, screen_y);
    MouseMuxVirtualCursorMoved(cursor_window, pixel.x(), pixel.y());
  }

  // 1. The MouseMux dialog and its help window: nobody's, everybody's.
  if (is_button_event && !dragging && under &&
      (under == g_mousemux_dialog_hwnd || under == g_mousemux_help_hwnd)) {
    route_log("dialog", under);
    DispatchToAuraHost(under, type, screen_x, screen_y, button_flags);
    return;
  }

  if (state.claimed_window && !::IsWindow(state.claimed_window)) {
    if (!PruneOwnedWindows(hwid)) {
      // Their last window is gone and nothing reported it (the frame died
      // without a page of ours in it).  Same outcome as a reported close:
      // the seat is handed back and this event has nowhere to go.
      LogDebug(base::StringPrintf(
          "Window gone - releasing owner 0x%x", hwid));
      ReleaseOwnerHwid(hwid);
      return;
    }
  }
  if (state.claimed_window && !dragging) {
    // 2. Which of this user's own windows is under the point, if any.  A
    //    popup counts as the window that owns it.
    gfx::AcceleratedWidget mine = gfx::kNullAcceleratedWidget;
    for (gfx::AcceleratedWidget owned : state.owned_windows) {
      if (owned && (owned == under || owned == under_owner)) {
        mine = owned;
        break;
      }
    }

    if (type == blink::WebInputEvent::Type::kMouseDown) {
      if (!mine) {
        // A window nobody owns goes to whoever clicks it first.  Anything
        // else - another user's window, the desktop, some other program -
        // gets nothing.
        if (under && WebViewInWindow(under) &&
            ClaimForPress(hwid, under, nullptr)) {
          mine = under;
          route_log("claim-unowned-window", under);
        } else {
          route_log("DROPPED-press-not-my-window", under);
          MMTRACE("CTRL/Inject", "DROPPED press outside owned windows hwid=%d",
                  hwid);
          return;
        }
      }
      if (mine != state.claimed_window) {
        state.claimed_window = mine;
        if (RenderWidgetHostViewAura* page = ActiveWebViewInWindow(mine)) {
          state.keyboard_target_view = page;
        }
        route_log("switch-current-window", mine);
      }
    }

    const gfx::AcceleratedWidget window = state.claimed_window;
    RenderWidgetHostViewAura* page = ActiveWebViewInWindow(window);
    // Over the current window or one of its popups: hover, popups, page and
    // toolbar all work.  Over anything else, moves and releases still reach
    // the current page, as under Windows capture.
    const bool over_current = mine == window;
#ifdef MOUSEMUX_DEBUG
    if (type == blink::WebInputEvent::Type::kMouseDown) {
      const gfx::Rect b = page ? page->GetViewBounds() : gfx::Rect();
      LogDebug(base::StringPrintf(
          "OWNED ROUTE: under=%p owner=%p mine=%p current=%p page=%p "
          "showing=%d bounds=(%d,%d,%dx%d) scale=%.2f contains=%d topmost=%d",
          static_cast<void*>(under), static_cast<void*>(under_owner),
          static_cast<void*>(mine), static_cast<void*>(window),
          static_cast<void*>(page), page && page->IsShowing() ? 1 : 0, b.x(),
          b.y(), b.width(), b.height(),
          page ? page->GetDeviceScaleFactor() : 0.0f,
          page && PageContainsPoint(page, screen_x, screen_y) ? 1 : 0,
          page && PageIsTopmostAt(page, screen_x, screen_y) ? 1 : 0));
    }
#endif
    if (over_current) {
      if (is_button_event && under != window &&
          TryDispatchToOverlayWindow(type, screen_x, screen_y, button_flags,
                                     window)) {
        route_log("owned-popup", under);
        return;
      }
      if (page && PageContainsPoint(page, screen_x, screen_y) &&
          PageIsTopmostAt(page, screen_x, screen_y)) {
        route_log("owned-page", window);
        view = page;
      } else {
        route_log(page ? "owned-chrome-ui" : "owned-chrome-ui-no-page",
                  window);
        if (is_button_event) {
          DispatchToAuraHost(window, type, screen_x, screen_y, button_flags);
        }
        return;
      }
    } else {
      route_log(page ? "elsewhere-to-owned-page" : "elsewhere-NO-PAGE",
                under);
      view = page;
      if (!view) {
        return;
      }
    }
    if (type == blink::WebInputEvent::Type::kMouseMove && g_ui_hover_hwnd) {
      ::PostMessage(g_ui_hover_hwnd, WM_MOUSEMUX_MOUSELEAVE, 0, 0);
      g_ui_hover_hwnd = nullptr;
    }
    if (type == blink::WebInputEvent::Type::kMouseDown) {
      state.drag_target_view = view;
      state.keyboard_target_view = view;
    } else if (type == blink::WebInputEvent::Type::kMouseUp &&
               state.button_state == 0) {
      state.drag_target_view = nullptr;
    }
    InjectMouseEvent(hwid, view, type, screen_x, screen_y, button_flags);
    return;
  }

  if (dragging) {
    route_log("drag-lock", ToplevelWindowOf(state.drag_target_view));
    view = state.drag_target_view;
  } else {
    route_log("unowned-search", gfx::kNullAcceleratedWidget);
#ifdef MOUSEMUX_AURA_UI_CLICK_THROUGH
    // For button events, check if there's an overlay window (context menu,
    // popup, dropdown) at the coordinates BEFORE checking web content.
    // Overlay windows sit above web content; without this check, clicks on
    // menu items would hit the web content view behind the menu, dismiss
    // the menu via menu_dismiss_callback_, and lose the menu action.
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp ||
        type == blink::WebInputEvent::Type::kMouseMove) {
      if (TryDispatchToOverlayWindow(type, screen_x, screen_y, button_flags)) {
        return;
      }
    }
#endif

    // Normal case: find the view under the cursor.
    view = FindViewAtPoint(screen_x, screen_y);
    // Another user's window is off limits for every event, not just presses:
    // hover from a user who owns nothing must not light up someone else's
    // page.
    if (view) {
      const int other = OwnerOfWindow(ToplevelWindowOf(view));
      if (other != -1 && other != hwid) {
        route_log("REFUSED-window-owned-by-other", ToplevelWindowOf(view));
        return;
      }
    }
    // ...unless something is drawn over the page there.  Then this is a
    // Chrome-UI event and takes the aura path below.
    if (view && !PageIsTopmostAt(view, screen_x, screen_y)) {
      view = nullptr;
    }
  }

  if (!view) {
    MMTRACE("CTRL/Inject", "no web view at (%.0f,%.0f) type=%d - aura fallback",
            screen_x, screen_y, static_cast<int>(type));
#ifdef MOUSEMUX_AURA_UI_CLICK_THROUGH
    // No web content view under the cursor — try dispatching through
    // the aura event system so Chrome UI (tabs, toolbar, etc.) can respond.
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp ||
        type == blink::WebInputEvent::Type::kMouseMove) {
      const gfx::AcceleratedWidget target =
          FindAuraTargetWindow(screen_x, screen_y);
      if (!target) {
        return;
      }
      {
        const int other = OwnerOfWindow(target);
        if (other != -1 && other != hwid) {
          route_log("REFUSED-window-owned-by-other", target);
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
      if (type == blink::WebInputEvent::Type::kMouseDown &&
          WebViewInWindow(target)) {
        ClaimForPress(hwid, target, nullptr);
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

  // Back over page content: end the hover on whatever Chrome UI the pointer
  // was last over, or its highlight sticks.
  if (type == blink::WebInputEvent::Type::kMouseMove && g_ui_hover_hwnd) {
    ::PostMessage(g_ui_hover_hwnd, WM_MOUSEMUX_MOUSELEAVE, 0, 0);
    g_ui_hover_hwnd = nullptr;
  }


  // Track this device's drag target: set on mousedown, clear on mouseup.
  if (type == blink::WebInputEvent::Type::kMouseDown) {
    const gfx::AcceleratedWidget window = ToplevelWindowOf(view);
    if (!ClaimForPress(hwid, window, view)) {
      route_log("REFUSED-window-owned-by-other", window);
      return;
    }
    state.drag_target_view = view;
  } else if (type == blink::WebInputEvent::Type::kMouseUp &&
             state.button_state == 0) {
    state.drag_target_view = nullptr;
  }

  InjectMouseEvent(hwid, view, type, screen_x, screen_y, button_flags);
}

#ifdef MOUSEMUX_AURA_UI_CLICK_THROUGH
// Hover over Chrome UI and menus.  One window at a time: moving to another
// window, or back over page content, first sends the old one a leave so its
// highlight clears.  Throttled to about 60 Hz; hover feedback does not need
// every SDK motion sample, and each one is a PostMessage plus a views hit
// test.

static bool PrepareUiHoverMove(HWND hwnd,
                               int button_flags,
                               UINT* msg,
                               WPARAM* wparam) {
  static base::TimeTicks last;
  const base::TimeTicks now = base::TimeTicks::Now();
  if (hwnd == g_ui_hover_hwnd && now - last < base::Milliseconds(16)) {
    return false;
  }
  last = now;
  if (g_ui_hover_hwnd && g_ui_hover_hwnd != hwnd) {
    ::PostMessage(g_ui_hover_hwnd, WM_MOUSEMUX_MOUSELEAVE, 0, 0);
  }
  g_ui_hover_hwnd = hwnd;
  *msg = WM_MOUSEMUX_MOUSEMOVE;
  *wparam = 0;
  if (button_flags & blink::WebMouseEvent::kLeftButtonDown) {
    *wparam |= MK_LBUTTON;
  }
  if (button_flags & blink::WebMouseEvent::kRightButtonDown) {
    *wparam |= MK_RBUTTON;
  }
  if (button_flags & blink::WebMouseEvent::kMiddleButtonDown) {
    *wparam |= MK_MBUTTON;
  }
  return true;
}

bool MouseMuxInputController::TryDispatchToOverlayWindow(
    blink::WebInputEvent::Type type,
    float screen_x,
    float screen_y,
    int button_flags,
    gfx::AcceleratedWidget owner) {
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

  const gfx::Point screen_pt = ScreenPixelPoint(screen_x, screen_y);

  // Collect all visible hosts at this point, then use Z-order to find topmost.
  const auto& hosts = env->window_tree_hosts();
  std::map<HWND, aura::WindowTreeHost*> candidates;
  for (aura::WindowTreeHost* host : hosts) {
    if (!host) continue;
    HWND hwnd = host->GetAcceleratedWidget();
    if (!hwnd || !::IsWindowVisible(hwnd)) continue;
    // An owned user's events belong to their window and its own popups; a
    // menu open on somebody else's window is not theirs to click.
    if (owner && hwnd != owner && ::GetAncestor(hwnd, GA_ROOTOWNER) != owner) {
      continue;
    }
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
  POINT client_pt = ScreenPixelPOINT(screen_x, screen_y);
  ::ScreenToClient(topmost_hwnd, &client_pt);
  LPARAM lparam = MAKELPARAM(client_pt.x, client_pt.y);

  UINT msg = 0;
  WPARAM wparam = 0;
  if (type == blink::WebInputEvent::Type::kMouseMove) {
    if (!PrepareUiHoverMove(topmost_hwnd, button_flags, &msg, &wparam)) {
      return true;  // Throttled.
    }
  } else if (type == blink::WebInputEvent::Type::kMouseDown) {
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

  if (msg && type != blink::WebInputEvent::Type::kMouseMove) {
    char cls[64] = {};
    ::GetClassNameA(topmost_hwnd, cls, static_cast<int>(sizeof(cls)));
    DiagLog(base::StringPrintf(
        "OVERLAY DISPATCH: msg=0x%x to hwnd=%p class=%s client(%ld,%ld) "
        "fg=%p",
        msg, static_cast<void*>(topmost_hwnd), cls, client_pt.x, client_pt.y,
        static_cast<void*>(::GetForegroundWindow())));
  }
  if (msg) {
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

  const gfx::Point screen_pt = ScreenPixelPoint(screen_x, screen_y);
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
  if (type == blink::WebInputEvent::Type::kMouseMove) {
    if (!PrepareUiHoverMove(target, button_flags, &msg, &wparam)) {
      return;  // Throttled.
    }
  } else if (type == blink::WebInputEvent::Type::kMouseDown) {
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

  POINT client_pt = ScreenPixelPOINT(screen_x, screen_y);
  ::ScreenToClient(target_hwnd, &client_pt);
  LPARAM lparam = MAKELPARAM(client_pt.x, client_pt.y);
#ifdef MOUSEMUX_DEBUG
  if (type != blink::WebInputEvent::Type::kMouseMove) {
    char cls[64] = {};
    ::GetClassNameA(target_hwnd, cls, static_cast<int>(sizeof(cls)));
    LogDebug(base::StringPrintf(
        "AURA DISPATCH: type=%d to hwnd=%p class=%s client(%ld,%ld) fg=%p",
        static_cast<int>(type), static_cast<void*>(target_hwnd), cls,
        client_pt.x, client_pt.y,
        static_cast<void*>(::GetForegroundWindow())));
  }
#endif

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
  // Chrome's scaled screen coordinates, per monitor (see ScreenPixelsToDIP).
  const gfx::PointF dip_screen = ScreenPixelsToDIP(screen_x, screen_y);
  float dip_screen_x = dip_screen.x();
  float dip_screen_y = dip_screen.y();

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

  // The wheel wedges the same way clicks do, and shares the same recovery -
  // the customer report that led here was "the pointer still moves but
  // scrolling stopped", which is one view's router stuck while the OS cursor
  // carries on regardless.
  RecoverStuckInputRouter(view, host,
                          host->input_router()->HasPendingEvents());

  // Check if the host will drop this event.
  bool wheel_ignoring = host->IsIgnoringWebInputEvents(event);
  if (wheel_ignoring) {
    DiagLog("*** DROPPING WHEEL: IsIgnoring=TRUE");
  }

#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf(
      "OUT wheel: hwid=0x%x view=%p host=%p px=(%.0f,%.0f) dip=(%.1f,%.1f) "
      "bounds=(%d,%d,%dx%d) widget=(%.1f,%.1f) inside=%d delta=(%.1f,%.1f) "
      "pending=%d ignoring=%d cross_process_frames=%d",
      hwid, static_cast<void*>(view), static_cast<void*>(host), screen_x,
      screen_y, dip_screen_x, dip_screen_y, view_bounds.x(), view_bounds.y(),
      view_bounds.width(), view_bounds.height(), widget_x, widget_y,
      (widget_x >= 0 && widget_y >= 0 && widget_x < view_bounds.width() &&
       widget_y < view_bounds.height()) ? 1 : 0,
      event.delta_x, event.delta_y,
      host->input_router()->HasPendingEvents() ? 1 : 0, wheel_ignoring ? 1 : 0,
      CrossProcessFrameCount(host)));
#endif
  // Forward the event.
  host->ForwardWheelEventWithLatencyInfo(event, ui::LatencyInfo());

  // The gesture this notch belongs to must END once the notches stop, or
  // Chrome treats every later notch as a continuation of it and keeps
  // scrolling whatever the first one latched onto - nothing, when that
  // first notch landed on a spot with no scroller or at the end of one.
  // Measured 2026-09-04: 206 consecutive wheels answered "no consumer" on a
  // page a real mouse scrolled at once; 447 scroll updates, 3 scroll begins.
  // A real mouse gets its end from MouseWheelPhaseHandler 100 ms after the
  // last notch; this is the same thing for an injected one.
  {
    DeviceState& state = StateFor(hwid);
    state.last_wheel_view = view;
    state.last_wheel_x = screen_x;
    state.last_wheel_y = screen_y;
    std::unique_ptr<base::OneShotTimer>& timer = wheel_end_timers_[hwid];
    if (!timer) {
      timer = std::make_unique<base::OneShotTimer>();
    }
    timer->Start(FROM_HERE, base::Milliseconds(100),
                 base::BindOnce(&MouseMuxInputController::SendWheelEnd,
                                base::Unretained(this), hwid));
  }
}

void MouseMuxInputController::SendWheelEnd(int hwid) {
  DeviceState& state = StateFor(hwid);
  RenderWidgetHostViewAura* view = state.last_wheel_view;
  state.last_wheel_view = nullptr;
  if (!view || !registered_views_.count(view)) {
    return;
  }
  RenderWidgetHostImpl* host =
      RenderWidgetHostImpl::From(view->GetRenderWidgetHost());
  if (!host) {
    return;
  }
  const gfx::PointF dip_screen =
      ScreenPixelsToDIP(state.last_wheel_x, state.last_wheel_y);
  const gfx::Rect view_bounds = view->GetViewBounds();
  blink::WebMouseWheelEvent event(blink::WebInputEvent::Type::kMouseWheel,
                                  blink::WebInputEvent::kFromDebugger |
                                      state.button_state,
                                  base::TimeTicks::Now());
  event.SetPositionInWidget(dip_screen.x() - view_bounds.x(),
                            dip_screen.y() - view_bounds.y());
  event.SetPositionInScreen(dip_screen.x(), dip_screen.y());
  event.delta_x = 0;
  event.delta_y = 0;
  event.wheel_ticks_x = 0;
  event.wheel_ticks_y = 0;
  event.phase = blink::WebMouseWheelEvent::kPhaseEnded;
  event.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
  event.dispatch_type = blink::WebInputEvent::DispatchType::kEventNonBlocking;
#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf("OUT wheel-end: hwid=0x%x view=%p host=%p",
                              hwid, static_cast<void*>(view),
                              static_cast<void*>(host)));
#endif
  host->ForwardWheelEventWithLatencyInfo(event, ui::LatencyInfo());
}

void MouseMuxInputController::NoteSdkButtonState() {
  // Defined in desktop_window_tree_host_win.cc; read by
  // WebContentsViewAura::StartDragging and
  // DesktopDragDropClientWin::StartDragAndDrop.  A drag that begins while an
  // injected mouse holds a button was begun by that mouse, and must not
  // become an OLE drag: DoDragDrop is a modal Win32 loop that ends only on a
  // real button release, which the injected mouse never produces, and the
  // browser process hangs in that loop (field dump, 2026-09-04).
  extern bool g_mousemux_sdk_button_held;
  bool held = false;
  for (const auto& entry : device_state_) {
    if (entry.second.button_state != 0) {
      held = true;
      break;
    }
  }
  g_mousemux_sdk_button_held = held;
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

  InjectionStats& stats = injection_stats_[hwid];
  StateFor(hwid).last_input_time = base::TimeTicks::Now();

  RenderWidgetHostImpl* host = RenderWidgetHostImpl::From(
      view->GetRenderWidgetHost());
  if (!host) {
    ++stats.no_host;
    LogDebug("InjectKeyboardEvent: host is null!");
    return;
  }

  // Recorded rather than acted on: if the renderer is discarding our events,
  // that is the answer, and it is not something this code can force.
  if (!host->IsInitializedAndNotDead()) {
    ++stats.renderer_dead;
  }
  stats.last_view_focused = view->HasFocus();
  stats.last_page_focused = host->is_focused();

  // No focus work here.  Focus is the window's business: PreHandleMSG
  // re-activates the views side of an OS-inactive window before dispatching,
  // which restores the view the user last focused, and that view focuses its
  // own renderer when it is the page.  Forcing page focus on every renderer
  // from here would put a caret back in the page while the user is typing in
  // the omnibox.


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

  // Into the window, where a real keystroke enters.  PreHandleMSG turns these
  // into ui::KeyEvents flagged EF_IS_SYNTHESIZED and sends them where a
  // WM_KEYDOWN goes: the aura dispatcher skips the input method for
  // synthesized events and hands them to the views focus manager, so
  // accelerators fire and the focused view - omnibox, find bar or page -
  // receives them.  The character goes separately, to the focused
  // TextInputClient, because the views layer drops char events on the
  // dispatch path.
  //
  // PostMessage rather than a call: it is what the mouse path does, it keeps
  // the content layer out of ui/views, and one window's messages are handled
  // in order, so a keydown always precedes its characters and never
  // interleaves with another user's.
  const HWND hwnd = ToplevelWindowOf(view);
  if (!hwnd) {
    LogDebug("InjectKeyboardEvent: view has no toplevel window");
    return;
  }
  const WPARAM mods = static_cast<WPARAM>(ui_flags & 0xFFFF) << 16;
  const LPARAM code = static_cast<LPARAM>(static_cast<uint32_t>(dom_code));
  ++stats.posted_keys;
  ::PostMessage(hwnd, is_down ? WM_MOUSEMUX_KEYDOWN : WM_MOUSEMUX_KEYUP,
                mods | static_cast<WPARAM>(vkey & 0xFFFF), code);

  if (is_down) {
    std::u16string text;
#ifdef MOUSEMUX_KEYBOARD_LAYOUT
    text = layout_text;
#else
    if (dom_key.IsCharacter()) {
      text.push_back(static_cast<char16_t>(dom_key.ToCharacter()));
    }
#endif
    const LPARAM char_param =
        code | (static_cast<LPARAM>(vkey & 0xFFFF) << 32);
    for (char16_t ch : text) {
      ++stats.posted_chars;
      ::PostMessage(hwnd, WM_MOUSEMUX_CHAR, mods | ch, char_param);
    }
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "OUT key DOWN: hwid=0x%x user=%s hwnd=%p view=%p vkey=0x%x flags=0x%x "
        "chars=%d",
        hwid, UserNameOf(hwid).c_str(), static_cast<void*>(hwnd),
        static_cast<void*>(view), MouseMuxLogVkey(vkey), ui_flags,
        static_cast<int>(text.size())));
  } else {
    LogDebug(base::StringPrintf(
        "OUT key UP: hwid=0x%x user=%s hwnd=%p view=%p vkey=0x%x", hwid,
        UserNameOf(hwid).c_str(), static_cast<void*>(hwnd),
        static_cast<void*>(view), MouseMuxLogVkey(vkey)));
#endif
  }
}

}  // namespace content
