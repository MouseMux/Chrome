// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"

#include <windows.h>
#include <fstream>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/process/process_handle.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
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
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/events/event_utils.h"
#include "ui/events/types/scroll_types.h"
#include "ui/latency/latency_info.h"

namespace content {

namespace {

// Always-on diagnostic log - only writes when something goes wrong.
const char kDiagLogPath[] = "O:/tmp/mousemux_diag.log";

void DiagLog(const std::string& message) {
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.LocalExplode(&exploded);
  std::ofstream file(kDiagLogPath, std::ios::app);
  if (file.is_open()) {
    file << base::StringPrintf("[%02d:%02d:%02d.%03d|PID:%d] %s\n",
                                exploded.hour, exploded.minute,
                                exploded.second, exploded.millisecond,
                                static_cast<int>(base::GetCurrentProcId()),
                                message.c_str());
    file.close();
  }
}

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
  LogDebug("MouseMuxInputController created");
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
    LogDebug(base::StringPrintf("ReleaseCapture: Released hwid=0x%x", owner_hwid_));
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(false);
    }
    return true;
  }
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

  LogDebug(base::StringPrintf("SetNativeInputBlocked(%s) - %zu views registered",
                               blocked ? "true" : "false",
                               registered_views_.size()));

  // Update all registered views - block both mouse and keyboard.
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (view && view->event_handler()) {
      view->event_handler()->SetNativeMouseInputBlocked(blocked);
      view->event_handler()->SetNativeKeyboardInputBlocked(blocked);
      LogDebug("  - Updated view event handler (mouse + keyboard)");
    }
  }
}

void MouseMuxInputController::SetMouseMuxEnabled(bool enabled) {
  LogDebug(base::StringPrintf("SetMouseMuxEnabled(%s)", enabled ? "true" : "false"));

  if (enabled) {
    if (!client_) {
      LogDebug("Creating new MouseMuxClient...");
      client_ = std::make_unique<MouseMuxClient>();
      // Pass our debug callback to the client.
      if (debug_log_callback_) {
        client_->SetDebugLogCallback(debug_log_callback_);
        LogDebug("Debug callback passed to client");
      }
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
  // Clear pending_view_ if it points to the unregistered view to prevent
  // dangling pointer access in the stuck-ACK detection logic.
  LogDebug(base::StringPrintf("UnregisterView: now %zu views", registered_views_.size()));
}

void MouseMuxInputController::OnMouseMotion(int hwid, float x, float y) {
  // Ensure we're on the UI thread - WebSocket callbacks come from IO thread.
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&MouseMuxInputController::OnMouseMotion,
                       base::Unretained(this), hwid, x, y));
    return;
  }

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
    return;
  }

  // Inject motion event.
  last_motion_inject_time_ = now;
  has_pending_motion_ = false;
  InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseMove, x, y,
                            current_button_state_);
}

void MouseMuxInputController::OnMouseButton(int hwid,
                                            float x,
                                            float y,
                                            int data) {
  // Ensure we're on the UI thread - WebSocket callbacks come from IO thread.
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&MouseMuxInputController::OnMouseButton,
                       base::Unretained(this), hwid, x, y, data));
    return;
  }

  // Flush any pending motion before button event to ensure accurate position.
  if (has_pending_motion_ && hwid == owner_hwid_) {
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseMove,
                              pending_motion_x_, pending_motion_y_,
                              current_button_state_);
    has_pending_motion_ = false;
  }

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

  // Update position tracking.
  user_positions_[hwid] = {x, y};

  // Check if this is a click that should claim ownership.
  // Only left-down claims ownership.
  if (owner_hwid_ == -1 && (data & kLeftDown)) {
    if (registered_views_.empty()) {
      LogDebug("BTN IGNORED: No views registered - cannot claim ownership");
      return;
    }

    // Check if cursor is over Chrome using hit-test.
    RenderWidgetHostViewAura* hit_view = FindViewAtPoint(x, y);
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
    LogDebug("BTN IGNORED: No owner set");
    return;
  }

  // Only process events from the owner.
  if (hwid != owner_hwid_) {
    LogDebug(base::StringPrintf("BTN IGNORED: hwid=0x%x is not owner=0x%x", hwid, owner_hwid_));
    return;
  }

  // Process button state changes from owner.
  if (data & kLeftDown) {
    LogDebug("Injecting LEFT DOWN");
    current_button_state_ |= blink::WebMouseEvent::kLeftButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kLeftUp) {
    LogDebug("Injecting LEFT UP");
    current_button_state_ &= ~blink::WebMouseEvent::kLeftButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kLeftButtonDown);
  }
  if (data & kRightDown) {
    LogDebug("Injecting RIGHT DOWN");
    current_button_state_ |= blink::WebMouseEvent::kRightButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kRightUp) {
    LogDebug("Injecting RIGHT UP");
    current_button_state_ &= ~blink::WebMouseEvent::kRightButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kRightButtonDown);
  }
  if (data & kMiddleDown) {
    LogDebug("Injecting MIDDLE DOWN");
    current_button_state_ |= blink::WebMouseEvent::kMiddleButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseDown, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
  if (data & kMiddleUp) {
    LogDebug("Injecting MIDDLE UP");
    current_button_state_ &= ~blink::WebMouseEvent::kMiddleButtonDown;
    InjectMouseEventToAnyView(blink::WebInputEvent::Type::kMouseUp, x, y,
                              blink::WebMouseEvent::kMiddleButtonDown);
  }
}

