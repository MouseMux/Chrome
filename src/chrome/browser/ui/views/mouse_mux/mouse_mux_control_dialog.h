// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_CONTROL_DIALOG_H_
#define CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_CONTROL_DIALOG_H_

// MOUSEMUX_DEBUG and other defines live in mouse_mux_config.h (tiny header).
// We include ONLY the config here to avoid pulling in the full controller
// header, which would cause cascade rebuilds of everything in chrome/browser/ui.
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_config.h"

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/window/dialog_delegate.h"

namespace views {
class Checkbox;
class Combobox;
class MdTextButton;
class ToggleButton;
class Label;
#ifdef MOUSEMUX_DEBUG
class Textarea;
#endif
}  // namespace views

namespace ui {
class ComboboxModel;
}

namespace mouse_mux {

// Dialog that provides controls for MouseMux integration.
// Shows at startup when kMouseMuxIntegration feature is enabled.
// Stays open to allow real-time control of MouseMux settings.
class MouseMuxControlDialog : public views::DialogDelegateView {
  METADATA_HEADER(MouseMuxControlDialog, views::DialogDelegateView)

 public:
  MouseMuxControlDialog();
  MouseMuxControlDialog(const MouseMuxControlDialog&) = delete;
  MouseMuxControlDialog& operator=(const MouseMuxControlDialog&) = delete;
  ~MouseMuxControlDialog() override;

  // Creates and shows the dialog. The dialog stays open for runtime control.
  static void Show();

  // Get the singleton instance (may be null if dialog not shown).
  static MouseMuxControlDialog* GetInstance();

  // Add a debug message to the log area and file.
  void LogDebug(const std::string& message);

  // Called when connection state changes.
  void OnConnectionStateChanged(bool connected);

  // Called when capture state changes.
  void OnCaptureStateChanged(bool captured);

  // Called when native input blocking state changes (e.g. from control server).
  void OnNativeBlockingChanged(bool blocked);

  // Shows or hides the dialog window.  Driven by the controller so the
  // control server can reach it without content depending on ui/views.
  void OnVisibilityChanged(bool visible);

 private:
  // Keyboard event handler - returns true to consume the event.
  bool OnKeyboardEvent(int vkey, bool shift, bool ctrl, bool alt, bool is_down);
  // views::DialogDelegateView overrides.
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;
  bool ShouldShowWindowTitle() const override;
  bool ShouldShowWindowIcon() const override;
  bool ShouldShowCloseButton() const override;
  ui::ImageModel GetWindowIcon() override;

  void SetupContents();

  // Called when controls change.
  void OnNativeInputToggled();
  void OnConnectClicked();
  void OnHardLockToggled();

  // Drops every owner. A reset rather than a daily action, which is why it
  // lives behind Advanced: releasing capture is what ends a shift, dropping
  // ownership is what you do when something is wrong.
  void OnReleaseOwnerClicked();

  // Refreshes the connection line and the user count in the list heading.
  void UpdateStatusLine();

  // Greys out everything that cannot do anything useful while the connection
  // is down.  Connect stays live, since it is the way back, and so do Collapse
  // and the window's own close button.
  void UpdateEnabledState();

  // views::WidgetDelegate:
  void WindowClosing() override;

  // Per-user capture, from the checkbox on that user's row.
  void OnOwnerCaptureToggled(int hwid, views::Checkbox* box);

  // Collapses the dialog to a small strip, and expands it again.  Deliberately
  // not the same as the control server's "visible" flag: collapsing leaves a
  // window on screen to click, so a user can always get back.  Fully hiding is
  // automation-only, because only automation can undo it.
  void OnCollapseClicked();

  // Opens the help window - see MouseMuxHelpDialog.
  void OnHelpClicked();
  void SetCollapsed(bool collapsed);

  // Called when hotkey dropdown selection changes.
  void OnHotkeyChanged();

  // Brings the per-owner rows up to date with the controller's owner list.
  //
  // Rebuilds the views only when the SET of users changes.  Everything else -
  // a title changing as somebody browses, a capture being taken, a keyboard
  // starting to type - is written into the existing labels instead.
  //
  // That distinction matters at scale rather than at two users: this runs on a
  // one-second timer, and with twenty people browsing, the titles alone would
  // otherwise rebuild every view in the list every second.  Destroying views
  // also destroys the tooltip under the pointer, so a list that rebuilds
  // constantly is a list whose tooltips can never be read.
  void RebuildOwnerList();

  // Which users are in the list, in order.  Changing this is what forces a
  // rebuild; changing anything else does not.
  std::string OwnerMembership() const;

  // Everything the rows display, in a form cheap to compare - including the
  // window title, so an in-place update is skipped when nothing at all moved.
  std::string OwnerSignature() const;

  // Re-asserts layout and a frame for this dialog.
  void EnsurePainted();

  // What the owner rows actually are right now - bounds, visibility, text -
  // reported through the control server.  See ViewDiagnostics().
  std::string ViewDiagnostics() const;

  // Rebuilds on a FRESH task rather than inline.  See the definition: a
  // rebuild run from the injected-input call stack draws only half of what it
  // built.
  void ScheduleRebuild();

  // Fills in the line under the owner list — see keyboard_note_label_.
  // Takes no owner list, and asks the controller for its own: this header
  // deliberately does not include the controller's, so it cannot name
  // OwnerInfo, and pulling that header in here would rebuild all of
  // chrome/browser/ui on every controller edit.
  void UpdateKeyboardNote();

