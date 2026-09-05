// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_INPUT_CONTROLLER_H_
#define CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_INPUT_CONTROLLER_H_

// Compile-time defines (MOUSEMUX_DEBUG, MOUSEMUX_NATIVE_BLOCK, the version
// stamp) live in mouse_mux_config.h — a tiny header that rarely changes.
// This prevents cascade rebuilds when only the controller header changes.
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_config.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/timer/timer.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_client.h"
#include "content/common/content_export.h"
#include "content/public/browser/render_widget_host.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/gfx/native_ui_types.h"

namespace blink {
// web_input_event.h declares WebInputEvent but not WebMouseEvent, which lives
// in web_mouse_event.h.  Forward declared rather than included: this header is
// pulled in by the dialog, so a heavier include here widens the rebuild.
class WebMouseEvent;
}  // namespace blink

namespace content {

class MouseMuxControlServer;
class RenderWidgetHostImpl;
class RenderWidgetHostViewAura;

// Singleton controller that coordinates MouseMux integration.
// Manages the WebSocket connection and event injection into registered views.
class CONTENT_EXPORT MouseMuxInputController
    : public MouseMuxClient::Observer,
      public RenderWidgetHost::InputEventObserver {
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

  // Menu dismiss callback type. Called to dismiss any active context menu
  // before injecting a mouse down event. Implemented by the chrome layer
  // since content cannot depend on ui/views.
  // Every injected press, with whether it landed on a page.  The dialog
  // uses it to attribute the open menu to the user who opened it (any
  // press counts for that) and to close it when that user presses a page
  // outside it (only a page press may cancel).
  using MenuDismissCallback =
      base::RepeatingCallback<void(int hwid, bool page_press)>;

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

  // A snapshot of the dialog's view tree, for diagnosing what reached the
  // screen and what did not.  The dialog supplies it; content cannot look at
  // views, so this is a string it hands over rather than anything structured.
  using DiagnosticsCallback = base::RepeatingCallback<std::string()>;
  void SetDiagnosticsCallback(DiagnosticsCallback callback);

  // A message from the server the operator should see: a timeout warning,
  // the session ending.  The dialog shows it in a window of its own; a
  // MessageBox from here would spin a nested loop on the UI thread and stall
  // every user's input until dismissed.
  using NoticeCallback =
      base::RepeatingCallback<void(const std::string& text, bool error)>;
  void SetNoticeCallback(NoticeCallback callback);
  std::string GetDialogDiagnostics() const;

  // The MouseMux version we are connected to, empty when not connected or
  // before the server has introduced itself.  Passed through from the client
  // so the dialog does not have to know one exists.
  std::string GetServerVersion() const;

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

  // Set a callback for native input blocking state changes.

  // Set a callback to dismiss active context menus before mouse down.
  void SetMenuDismissCallback(MenuDismissCallback callback);

  // Dialog visibility.  Implemented by the chrome layer via callback since
  // content cannot depend on ui/views.
  using VisibilityChangedCallback = base::RepeatingCallback<void(bool visible)>;
  void SetVisibilityChangedCallback(VisibilityChangedCallback callback);

  // Drops every callback the dialog registered.  The dialog binds them to
  // itself with raw pointers and dies before the browser windows do (closing
  // it is what ends the seat), while each tab that closes after it still
  // notifies ownership changes through here.  Called from the dialog's
  // destructor; after it, notifications go nowhere instead of into freed
  // memory.
  void ClearUiCallbacks();

  // Show or hide the control dialog.  Hiding leaves the seat fully
  // functional — only the window goes away — so it can always be brought
  // back through the control server.
  void SetDialogVisible(bool visible);
  bool IsDialogVisible() const { return dialog_visible_; }

  // Set owner by hwid directly (for automation via control server).
  void SetOwner(int hwid);

  // Set owner by name (e.g. "user1:mouse1"). Returns false if not found.
  bool SetOwnerByName(const std::string& name);

  // Gives `window` to the user MouseMux calls `name`, as if they had clicked
  // in it, and applies their two flags.  For loading a saved layout and for
  // putting things back after a reconnect: the name is the stable identity,
  // device ids change with the hardware.  False when the name is not in the
  // user list (that device is not connected yet) or the window is another
  // user's; nothing is changed then.
  bool AssignWindow(const std::string& name,
                    gfx::AcceleratedWidget window,
                    bool captured,
                    bool block_native);
  // The same for a device already known by id (a virtual user the host page
  // just created, for instance).
  bool AssignWindowToHwid(int hwid,
                          gfx::AcceleratedWidget window,
                          bool captured,
                          bool block_native);

  // Every top-level window that has a page of ours in it, owned or not.
  std::vector<gfx::AcceleratedWidget> KnownWindows() const;

  // Release ALL ownership, allowing new users to claim.
  void ReleaseOwnership();

  // Release ONE owner, leaving the others working.  This is the dialog's
  // per-row "drop"; ReleaseOwnership() is the "release all" control.
  void ReleaseOwnerHwid(int hwid);

  // Capture the current owner's mouse (stops it from sending to Windows).
  // Returns true if capture request was sent, false if no owner.
  bool CaptureOwner();

  // Release capture of the current owner's mouse.
  // Returns true if release request was sent, false if not captured.
  bool ReleaseCapture();

  // Check if capture is in effect.  With several owners this answers "is
  // anyone captured", which is what a single indicator can usefully say —
  // per-owner state is in GetOwners().
  bool IsCaptured() const;

  // Capture or release ONE owner, leaving the others alone.  This is what the
  // dialog's per-owner control uses.  Returns false if the hwid is not an
  // owner, or is already in the requested state.
  bool CaptureOwnerHwid(int hwid);
  bool ReleaseCaptureHwid(int hwid);

  // One row of the dialog's owner list.  Everything the control centre needs
  // to show a user and act on them, gathered here so the dialog never has to
  // reach into controller internals — content cannot depend on ui/views, so
  // this struct is the whole contract between them.
  struct OwnerInfo {
    int hwid = -1;
    std::string name;             // from the SDK user list; may be empty
    std::u16string window_title;  // toplevel window this user is working in
    gfx::AcceleratedWidget window = gfx::kNullAcceleratedWidget;
    bool captured = false;
    bool is_primary = false;      // the one the single-owner API reports
    bool has_window = false;      // false until they have clicked somewhere
    int extra_windows = 0;        // owned windows beyond the current one
    bool block_native = true;     // real-mouse input dropped in their windows

    // Which keyboard MouseMux has attached to this user, and whether anything
    // has ever arrived from it.
    //
    // A user with a mouse but no keyboard is the single most common
    // misconfiguration behind "everybody's typing lands in one window":
    // MouseMux detects the keyboard and it types perfectly well, but until it
    // is assigned to the same USER as the mouse there is nothing to route by.
    // Nothing else in the product shows that, so it shows here.
    int keyboard_hwid = 0;        // 0 = no keyboard on this user
    bool keyboard_typed = false;  // a keystroke has arrived from it

    // Typing activity: 0 nothing recently, 1 delivered, 2 dropped.
    int typing = 0;

    // Which display this user's window is on, 1-based, 0 when unknown or when
    // there is only one.  Operators of a four-monitor desk think in screens
    // rather than window titles, and a title changes as people browse.
    //
  };

  // Every current owner, in hwid order so rows do not jump around between
  // refreshes.
  std::vector<OwnerInfo> GetOwners() const;

  // The MouseMux user name of the user who owns `window`, or "" when
  // nobody does.  Shown in the window's caption.
  std::string OwnerNameOfWindow(gfx::AcceleratedWidget window) const;
  // The hwid whose set contains `window`, or -1.
  int OwnerOfWindow(gfx::AcceleratedWidget window) const;

  // Per owner: whether the real mouse's input is dropped inside their
  // windows (default on).  See ApplyNativeBlocking().
  void SetOwnerBlockNative(int hwid, bool block);

  // Where recent keystrokes WENT — never what they were.  Nothing recorded
  // here identifies a key, which is why it can stay on in release builds
  // where per-key logging must not.
  //
  // It exists because a keyboard reaching the wrong window is otherwise
  // invisible: the owner list says where each user is WORKING, and that is
  // the same thing as where their typing lands only when the pairing is
  // right.  When it is not, this is the difference between a bug report and
  // a diagnosis.
  struct KeyRoute {
    int keyboard_hwid = 0;
    int mouse_hwid = -1;          // -1 when the keyboard could not be paired
    std::u16string window_title;  // empty when nothing was delivered
    bool dropped = false;
    int count = 1;                // identical consecutive routes, collapsed
  };

  // Most recent last.  Bounded by kMaxKeyRoutes.
  std::vector<KeyRoute> GetKeyRoutes() const;

  // What happened to keystrokes AFTER they were routed.
  //
  // Routing is observable already - the dialog shows each user typing - but
  // that only proves the event reached a view.  Everything past that point is
  // invisible from outside: whether we called into the renderer at all, and
  // whether the renderer took it.  Those two look identical from the dialog
  // and have completely different causes.
  //
  // Counters only.  No key codes, no characters, nothing that identifies what
  // was typed, so this stays compiled in and can be read from a customer's
  // machine over the control server.
  std::string GetInjectionStats() const;

  // Every registered view, with whether it claims to be showing and which
  // window it is in.  The keyboard goes to "the first showing view in the
  // user's window", and if more than one answers that, the choice is
  // arbitrary and can land in a tab nobody is looking at.
  std::string GetViewInventory() const;

  // Asks the browser which view belongs to the active tab of a window.
  //
  // The authoritative answer to "which page is this person looking at",
  // supplied by the dialog because only chrome/browser/ui can see a tab strip
  // and content/ must not depend on it.  Returns null when the window is not
  // a browser window or the callback is not installed.
  //
  // Worth the plumbing: guessing this from the views we happen to have
  // registered cost a day.  Two views in one window both reported themselves
  // as showing frames with identical names, and the one that sorted first was
  // a 34x34 widget that swallowed every keystroke.
  using ActiveViewForWindowCallback =
      base::RepeatingCallback<RenderWidgetHostViewAura*(gfx::AcceleratedWidget)>;
  void SetActiveViewForWindowCallback(ActiveViewForWindowCallback callback);

  // Launches another seat: a separate browser process with its own profile,
  // control port and dialog.  Picks the lowest free seat itself and starts
  // this same executable, so no helper program is involved.
  //
  // A seat is a PEER, not something this browser manages — it connects to
  // MouseMux itself and owns its own users.  Use it for isolation (separate
  // logins, or containing a crash); to add a user to THIS browser, open a
  // window instead.
  bool LaunchAdditionalSeat();

  // Hard lock: confine each user to the window they first clicked in.
  //
  // Off by default, which is "soft": a user who clicks another window simply
  // moves there.  That is not a separate mode, it is just what routing by
  // hit-test does, so there are only two states and this is a plain toggle.
  //
  // On, a click that lands outside a user's own window is dropped, giving four
  // genuinely independent workstations.  A user with no window yet claims the
  // first one they click, and closing a window frees its user to claim again,
  // so nobody can be locked out with no way back.
  void ClaimOwnSeat(int control_port);

  // Whether |view| should keep renderer page focus even though aura says its
  // window just lost focus: while anyone is captured, or when the view sits
  // in an owned window.
  //
  // In both cases the OS foreground window says nothing about who is working
  // where.  Captured devices produce no native input, and an owned window
  // gets its input by ownership, not by focus.  Letting an activation change
  // blur the view would kill the caret of a user who is still typing - which
  // happens every time the operator so much as clicks the control dialog.
  // With nobody captured and nothing owned, normal blur behaviour is intact.
  bool ShouldSuppressBlur(RenderWidgetHostViewAura* view) const;

  // Get current owner hwid (-1 if no owner).
  int GetOwnerHwid() const { return owner_hwid_; }

  // Get current owner name (empty if no owner or unknown).
  std::string GetOwnerName() const;

  // MouseMuxClient::Observer implementation:
  void OnMouseMotion(int hwid, float x, float y) override;
  void OnMouseButton(int hwid, float x, float y, int data) override;
  void OnMouseWheel(int hwid, float x, float y, int delta, bool horizontal) override;
#ifdef MOUSEMUX_PEN_TOUCH_INJECT
  void OnPenMotion(int hwid,
                   float x,
                   float y,
                   int pressure,
                   int tilt_x,
                   int tilt_y,
                   int rotation) override;
#endif
  void OnConnectionStateChanged(bool connected) override;
  void OnUserList(const std::vector<MouseMuxClient::UserInfo>& users) override;
  void OnUserCreated(const MouseMuxClient::UserInfo& user) override;
  void OnUserDisposed(int hwid_mouse, int hwid_keyboard) override;
  void OnKeyboardKey(int hwid, int vkey, int message, int scan, int flags) override;
  void OnTimeoutWarning(int minutes) override;
  void OnTimeoutStopped(const std::string& reason) override;

  // For testing.
  MouseMuxClient* client_for_testing() { return client_.get(); }

  // Debug logging, to the same file as everything else.  Public so the
  // control server can log what it is asked and what it answered.
  void LogDebug(const std::string& message);

 private:
  friend class base::NoDestructor<MouseMuxInputController>;
  friend class MouseMuxInputControllerTest;

  MouseMuxInputController();
  ~MouseMuxInputController() override;

  // Finds the view under the given screen coordinates.
  // Set verbose_log=true for debugging (only use for button events, not motion).
  RenderWidgetHostViewAura* FindViewAtPoint(float screen_x, float screen_y,
                                            bool verbose_log = false);

  // Injects a mouse event into the given view.  |hwid| is the MOUSE hwid the
  // event came from and selects which device's state applies.
  void InjectMouseEvent(int hwid,
                        RenderWidgetHostViewAura* view,
                        blink::WebInputEvent::Type type,
                        float screen_x,
                        float screen_y,
                        int button_flags);

  // Injects mouse event to any available view (for owner who may be outside).
  void InjectMouseEventToAnyView(int hwid,
                                 blink::WebInputEvent::Type type,
                                 float screen_x,
                                 float screen_y,
                                 int button_flags);

#ifdef MOUSEMUX_AURA_UI_CLICK_THROUGH
  // Checks if there's an overlay window (popup menu, dialog, dropdown) at the
  // given screen coordinates.  If found, dispatches the event through aura
  // and returns true.  Returns false if the topmost window is a web content
  // host (normal injection should handle it).
  // `owner`: consider only popups owned by that window (a user's own menu),
  // or any popup when null.
  bool TryDispatchToOverlayWindow(blink::WebInputEvent::Type type,
                                   float screen_x,
                                   float screen_y,
                                   int button_flags,
                                   gfx::AcceleratedWidget owner =
                                       gfx::kNullAcceleratedWidget);

  // The toplevel Chrome window under the given screen point, chosen by Win32
  // Z-order so the topmost one wins.  Null when the point is over nothing of
  // ours.
  //
  // Split out from DispatchToAuraHost because the ANSWER is needed before the
  // click is sent: the window a click lands in decides whether the hard lock
  // allows it and which window that user's keyboard should follow, and both
  // of those have to be settled first.
  gfx::AcceleratedWidget FindAuraTargetWindow(float screen_x, float screen_y);

  // Dispatches a click through the aura event system when no web content view
  // is under the cursor, so Chrome UI (tabs, popups, etc.) can receive it.
  void DispatchToAuraHost(gfx::AcceleratedWidget target,
                          blink::WebInputEvent::Type type,
                          float screen_x,
                          float screen_y,
                          int button_flags);
#endif

  // A registered web content view living in |window|, or null.  Used to give
  // a keyboard somewhere to go when its user clicked on Chrome's own UI
  // rather than on a page.
  RenderWidgetHostViewAura* WebViewInWindow(
      gfx::AcceleratedWidget window) const;

#ifdef MOUSEMUX_PEN_TOUCH_INJECT
  // Most recent pen/touch metadata per hwid, from pointer.pen.notify.M2A.
  struct PenState {
    int pressure = 0;
    int tilt_x = 0;
    int tilt_y = 0;
    int rotation = 0;
  };
  std::map<int, PenState> pen_state_;

  // Stamps pointer_type — and for pen/touch the pressure, tilt and twist
  // from pen_state_ — onto an event about to be injected, based on |hwid|'s
  // device subtype from the user list.  Applied to motion AND button events so
  // contact and movement look like the same pointer to Blink.
  void ApplyPointerProperties(int hwid, blink::WebMouseEvent* event);
#endif

  // Injects a wheel event into the given view.  |hwid| is the MOUSE hwid.
  void InjectWheelEvent(int hwid,
                        RenderWidgetHostViewAura* view,
                        float screen_x,
                        float screen_y,
                        int delta,
                        bool horizontal = false);

  // Injects a keyboard event into the given view.  |hwid| is the MOUSE hwid of
  // the pair the keystroke belongs to, NOT the keyboard's own hwid: modifier
  // state is held per device pair, and the caller has already resolved it.
  // |scan| is the hardware scan code the SDK reported.  Needed because
  // translating a key through a real keyboard layout takes the scan code as
  // well as the virtual key; the US-layout path ignored it.
  void InjectKeyboardEvent(int hwid,
                           RenderWidgetHostViewAura* view,
                           int vkey,
                           int scan,
                           bool is_down);

  // Schedules the next reconnect attempt with exponential backoff, and runs
  // one attempt.  Without this a dropped MouseMux server leaves the seat dead
  // until somebody re-toggles it by hand — the main unattended failure mode.
  void ScheduleReconnect();
  void AttemptReconnect();

  // Whether a connection was actually asked for.  Distinguishes a fault to
  // recover from (drop while true) from a deliberate disconnect (drop while
  // false), so we never fight the user's own toggle.
  bool should_be_connected_ = false;
  int reconnect_attempts_ = 0;
  base::OneShotTimer reconnect_timer_;

  // Delivers the final throttled position once motion stops.  Without it the
  // last move before the cursor comes to rest is never injected, leaving
  // hover state up to one throttle interval stale.  Flushes EVERY device that
  // has motion pending, so one timer serves all of them.
  void FlushPendingMotion();
  base::OneShotTimer motion_flush_timer_;

  bool native_input_blocked_ = false;

  std::unique_ptr<MouseMuxClient> client_;
  std::set<raw_ptr<RenderWidgetHostViewAura>> registered_views_;

  // Views registered before their window existed, to be looked at again
  // shortly.  Pointers only live here while registered: UnregisterView
  // erases, so the retry never dereferences a dead view.
  std::set<raw_ptr<RenderWidgetHostViewAura>> adopt_pending_;

  // Everything that belongs to ONE device pair.
  //
  // All of this used to be single global members, which was correct while
  // exactly one device could drive Chrome.  With several owners each of them
  // is per-device or users corrupt each other: a shared keyboard target sends
  // everyone's typing to whichever window was clicked last, a shared button
  // state makes one user's held button appear in another's drag, and a shared
  // pending-motion slot mixes two cursors' coordinates.
  //
  // Keyed by MOUSE hwid, which is the identity of a device PAIR — keyboard
  // events resolve through keyboard_to_mouse_hwid_ first.  A device with no
  // entry yet is default-constructed on first use by StateFor().
  struct DeviceState {
    // The view that received this device's mousedown.  Subsequent
    // mousemove-during-drag and mouseup must reach the SAME view or selection
    // and drag break.
    raw_ptr<RenderWidgetHostViewAura> drag_target_view = nullptr;

    // The page this device last clicked in.  A fallback only: keys go to
    // the active page of claimed_window, and this is consulted when that
    // window has no active page to offer.  Nothing else reads it.
    raw_ptr<RenderWidgetHostViewAura> keyboard_target_view = nullptr;

    // Blink button mask for the buttons this device is holding.
    int button_state = 0;

    // Which keys this device is holding, for modifier composition.
    std::set<int> pressed_keys;

    // Motion throttling, per device: two users moving at once would otherwise
    // overwrite each other's pending position.
    base::TimeTicks last_motion_inject_time;
    float pending_motion_x = 0;
    float pending_motion_y = 0;
    bool has_pending_motion = false;

    // Whether this device is captured — the server has stopped it producing
    // native Windows input.  Per device because capture is what allows several
    // owners to coexist, and the operator may hand it out one user at a time.
    bool captured = false;

#ifdef MOUSEMUX_KEYBOARD_LAYOUT
    // A dead key this device has pressed but not yet resolved — the acute in
    // acute-then-a.  Held PER DEVICE deliberately: ToUnicodeEx keeps its
    // composition state per thread, and the browser has one thread, so two
    // users mid-accent would otherwise compose into each other.  Zero means
    // nothing pending.
    //
    // The full key state is kept, not just the vkey, because re-feeding the
    // dead key has to reproduce the exact modifiers it was typed with or the
    // layout composes something different.
    int pending_dead_vkey = 0;
    int pending_dead_scan = 0;
    std::array<uint8_t, 256> pending_dead_key_state{};
#endif

    // The toplevel window this device last clicked in, kept alongside
    // keyboard_target_view because the view dies with a tab while the window
    // outlives it.  Without it there is no way, once a view is gone, to tell
    // "they closed a tab" from "their whole window went away" - and those want
    // opposite answers.
    gfx::AcceleratedWidget claimed_window = gfx::kNullAcceleratedWidget;

    // Every window this user owns; claimed_window is the CURRENT one, the
    // one all their input goes to.  A press inside another window of this
    // set makes that one current.  Windows join the set by being claimed,
    // or by being opened from one of them (see MaybeAdoptNewWindow).  Nobody
    // else can claim a window in somebody's set.  Most recent last.
    std::vector<gfx::AcceleratedWidget> owned_windows;

    // When any injected input from this device last arrived.  A window that
    // appears with no opener is attributed to the user whose input is the
    // most recent: their keystroke or click created it.
    base::TimeTicks last_input_time;

    // Whether the real mouse's input is dropped inside this user's windows.
    // On by default: an owned window is somebody's workplace.  The dialog
    // row's "Block native" checkbox turns it off for one user.
    bool block_native = true;

    // Where this device's last wheel notch went, so the scroll gesture can
    // be ended there (SendWheelEnd) once the notches stop.
    raw_ptr<RenderWidgetHostViewAura> last_wheel_view = nullptr;
    float last_wheel_x = 0;
    float last_wheel_y = 0;

    // When this pair last typed, and whether that keystroke was delivered.
    // Read by the dialog to show typing activity per user - a sentence
    // describing where everybody's keys went was unreadable at two users and
    // would be absurd at twenty.
    base::TimeTicks last_key_time;
    bool last_key_dropped = false;
  };

  std::map<int, DeviceState> device_state_;

  // One per device: fires 100 ms after its last wheel notch (SendWheelEnd).
  std::map<int, std::unique_ptr<base::OneShotTimer>> wheel_end_timers_;

  // Returns the state for a mouse hwid, creating it if this is the first
  // event from that device.
  DeviceState& StateFor(int mouse_hwid);

  // RenderWidgetHost::InputEventObserver: Chrome's answer to every input
  // event of every registered view, ours and native alike.  Logged under
  // MOUSEMUX_DEBUG as "ACK ..." lines; a no-op otherwise.
  void OnInputEventAck(const RenderWidgetHost& host,
                       blink::mojom::InputEventResultSource source,
                       blink::mojom::InputEventResultState state,
                       const blink::WebInputEvent& event) override;
  // Every event forwarded to a page, with its origin: injected-page,
  // injected-ui, or native.  Logged as "FWD ..." under MOUSEMUX_DEBUG.
  void OnInputEvent(const RenderWidgetHost& host,
                    const blink::WebInputEvent& event,
                    InputEventSource source) override;

  // The MouseMux user name a mouse or keyboard hwid belongs to, or "?".
  std::string UserNameOf(int hwid) const;

  // Adds `window` to this user's set (no-op if present).
  void AdoptWindow(int hwid, gfx::AcceleratedWidget window);
  // A press by this user on `window`: refused (false) when another user
  // owns it; otherwise the window is theirs, current, and their keyboard
  // follows it - to `page` when the press landed on one, else the window's
  // active page.  The one place a press changes what a user owns.
  bool ClaimForPress(int hwid,
                     gfx::AcceleratedWidget window,
                     RenderWidgetHostViewAura* page);
  // Second look at the views in adopt_pending_.
  void RetryPendingAdoptions();
  // A view has just registered: if its window is nobody's, give it to the
  // owner of the window that opened it, else to the user whose injected
  // input is the most recent.  Retries once if the window has no HWND yet.
  void MaybeAdoptNewWindow(RenderWidgetHostViewAura* view, bool retry);
  // Drops windows that no longer exist from a user's set and, if the current
  // one went, makes the most recent survivor current.  False if none is left.
  bool PruneOwnedWindows(int hwid);

  // Recomputes which windows drop native input: every window of every owner
  // whose block_native is set, plus everything while the legacy global flag
  // (control server) is on.  Applied to each registered view's event
  // handler and published to the views layer as a set of HWNDs.
  void ApplyNativeBlocking();

  // Ends the scroll gesture this device's wheel started: a wheel event with
  // phase kPhaseEnded and zero delta to the page the last notch went to.
  void SendWheelEnd(int hwid);
  // Publishes whether any injected mouse holds a button; see the .cc.
  void NoteSdkButtonState();

  // Resolves a KEYBOARD hwid to the mouse hwid of its pair, which is the key
  // device_state_ uses.  Returns -1 when the pairing is unknown.
  int MouseHwidForKeyboard(int keyboard_hwid) const;

  // Drops a view from every device's targets — on unregister the pointer is
  // about to dangle, and it may be the target of several devices at once.
  //
  // Also decides what a departing view MEANS for its owner: another tab of the
  // same window takes over, or, if the window itself has gone, that user is
  // released.
  void ForgetViewEverywhere(RenderWidgetHostViewAura* view);

  // A registered web view in |window|, other than |except|.  Used to retarget
  // a device when the tab it was working in closes but its window does not.
  RenderWidgetHostViewAura* OtherWebViewInWindow(
      gfx::AcceleratedWidget window,
      RenderWidgetHostViewAura* except) const;

  // The web view of the ACTIVE tab in |window|, or null.
  //
  // This is what a user's keyboard follows, rather than the view they happened
  // to click in.  A user owns a WINDOW; the tab showing inside it is Chrome's
  // business and changes without anybody claiming anything - the site opens
  // one, they switch to it, they close it again.  Binding input to a view
  // meant a tab the site opened for them inherited nothing and stopped
  // answering their keyboard.
  RenderWidgetHostViewAura* ActiveWebViewInWindow(
      gfx::AcceleratedWidget window) const;

  // Owner tracking.
  //
  // owners_ is authoritative: every device allowed to drive Chrome.
  //
  // owner_hwid_ is the PRIMARY owner — the first to claim, and the one the
  // single-owner API still reports: GetOwnerHwid(), the dialog title, the
  // control server's "owner" field.  It is always either -1 or a member of
  // owners_.  Kept because the launcher and external automation depend on that
  // API, and breaking it to add a feature they do not use would be rude.
  std::set<int> owners_;
  int owner_hwid_ = -1;

  // The gate every SDK event passes: may this device drive Chrome?
  bool IsOwner(int hwid) const { return owners_.count(hwid) > 0; }
  bool HasAnyOwner() const { return !owners_.empty(); }

  // Adds an owner.  With the experiment off this REPLACES any existing owner,
  // preserving single-owner semantics exactly.
  void AddOwner(int hwid);

  // Removes one owner, promoting another to primary if the primary left.
  void RemoveOwner(int hwid);

  // Motion throttling (~60fps, to avoid flooding the UI thread) is per device
  // now — see DeviceState.

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


  // Menu dismiss callback.
  MenuDismissCallback menu_dismiss_callback_;

  // Dialog visibility.  Tracked here rather than in the dialog so the control
  // server can report it without reaching across the layering boundary.
  VisibilityChangedCallback visibility_changed_callback_;
  DiagnosticsCallback diagnostics_callback_;
  ActiveViewForWindowCallback active_view_for_window_callback_;
  NoticeCallback notice_callback_;
  bool dialog_visible_ = true;

  // User info cache (hwid_mouse -> UserInfo).
  std::map<int, MouseMuxClient::UserInfo> user_info_;

  // Keyboard hwid to mouse hwid mapping (for looking up owner).
  std::map<int, int> keyboard_to_mouse_hwid_;

  // Rate-limit user list refresh requests for unknown keyboards.
  base::TimeTicks last_user_list_request_;

  // Keyboard hwids that have delivered at least one keystroke.  Read by
  // GetOwners() to tell "no keyboard assigned" from "assigned but silent" —
  // different faults with different fixes, and indistinguishable without it.
  std::set<int> keyboards_seen_;

  // See GetInjectionStats().  Keys go into the window as WM_MOUSEMUX_KEY*
  // messages, so the counters say what was posted, not what a renderer did.
  struct InjectionStats {
    int posted_keys = 0;     // WM_MOUSEMUX_KEYDOWN/KEYUP posted
    int posted_chars = 0;    // WM_MOUSEMUX_CHAR posted
    int no_host = 0;         // view had no RenderWidgetHostImpl
    int renderer_dead = 0;   // no live renderer to send to
    bool last_view_focused = false;
    bool last_page_focused = false;
  };
  std::map<int, InjectionStats> injection_stats_;

  // Keyboard routing history for the dialog — see KeyRoute.
  static constexpr size_t kMaxKeyRoutes = 6;
  std::vector<KeyRoute> key_routes_;
  void RecordKeyRoute(int keyboard_hwid,
                      int mouse_hwid,
                      const std::u16string& window_title,
                      bool dropped);

  // Held keys, drag target and keyboard target are per device now — see
  // DeviceState.

  // InputRouter pending-state tracking for stuck ACK detection.
  // When the InputRouter has had pending events for too long, we reset it.
  // When each view's InputRouter started holding un-acked events.
  //
  // Per VIEW, not one at a time.  It used to be a single view pointer and a
  // single timestamp: whenever injection touched a different view the clock
  // restarted, so with two people working in two windows the "stuck for more
  // than 300ms" test could never come true, and a genuinely stuck router was
  // never reset.  That view then stopped answering clicks and the wheel while
  // its cursor carried on moving - which is an OS thing and needs nothing
  // from us - so it looked like input decaying after a while rather than one
  // view wedging.
  std::map<raw_ptr<RenderWidgetHostViewAura>, base::TimeTicks> pending_since_;

  // Resets |view|'s InputRouter if it has been waiting too long, and returns
  // whether anything is still pending afterwards.
  bool RecoverStuckInputRouter(RenderWidgetHostViewAura* view,
                               RenderWidgetHostImpl* host,
                               bool has_pending);

  // Notify ownership changed.
  void NotifyOwnershipChanged();

  // Control server for external automation (--mousemux-control-port).
  std::unique_ptr<MouseMuxControlServer> control_server_;
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_INPUT_CONTROLLER_H_