void MouseMuxInputController::OnMouseWheel(int hwid, float x, float y, int delta, bool horizontal) {
  // Ensure we're on the UI thread - WebSocket callbacks come from IO thread.
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&MouseMuxInputController::OnMouseWheel,
                       base::Unretained(this), hwid, x, y, delta, horizontal));
    return;
  }

  // Update position tracking.
  user_positions_[hwid] = {x, y};

  // If no owner, ignore wheel events.
  if (owner_hwid_ == -1) {
    LogDebug("WHEEL IGNORED: No owner set");
    return;
  }

  // Only process events from the owner.
  if (hwid != owner_hwid_) {
    return;
  }

  // Log wheel events.
  LogDebug(base::StringPrintf("WHEEL: delta=%d horizontal=%d pos=(%.0f,%.0f)",
                               delta, horizontal ? 1 : 0, x, y));

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
  // Ensure we're on the UI thread.
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&MouseMuxInputController::OnConnectionStateChanged,
                       base::Unretained(this), connected));
    return;
  }

  LogDebug(base::StringPrintf("OnConnectionStateChanged: %s (views=%zu)",
                               connected ? "CONNECTED" : "DISCONNECTED",
                               registered_views_.size()));

  // Notify dialog of connection state change.
  if (connection_changed_callback_) {
    connection_changed_callback_.Run(connected);
  }

  if (connected) {
    // Reset owner when reconnecting.
    owner_hwid_ = -1;
    current_button_state_ = 0;
    is_captured_ = false;
    user_positions_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
    pressed_keys_.clear();
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
    // Clear state on disconnect.
    owner_hwid_ = -1;
    current_button_state_ = 0;
    is_captured_ = false;
    user_positions_.clear();
    user_info_.clear();
    keyboard_to_mouse_hwid_.clear();
    pressed_keys_.clear();
    NotifyOwnershipChanged();
    if (capture_changed_callback_) {
      capture_changed_callback_.Run(false);
    }
  }
}

