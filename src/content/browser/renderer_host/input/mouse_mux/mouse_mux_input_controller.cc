// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"

#include <windows.h>
#include <fstream>
#include <tuple>
#include <vector>

#include "base/command_line.h"
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

void MouseMuxInputController::FlushPendingMotion() {
  if (!has_pending_motion_ || owner_hwid_ == -1) {
    return;
  }
  has_pending_motion_ = false;
  last_motion_inject_time_ = base::TimeTicks::Now();
  InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseMove,
                            pending_motion_x_, pending_motion_y_,
                            current_button_state_);
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

bool MouseMuxInputController::CaptureOwner() {
  if (owner_hwid_ == -1) {
    LogDebug("CaptureOwner: No owner to capture");
    return false;
  }
  if (is_captured_) {
    LogDebug("CaptureOwner: Already captured");
    return false;
  }
  if (client_) {
    client_->SendCaptureRequest(owner_hwid_);
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
  if (owner_hwid_ == -1) {
    // Edge case: owner was released but capture state wasn't cleared.
    is_captured_ = false;
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(false);
    }
    return false;
  }
  if (client_) {
    client_->SendCaptureRelease(owner_hwid_);
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

void MouseMuxInputController::SetOwner(int hwid) {
  if (hwid == owner_hwid_) return;
  LogDebug(base::StringPrintf("SetOwner: hwid=0x%x (was 0x%x)", hwid, owner_hwid_));
  owner_hwid_ = hwid;
  current_button_state_ = 0;
  NotifyOwnershipChanged();
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

void MouseMuxInputController::ReleaseOwnership() {
  LogDebug(base::StringPrintf("ReleaseOwnership: hwid=0x%x", owner_hwid_));

  // Release capture first if captured.
  if (is_captured_) {
    ReleaseCapture();
  }

  owner_hwid_ = -1;
  current_button_state_ = 0;
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
  if (drag_target_view_ == view) {
    drag_target_view_ = nullptr;
  }
  if (keyboard_target_view_ == view) {
    keyboard_target_view_ = nullptr;
  }
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
  if (owner_hwid_ == -1) {
    return;
  }

  // Only process events from the owner.
  if (hwid != owner_hwid_) {
    return;
  }

  // Throttle motion injection to max 60fps (16ms between events).
  // This prevents flooding the UI thread with motion events.
  base::TimeTicks now = base::TimeTicks::Now();
  constexpr base::TimeDelta kMinMotionInterval = base::Milliseconds(16);
  if (now - last_motion_inject_time_ < kMinMotionInterval) {
    // Store position for next injection, but don't inject now.
    pending_motion_x_ = x;
    pending_motion_y_ = y;
    has_pending_motion_ = true;
    // Deliver it anyway if no further motion arrives — otherwise the cursor
    // comes to rest at a position the renderer never saw.
    motion_flush_timer_.Start(
        FROM_HERE, kMinMotionInterval,
        base::BindOnce(&MouseMuxInputController::FlushPendingMotion,
                       base::Unretained(this)));
    return;
  }

  // Inject motion event.
  last_motion_inject_time_ = now;
  has_pending_motion_ = false;
  InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseMove, x, y,
                            current_button_state_);
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
  if (owner_hwid_ == -1) {
    MMTRACE("CTRL/PenMotion", "DROPPED: no owner claimed yet (hwid=%d)",
            hwid);
    return;
  }

  // Only process events from the owner.
  if (hwid != owner_hwid_) {
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
  if (now - last_motion_inject_time_ < kMinPenInterval) {
    pending_motion_x_ = x;
    pending_motion_y_ = y;
    has_pending_motion_ = true;
    motion_flush_timer_.Start(
        FROM_HERE, kMinPenInterval,
        base::BindOnce(&MouseMuxInputController::FlushPendingMotion,
                       base::Unretained(this)));
    return;
  }

  last_motion_inject_time_ = now;
  has_pending_motion_ = false;
  InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseMove, x, y,
                            current_button_state_);
}
#endif

void MouseMuxInputController::OnMouseButton(int hwid,
                                            float x,
                                            float y,
                                            int data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Flush any pending motion before button event to ensure accurate position.
  if (has_pending_motion_ && hwid == owner_hwid_) {
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseMove,
                              pending_motion_x_, pending_motion_y_,
                              current_button_state_);
    has_pending_motion_ = false;
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
  if (owner_hwid_ == -1 && (data & kLeftDown)) {
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
      owner_hwid_ = hwid;
      LogDebug(base::StringPrintf("OWNER SET via hit-test: hwid=0x%x", hwid));
      NotifyOwnershipChanged();
    } else {
      // Hit-test failed, but we have views. Log why and try alternative.
      LogDebug("Hit-test failed. Trying coordinate-agnostic ownership claim...");

      // Alternative: Check if we should claim ownership anyway.
      // If the user has enabled MouseMux and is clicking, they probably want it to work.
      // Claim ownership and use the first view.
      owner_hwid_ = hwid;
      LogDebug(base::StringPrintf(
          "OWNER SET via fallback (hit-test failed but views exist): hwid=0x%x", hwid));
      NotifyOwnershipChanged();
    }
  }

  // If no owner, ignore.
  if (owner_hwid_ == -1) {
    MMTRACE("CTRL/Button", "DROPPED: no owner set (hwid=%d data=0x%x)", hwid,
            data);
#ifdef MOUSEMUX_DEBUG
    LogDebug("BTN IGNORED: No owner set");
#endif
    return;
  }

  // Only process events from the owner.
  if (hwid != owner_hwid_) {
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
    current_button_state_ |= blink::WebMouseEvent::kLeftButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kLeftUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting LEFT UP");
#endif
    current_button_state_ &= ~blink::WebMouseEvent::kLeftButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kRightDown) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting RIGHT DOWN");
#endif
    current_button_state_ |= blink::WebMouseEvent::kRightButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kRightUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting RIGHT UP");
#endif
    current_button_state_ &= ~blink::WebMouseEvent::kRightButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kMiddleDown) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting MIDDLE DOWN");
#endif
    current_button_state_ |= blink::WebMouseEvent::kMiddleButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
  if (data & kMiddleUp) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("Injecting MIDDLE UP");
#endif
    current_button_state_ &= ~blink::WebMouseEvent::kMiddleButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
}

