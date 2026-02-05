// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_INPUT_CONTROLLER_H_
#define CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_INPUT_CONTROLLER_H_

#include <map>
#include <memory>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_client.h"
#include "content/common/content_export.h"
#include "third_party/blink/public/common/input/web_input_event.h"

namespace content {

class RenderWidgetHostViewAura;

// Singleton controller that coordinates MouseMux integration.
// Manages the WebSocket connection and event injection into registered views.
class CONTENT_EXPORT MouseMuxInputController : public MouseMuxClient::Observer {
 public:
  // Debug logging callback type.
  using DebugLogCallback = base::RepeatingCallback<void(const std::string&)>;

  // Ownership changed callback type. Called with hwid and name when owner changes.
  using OwnershipChangedCallback = base::RepeatingCallback<void(int hwid, const std::string& name)>;

  // Connection state changed callback type.
  using ConnectionChangedCallback = base::RepeatingCallback<void(bool connected)>;

  // Capture state changed callback type.
  using CaptureChangedCallback = base::RepeatingCallback<void(bool captured)>;

  // Keyboard event callback type (for hotkey detection).
  // Parameters: vkey, shift, ctrl, alt, is_down
  // Return true to consume the event (don't inject to view).
  using KeyboardEventCallback = base::RepeatingCallback<bool(int vkey, bool shift, bool ctrl, bool alt, bool is_down)>;

  // Returns the singleton instance.
  static MouseMuxInputController* GetInstance();

  MouseMuxInputController(const MouseMuxInputController&) = delete;
  MouseMuxInputController& operator=(const MouseMuxInputController&) = delete;

  // Controls whether native mouse input is blocked for web content.
  void SetNativeInputBlocked(bool blocked);
  bool IsNativeInputBlocked() const { return native_input_blocked_; }

  // Controls the WebSocket connection to MouseMux server.
  void SetMouseMuxEnabled(bool enabled);
  bool IsMouseMuxEnabled() const;

  // Register/unregister views for event injection.
  void RegisterView(RenderWidgetHostViewAura* view);
  void UnregisterView(RenderWidgetHostViewAura* view);

  // Set a callback for debug logging.
  void SetDebugLogCallback(DebugLogCallback callback);

  // Set a callback for ownership changes.
  void SetOwnershipChangedCallback(OwnershipChangedCallback callback);

  // Set a callback for connection state changes.
  void SetConnectionChangedCallback(ConnectionChangedCallback callback);

  // Set a callback for capture state changes.
  void SetCaptureChangedCallback(CaptureChangedCallback callback);

  // Set a callback for keyboard events (for hotkey detection).
  // If callback returns true, the key event is consumed and not injected.
  void SetKeyboardEventCallback(KeyboardEventCallback callback);

  // Release current ownership, allowing a new user to claim.
  void ReleaseOwnership();

  // Capture the current owner's mouse (stops it from sending to Windows).
  // Returns true if capture request was sent, false if no owner.
  bool CaptureOwner();

  // Release capture of the current owner's mouse.
  // Returns true if release request was sent, false if not captured.
  bool ReleaseCapture();

  // Check if owner's mouse is currently captured.
  bool IsCaptured() const { return is_captured_; }

  // Get current owner hwid (-1 if no owner).
  int GetOwnerHwid() const { return owner_hwid_; }

  // Get current owner name (empty if no owner or unknown).
  std::string GetOwnerName() const;

  // MouseMuxClient::Observer implementation:
  void OnMouseMotion(int hwid, float x, float y) override;
  void OnMouseButton(int hwid, float x, float y, int data) override;
  void OnMouseWheel(int hwid, float x, float y, int delta, bool horizontal) override;
  void OnConnectionStateChanged(bool connected) override;
  void OnUserList(const std::vector<MouseMuxClient::UserInfo>& users) override;
  void OnUserCreated(const MouseMuxClient::UserInfo& user) override;
  void OnUserDisposed(int hwid_mouse, int hwid_keyboard) override;
  void OnKeyboardKey(int hwid, int vkey, int message, int scan, int flags) override;
  void OnTimeoutWarning(int minutes) override;
  void OnTimeoutStopped(const std::string& reason) override;