void MouseMuxInputController::OnUserList(
    const std::vector<MouseMuxClient::UserInfo>& users) {
  LogDebug(base::StringPrintf("UserList: %zu users", users.size()));
  user_info_.clear();
  keyboard_to_mouse_hwid_.clear();
  for (const auto& user : users) {
    LogDebug(base::StringPrintf("  User: id=%d name=%s mouse=0x%x kb=0x%x",
                                 user.user_id, user.name.c_str(),
                                 user.hwid_mouse, user.hwid_keyboard));
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
  LogDebug(base::StringPrintf("UserCreated: id=%d mouse=0x%x kb=0x%x name=%s",
                               user.user_id, user.hwid_mouse,
                               user.hwid_keyboard, user.name.c_str()));
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
  // Ensure we're on the UI thread.
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&MouseMuxInputController::OnKeyboardKey,
                       base::Unretained(this), hwid, vkey, message, scan, flags));
    return;
  }

  // If no owner, ignore keyboard events.
  if (owner_hwid_ == -1) {
    LogDebug(base::StringPrintf(
        "KEY SKIP: no owner yet, kb_hwid=0x%x vkey=0x%x", hwid, vkey));
    return;
  }

  // Look up which mouse hwid this keyboard belongs to.
  auto it = keyboard_to_mouse_hwid_.find(hwid);
  if (it == keyboard_to_mouse_hwid_.end()) {
    // Unknown keyboard - request user list refresh (rate-limited to every 2s).
    base::TimeTicks now = base::TimeTicks::Now();
    if (now - last_user_list_request_ > base::Seconds(2)) {
      last_user_list_request_ = now;
      LogDebug(base::StringPrintf(
          "KEY: unknown kb_hwid=0x%x vkey=0x%x owner=0x%x - requesting refresh",
          hwid, vkey, owner_hwid_));
      if (client_) {
        client_->RequestUserList();
      }
    }
    return;
  }

  int mouse_hwid = it->second;

  // Look up user name for logging.
  std::string user_name = "?";
  auto ui_it = user_info_.find(mouse_hwid);
  if (ui_it != user_info_.end()) {
    user_name = ui_it->second.name;
  }

  // Only accept keyboard events from the owner's keyboard.
  if (mouse_hwid != owner_hwid_) {
    LogDebug(base::StringPrintf(
        "KEY BLOCKED: kb=0x%x user=%s(mouse=0x%x) != owner=0x%x vkey=0x%x",
        hwid, user_name.c_str(), mouse_hwid, owner_hwid_, vkey));
    return;
  }

  // Determine if key down or up based on Windows message.
  // WM_KEYDOWN = 0x100, WM_KEYUP = 0x101
  // WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105
  bool is_down = (message == 0x100 || message == 0x104);
  bool is_up = (message == 0x101 || message == 0x105);

  if (!is_down && !is_up) {
    LogDebug(base::StringPrintf("KEY IGNORED: unknown message=0x%x", message));
    return;
  }

  // Track key state.
  if (is_down) {
    if (pressed_keys_.count(vkey)) {
      LogDebug(base::StringPrintf(
          "KEY ACCEPT REPEAT: user=%s kb=0x%x vkey=0x%x owner=0x%x",
          user_name.c_str(), hwid, vkey, owner_hwid_));
    } else {
      pressed_keys_.insert(vkey);
      LogDebug(base::StringPrintf(
          "KEY ACCEPT DOWN: user=%s kb=0x%x vkey=0x%x scan=%d owner=0x%x views=%zu",
          user_name.c_str(), hwid, vkey, scan, owner_hwid_,
          registered_views_.size()));
    }
  } else {
    pressed_keys_.erase(vkey);
    LogDebug(base::StringPrintf(
        "KEY ACCEPT UP: user=%s kb=0x%x vkey=0x%x scan=%d owner=0x%x",
        user_name.c_str(), hwid, vkey, scan, owner_hwid_));
  }

  // Check for hotkey (only on key down).
  if (is_down && keyboard_event_callback_) {
    bool shift = pressed_keys_.count(VK_SHIFT) || pressed_keys_.count(VK_LSHIFT) ||
                 pressed_keys_.count(VK_RSHIFT);
    bool ctrl = pressed_keys_.count(VK_CONTROL) || pressed_keys_.count(VK_LCONTROL) ||
                pressed_keys_.count(VK_RCONTROL);
    bool alt = pressed_keys_.count(VK_MENU) || pressed_keys_.count(VK_LMENU) ||
               pressed_keys_.count(VK_RMENU);
    if (keyboard_event_callback_.Run(vkey, shift, ctrl, alt, is_down)) {
      LogDebug("KEY CONSUMED by hotkey callback");
      return;
    }
  }

  // Inject the keyboard event.
  if (registered_views_.empty()) {
    LogDebug("KEY INJECT FAILED: No views registered!");
    return;
  }

  RenderWidgetHostViewAura* view = *registered_views_.begin();
  LogDebug(base::StringPrintf(
      "KEY INJECT -> view=%p views_total=%zu",
      static_cast<void*>(view), registered_views_.size()));
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

  // Check each registered view. Only consider visible (showing) views
  // to avoid injecting events into hidden/inactive tabs.
  for (RenderWidgetHostViewAura* view : registered_views_) {
    if (!view || !view->IsShowing())
      continue;

    // Get view bounds (in screen DIP coordinates).
    gfx::Rect bounds = view->GetViewBounds();

    if (bounds.Contains(static_cast<int>(dip_x), static_cast<int>(dip_y))) {
      return view;
    }
  }

  return nullptr;
}