void MouseMuxInputController::OnMouseWheel(int hwid, float x, float y, int delta, bool horizontal) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Update position tracking.
  user_positions_[hwid] = {x, y};

  // If no owner, ignore wheel events.
  if (owner_hwid_ == -1) {
#ifdef MOUSEMUX_DEBUG
    LogDebug("WHEEL IGNORED: No owner set");
#endif
    return;
  }

  // Only process events from the owner.
  if (hwid != owner_hwid_) {
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
    InjectWheelEvent(view, x, y, delta, horizontal);
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
    owner_hwid_ = -1;
    current_button_state_ = 0;
    is_captured_ = false;
    user_positions_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
    pressed_keys_.clear();
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
    owner_hwid_ = -1;
    current_button_state_ = 0;
    is_captured_ = false;
    user_positions_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
    pressed_keys_.clear();
#ifdef MOUSEMUX_PEN_TOUCH_INJECT
    pen_state_.clear();
#endif
    drag_target_view_ = nullptr;
    keyboard_target_view_ = nullptr;
    pending_view_ = nullptr;
    has_pending_motion_ = false;
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

  // If the disposed user was the owner, clear ownership and keyboard state.
  if (hwid_mouse == owner_hwid_) {
    LogDebug("OWNER DISPOSED - clearing ownership");
    owner_hwid_ = -1;
    current_button_state_ = 0;
    pressed_keys_.clear();
    NotifyOwnershipChanged();
  }

  // Remove from position tracking.
  user_positions_.erase(hwid_mouse);
  user_positions_.erase(hwid_keyboard);

  // Remove from user info cache.
  user_info_.erase(hwid_mouse);

  // Remove from keyboard mapping.
  keyboard_to_mouse_hwid_.erase(hwid_keyboard);
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
  if (owner_hwid_ == -1) {
#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        "KEY SKIP: no owner yet, kb_hwid=0x%x vkey=0x%x", hwid, vkey));
#endif
    return;
  }

  // Accept keyboard events from ANY keyboard when there is an owner.
  // MouseMux "users" are device pairings — a keyboard may be paired with a
  // different mouse "user" than the person physically using it.  During
  // capture, MouseMux's own keyboard hook ensures only the captured user's
  // keyboard events reach Chrome, making an ownership check here redundant.
  // Without capture, we still accept all keyboard events for the owner.
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



  // Track key state.
  if (is_down) {
    if (pressed_keys_.count(vkey)) {
#ifdef MOUSEMUX_DEBUG
      LogDebug(base::StringPrintf(
          "KEY ACCEPT REPEAT: kb=0x%x vkey=0x%x owner=0x%x",
          hwid, vkey, owner_hwid_));
#endif
    } else {
      pressed_keys_.insert(vkey);
#ifdef MOUSEMUX_DEBUG
      LogDebug(base::StringPrintf(
          "KEY ACCEPT DOWN: kb=0x%x vkey=0x%x scan=%d owner=0x%x views=%zu",
          hwid, vkey, scan, owner_hwid_,
          registered_views_.size()));
#endif
    }
  } else {
    pressed_keys_.erase(vkey);
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
    bool sdk_shift = pressed_keys_.count(VK_SHIFT) || pressed_keys_.count(VK_LSHIFT) ||
                     pressed_keys_.count(VK_RSHIFT);
    bool sdk_ctrl = pressed_keys_.count(VK_CONTROL) || pressed_keys_.count(VK_LCONTROL) ||
                    pressed_keys_.count(VK_RCONTROL);
    bool sdk_alt = pressed_keys_.count(VK_MENU) || pressed_keys_.count(VK_LMENU) ||
                   pressed_keys_.count(VK_RMENU);
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

  // Inject the keyboard event.
  if (registered_views_.empty()) {
    LogDebug("KEY INJECT FAILED: No views registered!");
    return;
  }

  // Use the view that last received a mouse click. Fall back to first
  // showing view, then any view.
  RenderWidgetHostViewAura* view = nullptr;
  if (keyboard_target_view_ && registered_views_.count(keyboard_target_view_)) {
    view = keyboard_target_view_;
  } else {
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
  InjectKeyboardEvent(view, vkey, is_down);
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
    blink::WebMouseEvent* event) {
  event->id = 0;  // Primary pointer.

  // Subtype comes from the user list, so it is known before the first event
  // arrives — no need to infer pen-ness from the event stream.
  MouseMuxClient::PointerSubtype subtype =
      MouseMuxClient::PointerSubtype::kMouse;
  auto user_it = user_info_.find(owner_hwid_);
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

  auto pen_it = pen_state_.find(owner_hwid_);
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
    menu_dismiss_callback_.Run();
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
    if (!view->HasFocus()) {
      view->Focus();
    }
    // Also set page-level focus directly (sends SetFocus IPC to renderer),
    // matching what DevTools Input.dispatchMouseEvent does.
    host->Focus();
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
    modifiers |= current_button_state_;  // Include held button state for drags
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
    if (current_button_state_ & blink::WebMouseEvent::kLeftButtonDown) {
      event.button = blink::WebPointerProperties::Button::kLeft;
    } else if (current_button_state_ & blink::WebMouseEvent::kRightButtonDown) {
      event.button = blink::WebPointerProperties::Button::kRight;
    } else if (current_button_state_ & blink::WebMouseEvent::kMiddleButtonDown) {
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
  ApplyPointerProperties(&event);
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
  if (type == blink::WebInputEvent::Type::kMouseMove && current_button_state_ != 0) {
    LogDebug(base::StringPrintf(
        ">>> DRAG MOVE: widget(%.1f,%.1f) screen(%.1f,%.1f) mods=0x%x btn_state=0x%x",
        widget_x, widget_y, dip_screen_x, dip_screen_y,
        event.GetModifiers(), current_button_state_));
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
    blink::WebInputEvent::Type type,
    float screen_x,
    float screen_y,
    int button_flags) {
  RenderWidgetHostViewAura* view = nullptr;

  // During drag (button held), route all events to the drag target view.
  // This is critical for text selection — Chrome's selection handler requires
  // all events in a drag sequence to go to the SAME view.
  if (current_button_state_ != 0 && drag_target_view_ &&
      registered_views_.count(drag_target_view_)) {
    view = drag_target_view_;
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
      DispatchToAuraHost(type, screen_x, screen_y, button_flags);
    }
#else
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp) {
      LogDebug("INJECT FAILED: No view available!");
    }
#endif
    return;
  }

  // Track the drag target view: set on mousedown, clear on mouseup.
  if (type == blink::WebInputEvent::Type::kMouseDown) {
    drag_target_view_ = view;
    keyboard_target_view_ = view;  // Keyboard follows the last click.
  } else if (type == blink::WebInputEvent::Type::kMouseUp &&
             current_button_state_ == 0) {
    drag_target_view_ = nullptr;
  }

  InjectMouseEvent(view, type, screen_x, screen_y, button_flags);
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

void MouseMuxInputController::DispatchToAuraHost(
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

  // Enumerate ALL aura WindowTreeHosts and collect visible ones at the point.
  aura::Env* env = aura::Env::GetInstance();
  if (!env) {
    DiagLog("DispatchToAuraHost: no aura::Env");
    return;
  }

  gfx::Point screen_pt(static_cast<int>(screen_x), static_cast<int>(screen_y));

  const auto& hosts = env->window_tree_hosts();
  DiagLog(base::StringPrintf(
      "DispatchToAuraHost: screen(%.0f,%.0f) msg=0x%x hosts=%zu",
      screen_x, screen_y, msg, hosts.size()));

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
    DiagLog("DispatchToAuraHost: NO HOST MATCHED");
    return;
  }

  // Walk Win32 Z-order (top to bottom) to find the topmost candidate.
  HWND target_hwnd = nullptr;
  HWND current = ::GetTopWindow(nullptr);
  while (current) {
    if (candidates.count(current)) {
      target_hwnd = current;
      break;
    }
    current = ::GetNextWindow(current, GW_HWNDNEXT);
  }
  if (!target_hwnd) {
    // Fallback — pick any candidate.
    target_hwnd = candidates.begin()->first;
  }

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
  modifiers |= current_button_state_;

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
  RenderWidgetHostViewAura* view = keyboard_target_view_;
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
                RenderWidgetHostViewAura* v = self->keyboard_target_view_;
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
}

void MouseMuxInputController::InjectKeyboardEvent(
    RenderWidgetHostViewAura* view,
    int vkey,
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

  // Build modifiers from currently pressed modifier keys.
  int modifiers = blink::WebInputEvent::kFromDebugger;
  if (pressed_keys_.count(ui::VKEY_SHIFT) || pressed_keys_.count(ui::VKEY_LSHIFT) ||
      pressed_keys_.count(ui::VKEY_RSHIFT)) {
    modifiers |= blink::WebInputEvent::kShiftKey;
  }
  if (pressed_keys_.count(ui::VKEY_CONTROL) || pressed_keys_.count(ui::VKEY_LCONTROL) ||
      pressed_keys_.count(ui::VKEY_RCONTROL)) {
    modifiers |= blink::WebInputEvent::kControlKey;
  }
  if (pressed_keys_.count(ui::VKEY_MENU) || pressed_keys_.count(ui::VKEY_LMENU) ||
      pressed_keys_.count(ui::VKEY_RMENU)) {
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
  if (is_down && dom_key.IsCharacter()) {
    char16_t ch = static_cast<char16_t>(dom_key.ToCharacter());
    int char_flags = ui::EF_IS_SYNTHESIZED;
    if (modifiers & blink::WebInputEvent::kShiftKey)
      char_flags |= ui::EF_SHIFT_DOWN;
    if (modifiers & blink::WebInputEvent::kControlKey)
      char_flags |= ui::EF_CONTROL_DOWN;
    if (modifiers & blink::WebInputEvent::kAltKey)
      char_flags |= ui::EF_ALT_DOWN;

    ui::KeyEvent char_event = ui::KeyEvent::FromCharacter(
        ch, static_cast<ui::KeyboardCode>(vkey), dom_code, char_flags);

#ifdef MOUSEMUX_DEBUG
    LogDebug(base::StringPrintf(
        ">>> INJECT CHAR via InsertChar: vkey=0x%x char='%c' (%d)",
        vkey, static_cast<char>(ch), static_cast<int>(ch)));
#endif

    view->InsertChar(char_event);
  }
}

}  // namespace content