  // For testing.
  MouseMuxClient* client_for_testing() { return client_.get(); }

 private:
  friend class base::NoDestructor<MouseMuxInputController>;
  friend class MouseMuxInputControllerTest;

  MouseMuxInputController();
  ~MouseMuxInputController() override;

  // Internal debug logging.
  void LogDebug(const std::string& message);

  // Finds the view under the given screen coordinates.
  // Set verbose_log=true for debugging (only use for button events, not motion).
  RenderWidgetHostViewAura* FindViewAtPoint(float screen_x, float screen_y,
                                            bool verbose_log = false);

  // Checks if screen coordinates are over any Chrome view.
  bool IsPointOverChrome(float screen_x, float screen_y);

  // Injects a mouse event into the given view.
  void InjectMouseEvent(RenderWidgetHostViewAura* view,
                        blink::WebInputEvent::Type type,
                        float screen_x,
                        float screen_y,
                        int button_flags);

  // Injects mouse event to any available view (for owner who may be outside).
  void InjectMouseEventToAnyView(blink::WebInputEvent::Type type,
                                 float screen_x,
                                 float screen_y,
                                 int button_flags);

  // Injects a wheel event into the given view.
  void InjectWheelEvent(RenderWidgetHostViewAura* view,
                        float screen_x,
                        float screen_y,
                        int delta,
                        bool horizontal = false);

  // Injects a keyboard event into the given view.
  void InjectKeyboardEvent(RenderWidgetHostViewAura* view,
                           int vkey,
                           bool is_down);

  // Gets the keyboard hwid for the current owner (from user info).
  int GetOwnerKeyboardHwid() const;

  bool native_input_blocked_ = false;
  std::unique_ptr<MouseMuxClient> client_;
  std::set<raw_ptr<RenderWidgetHostViewAura>> registered_views_;

  // Button state tracking.
  int current_button_state_ = 0;

  // Owner tracking: the hwid that has claimed ownership by clicking on Chrome.
  // -1 means no owner yet.
  int owner_hwid_ = -1;

  // Track last known position for each hwid.
  struct UserPosition {
    float x = 0;
    float y = 0;
  };
  std::map<int, UserPosition> user_positions_;

  // Motion event counter for throttled logging.
  int motion_count_ = 0;

  // Motion throttling - limit to ~60fps to avoid flooding UI thread.
  base::TimeTicks last_motion_inject_time_;
  float pending_motion_x_ = 0;
  float pending_motion_y_ = 0;
  bool has_pending_motion_ = false;

  // Debug logging callback.
  DebugLogCallback debug_log_callback_;

  // Ownership changed callback.
  OwnershipChangedCallback ownership_changed_callback_;

  // Connection state changed callback.
  ConnectionChangedCallback connection_changed_callback_;

  // Capture state changed callback.
  CaptureChangedCallback capture_changed_callback_;

  // Keyboard event callback.
  KeyboardEventCallback keyboard_event_callback_;

  // Whether the owner's mouse is currently captured.
  bool is_captured_ = false;

  // User info cache (hwid_mouse -> UserInfo).
  std::map<int, MouseMuxClient::UserInfo> user_info_;

  // Keyboard hwid to mouse hwid mapping (for looking up owner).
  std::map<int, int> keyboard_to_mouse_hwid_;

  // Keyboard state tracking - which keys are currently pressed.
  std::set<int> pressed_keys_;

  // Rate-limit user list refresh requests for unknown keyboards.
  base::TimeTicks last_user_list_request_;

  // InputRouter pending-state tracking for stuck ACK detection.
  // When the InputRouter has had pending events for too long, we reset it.
  base::TimeTicks pending_start_time_;
  raw_ptr<RenderWidgetHostViewAura> pending_view_ = nullptr;

  // Notify ownership changed.
  void NotifyOwnershipChanged();
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_INPUT_CONTROLLER_H_