bool MouseMuxInputController::IsPointOverChrome(float screen_x, float screen_y) {
  return FindViewAtPoint(screen_x, screen_y) != nullptr;
}

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

  // Ensure focus for events to be processed.
  if (!view->HasFocus()) {
    view->Focus();
  }
  // For button events, also set page-level focus directly (sends SetFocus IPC
  // to renderer), matching what DevTools Input.dispatchMouseEvent does.
  // view->Focus() alone only sets OS-level window focus; the page focus IPC
  // may arrive at the renderer AFTER the mouse event without this.
  if (type == blink::WebInputEvent::Type::kMouseDown ||
      type == blink::WebInputEvent::Type::kMouseUp) {
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
    // For move events during drag, button is kNoButton.
    event.button = blink::WebPointerProperties::Button::kNoButton;
    event.click_count = 0;
  }

  // Set pointer type to mouse.
  event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  event.id = 0;  // Primary pointer

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
  // Try to find a view at the point first.
  RenderWidgetHostViewAura* view = FindViewAtPoint(screen_x, screen_y);

  // If no view at point, use the first visible view as fallback.
  // This handles the case where the owner's cursor is outside Chrome.
  if (!view && !registered_views_.empty()) {
    for (RenderWidgetHostViewAura* v : registered_views_) {
      if (v && v->IsShowing()) {
        view = v;
        break;
      }
    }
    // Last resort: use any view if none are showing.
    if (!view) {
      view = *registered_views_.begin();
    }
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp) {
      LogDebug("Using fallback view for injection");
    }
  }

  if (!view) {
    if (type == blink::WebInputEvent::Type::kMouseDown ||
        type == blink::WebInputEvent::Type::kMouseUp) {
      LogDebug("INJECT FAILED: No view available!");
    }
    return;
  }

  InjectMouseEvent(view, type, screen_x, screen_y, button_flags);
}

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

  // Ensure view has focus for keyboard events to be processed.
  if (!view->HasFocus()) {
    view->Focus();
  }

  // Determine event type.
  blink::WebInputEvent::Type type = is_down
      ? blink::WebInputEvent::Type::kRawKeyDown
      : blink::WebInputEvent::Type::kKeyUp;

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

  // Create the WebKeyboardEvent.
  blink::WebKeyboardEvent event(type, modifiers, base::TimeTicks::Now());

  // Set key code.
  event.windows_key_code = vkey;
  event.native_key_code = vkey;

  // Set DOM code from virtual key code.
  event.dom_code = static_cast<int>(ui::KeycodeConverter::NativeKeycodeToDomCode(vkey));

  // Set DOM key for the JavaScript `key` property on keydown/keyup events.
  // Only dom_key is set — NOT text[0]. Text insertion in modern Blink happens
  // via the kRawKeyDown's dom_key triggering beforeinput/insertText, so no
  // separate kChar event is needed (and would cause double insertion).
  if (vkey >= 'A' && vkey <= 'Z') {
    char16_t ch = static_cast<char16_t>(vkey);
    if (!(modifiers & blink::WebInputEvent::kShiftKey)) {
      ch = ch - 'A' + 'a';
    }
    event.dom_key = ui::DomKey::FromCharacter(ch);
  } else if (vkey >= '0' && vkey <= '9') {
    event.dom_key = ui::DomKey::FromCharacter(static_cast<char16_t>(vkey));
  } else if (vkey == ui::VKEY_RETURN) {
    event.dom_key = ui::DomKey::ENTER;
  } else if (vkey == ui::VKEY_SPACE) {
    event.dom_key = ui::DomKey::FromCharacter(' ');
  } else if (vkey == ui::VKEY_TAB) {
    event.dom_key = ui::DomKey::TAB;
  } else if (vkey == ui::VKEY_ESCAPE) {
    event.dom_key = ui::DomKey::ESCAPE;
  } else if (vkey == ui::VKEY_BACK) {
    event.dom_key = ui::DomKey::BACKSPACE;
  } else if (vkey == ui::VKEY_DELETE) {
    event.dom_key = ui::DomKey::DEL;
  }

  // Log injection.
  LogDebug(base::StringPrintf(">>> INJECT KEY %s: vkey=0x%x mods=0x%x",
                               is_down ? "DOWN" : "UP", vkey, modifiers));

  // Forward the key event (kRawKeyDown or kKeyUp).
  // Do NOT send a separate kChar event. Modern Blink inserts text from
  // kRawKeyDown when dom_key is set to a character (via beforeinput/insertText).
  // Sending an additional kChar causes double insertion because both events
  // are queued in the InputRouter before the renderer ACKs the first one,
  // bypassing the InputRouter's char-suppression mechanism.
  input::NativeWebKeyboardEvent native_event(event, gfx::NativeView());
  host->ForwardKeyboardEvent(native_event);
}

}  // namespace content
