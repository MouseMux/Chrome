// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_CONTROL_DIALOG_H_
#define CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_CONTROL_DIALOG_H_

// Uncomment the following line to enable full debug UI and logging.
// #define MOUSEMUX_DEBUG

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/window/dialog_delegate.h"

namespace views {
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

 private:
  // Keyboard event handler - returns true to consume the event.
  bool OnKeyboardEvent(int vkey, bool shift, bool ctrl, bool alt, bool is_down);
  // views::DialogDelegateView overrides.
  bool ShouldShowWindowTitle() const override;
  bool ShouldShowWindowIcon() const override;
  ui::ImageModel GetWindowIcon() override;
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;

  void SetupContents();

  // Called when toggle buttons are changed.
  void OnNativeInputToggled();
  void OnMouseMuxToggled();

  // Called when Release Owner button is clicked.
  void OnReleaseOwnerClicked();

  // Called when Capture button is clicked.
  void OnCaptureClicked();

  // Called when hotkey dropdown selection changes.
  void OnHotkeyChanged();

  // Update capture button state based on owner and capture state.
  void UpdateCaptureButton();

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

  raw_ptr<views::ToggleButton> native_input_toggle_ = nullptr;
  raw_ptr<views::ToggleButton> mousemux_toggle_ = nullptr;
  raw_ptr<views::Label> native_input_status_label_ = nullptr;
  raw_ptr<views::Label> mousemux_status_label_ = nullptr;
  raw_ptr<views::MdTextButton> release_owner_button_ = nullptr;
  raw_ptr<views::MdTextButton> capture_button_ = nullptr;
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

  // Selected hotkey index (0 = Shift+Escape, etc.)
  size_t selected_hotkey_index_ = 0;

  static MouseMuxControlDialog* instance_;
};

}  // namespace mouse_mux

#endif  // CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_CONTROL_DIALOG_H_