  // Per-owner row actions, all keyed by |hwid|.
  //
  // Deliberately NOT by window handle: rows outlive the window a user happened
  // to be in when the row was built, so a captured handle can name a window
  // they have since left or closed.  The current one is looked up when the
  // button is pressed.
  void OnOwnerReleaseClicked(int hwid);
  void OnOwnerCloseWindowClicked(int hwid);

  // Hand out a window: a copy of the current tab in another window of THIS
  // Chrome, already signed in and sharing the session, or a new seat — its
  // own process and profile, started by this executable, sharing nothing.
  void OnNewWindowClicked();
  void OnNewSeatClicked();

  // Called when ownership changes.
  void OnOwnershipChanged(int hwid, const std::string& name);

  // Update the dialog title with owner info.
  void UpdateTitle();

#ifdef MOUSEMUX_DEBUG
  // Write to log file.
  void WriteToLogFile(const std::string& message);

  // Flush buffered log messages to file.
  void FlushLogBuffer();

  // Buffer for batching log writes to reduce I/O overhead.
  std::vector<std::string> log_buffer_;
#endif

  // Connection: a status line and one button, rather than a toggle that looked
  // identical to two unrelated ones.
  // Green when connected, grey when not - the one thing an operator
  // glances at before anything else.
  raw_ptr<views::Label> connection_led_ = nullptr;
  raw_ptr<views::Label> mousemux_status_label_ = nullptr;
  raw_ptr<views::MdTextButton> connect_button_ = nullptr;

  // Held so they can be greyed out while disconnected - see
  // UpdateEnabledState().
  raw_ptr<views::MdTextButton> new_window_button_ = nullptr;
  raw_ptr<views::MdTextButton> new_seat_button_ = nullptr;
  raw_ptr<views::MdTextButton> release_all_button_ = nullptr;

  // "Users (N)" - the count belongs in the heading, not in a separate label.
  raw_ptr<views::Label> users_header_label_ = nullptr;

  // A preference about users, so it sits with them rather than among the
  // connection controls, and a checkbox rather than a fourth identical toggle.
  raw_ptr<views::Checkbox> hard_lock_checkbox_ = nullptr;

  // Options that apply to everybody, in the bottom pane.  Native input
  // blocking is deliberately NOT per user: the Windows messages it drops
  // carry no device identity, which is the entire reason MouseMux exists.
  // The per-user equivalent is capture, on each row.
  raw_ptr<views::Checkbox> native_input_checkbox_ = nullptr;

  // One built row, kept so it can be updated without being rebuilt.
  struct OwnerRow {
    int hwid = -1;
    raw_ptr<views::Label> dot = nullptr;
    raw_ptr<views::Label> name = nullptr;
    raw_ptr<views::Label> keyboard = nullptr;
    raw_ptr<views::Label> where = nullptr;
    raw_ptr<views::Checkbox> capture = nullptr;
    raw_ptr<views::MdTextButton> close = nullptr;
  };
  std::vector<OwnerRow> owner_rows_;

  // The user set the rows were built from - see OwnerMembership().
  std::string owner_membership_;

  // Container the per-owner rows are rebuilt into.
  raw_ptr<views::View> owner_list_ = nullptr;

  // What the rows were last drawn from - see OwnerSignature().
  std::string owner_signature_;

  // One line under the owner list: either a warning that a user has no
  // keyboard assigned in MouseMux, or where recent typing actually went.
  raw_ptr<views::Label> keyboard_note_label_ = nullptr;

  // Owners' window titles change as they browse, and nothing notifies us when
  // a user clicks into a different window, so the list is refreshed on a
  // timer as well as on ownership and capture callbacks.  Only runs while the
  // dialog is visible.
  base::RepeatingTimer owner_refresh_timer_;

  // Shown only while collapsed: the icon plus the one control left to click.
  raw_ptr<views::View> expand_row_ = nullptr;
  // Build info in the frame's button row; hidden while collapsed.
  raw_ptr<views::View> build_label_ = nullptr;
  bool collapsed_ = false;
  raw_ptr<views::Combobox> hotkey_dropdown_ = nullptr;
  std::unique_ptr<ui::ComboboxModel> hotkey_model_;
#ifdef MOUSEMUX_DEBUG
  raw_ptr<views::Textarea> debug_log_ = nullptr;
#endif

  // Window icon.
  gfx::ImageSkia window_icon_;

  // Current owner info.
  int owner_hwid_ = -1;
  std::string owner_name_;

  // Current capture state.
  bool is_captured_ = false;

  // Which device opened the currently active context menu, and which device
  // clicked most recently while no menu was open.  Chrome has ONE active menu
  // per process, so without this any user's click closes any user's menu.
  // -1 means unattributed.
  int menu_owner_hwid_ = -1;
  int pending_menu_hwid_ = -1;

  // Selected hotkey index (0 = Shift+Escape, etc.)
  size_t selected_hotkey_index_ = 0;

  static MouseMuxControlDialog* instance_;

  // For ScheduleRebuild(): the dialog can be closed between posting the task
  // and running it.
  base::WeakPtrFactory<MouseMuxControlDialog> weak_factory_{this};
};

}  // namespace mouse_mux

#endif  // CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_CONTROL_DIALOG_H_
