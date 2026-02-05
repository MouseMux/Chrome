// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/mouse_mux/mouse_mux_control_dialog.h"

#ifdef MOUSEMUX_DEBUG
#include <fstream>
#endif
#include <utility>

#include <windows.h>

#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/win/icon_util.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/base/models/image_model.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/gfx/font_list.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/button/toggle_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#ifdef MOUSEMUX_DEBUG
#include "ui/views/controls/textarea/textarea.h"
#endif
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/base/models/combobox_model.h"

namespace mouse_mux {

namespace {

// Hotkey options for releasing capture.
struct HotkeyOption {
  const char16_t* label;
  int vkey;       // VK_ESCAPE or VK_F12
  bool shift;
  bool ctrl;
  bool alt;
};

constexpr HotkeyOption kHotkeyOptions[] = {
    {u"Shift+Escape", VK_ESCAPE, true, false, false},
    {u"Ctrl+Shift+Escape", VK_ESCAPE, true, true, false},
    {u"Alt+Shift+Escape", VK_ESCAPE, true, false, true},
    {u"Shift+F12", VK_F12, true, false, false},
    {u"Alt+Shift+F12", VK_F12, true, false, true},
};

// ComboboxModel for hotkey dropdown.
class HotkeyComboboxModel : public ui::ComboboxModel {
 public:
  HotkeyComboboxModel() = default;
  ~HotkeyComboboxModel() override = default;

  size_t GetItemCount() const override {
    return std::size(kHotkeyOptions);
  }

  std::u16string GetItemAt(size_t index) const override {
    if (index < std::size(kHotkeyOptions)) {
      return kHotkeyOptions[index].label;
    }
    return u"";
  }

  std::optional<size_t> GetDefaultIndex() const override {
    return 0;  // Shift+Escape
  }
};

#ifdef MOUSEMUX_DEBUG
constexpr int kBuildNumber = 17;
constexpr int kDialogWidth = 600;
constexpr int kDialogHeight = 500;
constexpr int kLogFlushThreshold = 5;
const char kLogFilePath[] = "O:/tmp/mousemux_debug.log";
#else
constexpr int kDialogWidth = 320;
constexpr int kDialogHeight = 200;
const char kVersion[] = "2.2.46";
const char kBuildDate[] = "5 Feb 2026";
#endif

constexpr int kSpacing = 12;
constexpr int kToggleSpacing = 8;

}  // namespace

// Static instance pointer.
MouseMuxControlDialog* MouseMuxControlDialog::instance_ = nullptr;


MouseMuxControlDialog::MouseMuxControlDialog() {
  instance_ = this;

#ifdef MOUSEMUX_DEBUG
  SetTitle(base::ASCIIToUTF16(
      base::StringPrintf("MouseMux Control - Build #%d", kBuildNumber)));
#else
  SetTitle(u"MouseMux for Chrome");
#endif

  // Just a Close button - settings are applied immediately via toggles.
  SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
  SetButtonLabel(ui::mojom::DialogButton::kCancel, u"Close");

  SetModalType(ui::mojom::ModalType::kNone);
  set_draggable(true);
  SetBorder(views::CreateEmptyBorder(gfx::Insets(kSpacing)));

  SetupContents();

  // Register callbacks with controller.
  auto* controller = content::MouseMuxInputController::GetInstance();

#ifdef MOUSEMUX_DEBUG
  controller->SetDebugLogCallback(
      base::BindRepeating(&MouseMuxControlDialog::LogDebug,
                          base::Unretained(this)));
#endif

  // Register ownership changed callback.
  controller->SetOwnershipChangedCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnOwnershipChanged,
                          base::Unretained(this)));

  // Register connection state callback.
  controller->SetConnectionChangedCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnConnectionStateChanged,
                          base::Unretained(this)));

  // Register capture state callback.
  controller->SetCaptureChangedCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnCaptureStateChanged,
                          base::Unretained(this)));

  // Register keyboard event callback for hotkey detection.
  controller->SetKeyboardEventCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnKeyboardEvent,
                          base::Unretained(this)));

  // Load window icon from exe directory.
  base::FilePath exe_dir;
  if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    base::FilePath icon_path = exe_dir.Append(L"icon.ico");
    HICON hicon = static_cast<HICON>(
        ::LoadImageW(nullptr, icon_path.value().c_str(), IMAGE_ICON,
                     32, 32, LR_LOADFROMFILE));
    if (hicon) {
      SkBitmap bitmap = IconUtil::CreateSkBitmapFromHICON(hicon);
      if (!bitmap.isNull()) {
        window_icon_ = gfx::ImageSkia::CreateFromBitmap(bitmap, 1.0f);
      }
      ::DestroyIcon(hicon);
    }
  }

#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf("MouseMux Control Dialog initialized - BUILD #%d", kBuildNumber));
  FlushLogBuffer();  // Immediately write initialization message.
#endif
}

MouseMuxControlDialog::~MouseMuxControlDialog() {
#ifdef MOUSEMUX_DEBUG
  LogDebug("MouseMux Control Dialog destroyed");
#endif
  instance_ = nullptr;
}

// static
void MouseMuxControlDialog::Show() {
  auto* dialog = new MouseMuxControlDialog();
  auto* widget = views::DialogDelegate::CreateDialogWidget(
      dialog, gfx::NativeWindow(), gfx::NativeView());

  // Position the widget near the top-left of the screen.
  widget->SetBounds(gfx::Rect(50, 50, kDialogWidth, kDialogHeight));

  // Show and activate.
  widget->Show();
  widget->Activate();
}

// static
MouseMuxControlDialog* MouseMuxControlDialog::GetInstance() {
  return instance_;
}

void MouseMuxControlDialog::LogDebug(const std::string& message) {
#ifdef MOUSEMUX_DEBUG
  // Get timestamp immediately.
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.LocalExplode(&exploded);
  std::string timestamped = base::StringPrintf(
      "[%02d:%02d:%02d.%03d] %s", exploded.hour, exploded.minute,
      exploded.second, exploded.millisecond, message.c_str());

  // Buffer log messages instead of writing each one individually.
  // This prevents UI freezes from excessive file I/O.
  log_buffer_.push_back(timestamped);

  // Flush buffer periodically.
  if (log_buffer_.size() >= kLogFlushThreshold) {
    FlushLogBuffer();
  }

  // Only update UI for important messages (not motion-related).
  // Motion events are too frequent and cause UI lag.
  if (message.find("MOTION") == std::string::npos &&
      message.find("FindView") == std::string::npos) {
    if (debug_log_) {
      std::u16string current(debug_log_->GetText());
      // Keep only last 5000 chars to prevent memory bloat.
      if (current.size() > 5000) {
        current = current.substr(current.size() - 4000);
      }
      if (!current.empty()) {
        current += u"\n";
      }
      current += base::ASCIIToUTF16(timestamped);
      debug_log_->SetText(current);
    }
  }
#endif  // MOUSEMUX_DEBUG
}

#ifdef MOUSEMUX_DEBUG
void MouseMuxControlDialog::FlushLogBuffer() {
  if (log_buffer_.empty()) {
    return;
  }
  std::ofstream file(kLogFilePath, std::ios::app);
  if (file.is_open()) {
    for (const auto& msg : log_buffer_) {
      file << msg << "\n";
    }
    file.close();
  }
  log_buffer_.clear();
}

void MouseMuxControlDialog::WriteToLogFile(const std::string& message) {
  std::ofstream file(kLogFilePath, std::ios::app);
  if (file.is_open()) {
    file << message << std::endl;
    file.close();
  }
}
#endif  // MOUSEMUX_DEBUG

void MouseMuxControlDialog::SetupContents() {
  // Set up the layout for this view.
  auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), kSpacing));
  layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kStart);
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);

#ifdef MOUSEMUX_DEBUG
  // Title label.
  auto* title_label = AddChildView(std::make_unique<views::Label>(
      u"MouseMux Integration Controls", views::style::CONTEXT_DIALOG_TITLE,
      views::style::STYLE_PRIMARY));
  title_label->SetFontList(
      gfx::FontList().Derive(4, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
  title_label->SetHorizontalAlignment(gfx::ALIGN_CENTER);
#endif

  // Native input toggle row.
  auto* native_row = AddChildView(std::make_unique<views::View>());
  auto* native_layout =
      native_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  native_layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kStart);
  native_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  native_input_toggle_ =
      native_row->AddChildView(std::make_unique<views::ToggleButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnNativeInputToggled,
                              base::Unretained(this))));
  native_input_toggle_->SetAccessibleName(u"Disable Native Mouse Input");

  auto* native_label = native_row->AddChildView(
      std::make_unique<views::Label>(u"Disable Native Mouse Input"));
  native_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  native_layout->SetFlexForView(native_label, 1);

  native_input_status_label_ =
      native_row->AddChildView(std::make_unique<views::Label>(u"Off"));

  // MouseMux toggle row.
  auto* mousemux_row = AddChildView(std::make_unique<views::View>());
  auto* mousemux_layout =
      mousemux_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  mousemux_layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kStart);
  mousemux_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  mousemux_toggle_ =
      mousemux_row->AddChildView(std::make_unique<views::ToggleButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnMouseMuxToggled,
                              base::Unretained(this))));
  mousemux_toggle_->SetAccessibleName(u"Connect to MouseMux");

  auto* mousemux_label = mousemux_row->AddChildView(
      std::make_unique<views::Label>(u"Connect to MouseMux"));
  mousemux_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  mousemux_layout->SetFlexForView(mousemux_label, 1);

  mousemux_status_label_ =
      mousemux_row->AddChildView(std::make_unique<views::Label>(u"Disconnected"));

#ifdef MOUSEMUX_DEBUG
  // Info label with server address.
  auto* info_label = AddChildView(std::make_unique<views::Label>(
      u"Toggle settings take effect immediately. Server: ws://localhost:41001",
      views::style::CONTEXT_DIALOG_BODY_TEXT, views::style::STYLE_SECONDARY));
  info_label->SetHorizontalAlignment(gfx::ALIGN_CENTER);
#endif

  // Capture row: [Capture Mouse] button + [Hotkey dropdown] + [Release Owner] button
  auto* capture_row = AddChildView(std::make_unique<views::View>());
  auto* capture_layout =
      capture_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  capture_layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kStart);
  capture_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  capture_button_ = capture_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnCaptureClicked,
                              base::Unretained(this)),
          u"Capture Mouse"));
  capture_button_->SetEnabled(false);  // Disabled until we have an owner.

  // Hotkey dropdown label.
  capture_row->AddChildView(std::make_unique<views::Label>(u"Release:"));

  // Hotkey dropdown.
  hotkey_model_ = std::make_unique<HotkeyComboboxModel>();
  hotkey_dropdown_ = capture_row->AddChildView(
      std::make_unique<views::Combobox>(hotkey_model_.get()));
  hotkey_dropdown_->SetCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnHotkeyChanged,
                          base::Unretained(this)));
  hotkey_dropdown_->SetSelectedIndex(0);  // Default: Shift+Escape

  // Spacer to push Release Owner button to the right.
  auto* spacer = capture_row->AddChildView(std::make_unique<views::View>());
  capture_layout->SetFlexForView(spacer, 1);

  release_owner_button_ = capture_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnReleaseOwnerClicked,
                              base::Unretained(this)),
          u"Release Owner"));
  release_owner_button_->SetEnabled(false);  // Disabled until we have an owner.

#ifndef MOUSEMUX_DEBUG
  // Version and build date label in the button row at the lower left, light gray.
  auto build_label = std::make_unique<views::Label>(
      base::ASCIIToUTF16(base::StringPrintf("v%s (%s)", kVersion, kBuildDate)),
      views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_DISABLED);
  build_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  SetExtraView(std::move(build_label));
#endif

#ifdef MOUSEMUX_DEBUG
  // Debug log section.
  auto* debug_label = AddChildView(std::make_unique<views::Label>(
      u"Debug Log:", views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_PRIMARY));
  debug_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);

  // Debug textarea.
  debug_log_ = AddChildView(std::make_unique<views::Textarea>());
  debug_log_->SetPlaceholderText(u"Debug output will appear here...");
  debug_log_->SetReadOnly(true);
  debug_log_->SetFontList(gfx::FontList("Consolas, 10px"));

  // Make the textarea expand to fill available space.
  layout->SetFlexForView(debug_log_, 1);
#endif
}

bool MouseMuxControlDialog::ShouldShowWindowTitle() const {
  return true;
}

bool MouseMuxControlDialog::ShouldShowWindowIcon() const {
  return !window_icon_.isNull();
}

ui::ImageModel MouseMuxControlDialog::GetWindowIcon() {
  if (window_icon_.isNull()) {
    return ui::ImageModel();
  }
  return ui::ImageModel::FromImageSkia(window_icon_);
}

gfx::Size MouseMuxControlDialog::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(kDialogWidth, kDialogHeight);
}

void MouseMuxControlDialog::OnNativeInputToggled() {
  bool is_on = native_input_toggle_->GetIsOn();
  native_input_status_label_->SetText(is_on ? u"Blocking" : u"Off");

  LogDebug(std::string("Native input blocking: ") + (is_on ? "ENABLED" : "DISABLED"));

  // Apply immediately to controller.
  auto* controller = content::MouseMuxInputController::GetInstance();
  controller->SetNativeInputBlocked(is_on);
}

void MouseMuxControlDialog::OnMouseMuxToggled() {
  bool is_on = mousemux_toggle_->GetIsOn();
  mousemux_status_label_->SetText(is_on ? u"Connecting..." : u"Disconnected");

  LogDebug(std::string("MouseMux connection: ") + (is_on ? "CONNECTING" : "DISCONNECTING"));

  // Apply immediately to controller.
  auto* controller = content::MouseMuxInputController::GetInstance();
  controller->SetMouseMuxEnabled(is_on);
}

void MouseMuxControlDialog::OnConnectionStateChanged(bool connected) {
  if (mousemux_status_label_) {
    mousemux_status_label_->SetText(connected ? u"Connected" : u"Disconnected");
  }
}

void MouseMuxControlDialog::OnCaptureStateChanged(bool captured) {
  is_captured_ = captured;
  UpdateCaptureButton();
  UpdateTitle();
  LogDebug(std::string("Capture state changed: ") + (captured ? "CAPTURED" : "RELEASED"));
}

void MouseMuxControlDialog::OnCaptureClicked() {
  auto* controller = content::MouseMuxInputController::GetInstance();
  if (is_captured_) {
    LogDebug("Release Capture button clicked");
    controller->ReleaseCapture();
  } else {
    LogDebug("Capture Mouse button clicked");
    controller->CaptureOwner();
  }
}

void MouseMuxControlDialog::OnHotkeyChanged() {
  if (hotkey_dropdown_) {
    selected_hotkey_index_ = hotkey_dropdown_->GetSelectedIndex().value_or(0);
    LogDebug(base::StringPrintf("Hotkey changed to index %zu: %s",
                                 selected_hotkey_index_,
                                 base::UTF16ToASCII(kHotkeyOptions[selected_hotkey_index_].label).c_str()));
  }
}

void MouseMuxControlDialog::UpdateCaptureButton() {
  if (capture_button_) {
    // Button enabled only when we have an owner.
    capture_button_->SetEnabled(owner_hwid_ != -1);
    // Label changes based on capture state.
    capture_button_->SetText(is_captured_ ? u"Release Capture" : u"Capture Mouse");
  }
}

bool MouseMuxControlDialog::OnKeyboardEvent(int vkey, bool shift, bool ctrl, bool alt, bool is_down) {
  // Only check hotkey when captured and on key down.
  if (!is_captured_ || !is_down) {
    return false;
  }
  if (selected_hotkey_index_ >= std::size(kHotkeyOptions)) {
    return false;
  }
  const auto& hotkey = kHotkeyOptions[selected_hotkey_index_];
  if (vkey == hotkey.vkey && shift == hotkey.shift &&
      ctrl == hotkey.ctrl && alt == hotkey.alt) {
    LogDebug("Release hotkey detected - releasing capture");
    auto* controller = content::MouseMuxInputController::GetInstance();
    controller->ReleaseCapture();
    return true;  // Consume the event.
  }
  return false;
}

void MouseMuxControlDialog::OnReleaseOwnerClicked() {
  LogDebug("Release Owner button clicked");
  auto* controller = content::MouseMuxInputController::GetInstance();
  controller->ReleaseOwnership();
}

void MouseMuxControlDialog::OnOwnershipChanged(int hwid, const std::string& name) {
  owner_hwid_ = hwid;
  owner_name_ = name;

  // Update button states.
  if (release_owner_button_) {
    release_owner_button_->SetEnabled(hwid != -1);
  }
  UpdateCaptureButton();

  // Update title.
  UpdateTitle();

  if (hwid != -1) {
    LogDebug(base::StringPrintf("Ownership changed: hwid=0x%x name=%s",
                                 hwid, name.empty() ? "(unknown)" : name.c_str()));
  } else {
    LogDebug("Ownership released - waiting for first click");
  }
}

void MouseMuxControlDialog::UpdateTitle() {
  std::string title;
  const char* capture_suffix = is_captured_ ? " [CAPTURED]" : "";
#ifdef MOUSEMUX_DEBUG
  if (owner_hwid_ == -1) {
    title = base::StringPrintf("MouseMux Control - Build #%d (No Owner)", kBuildNumber);
  } else if (owner_name_.empty()) {
    title = base::StringPrintf("MouseMux Control - Build #%d - Owner: 0x%X%s",
                               kBuildNumber, owner_hwid_, capture_suffix);
  } else {
    title = base::StringPrintf("MouseMux Control - Build #%d - Owner: %s (0x%X)%s",
                               kBuildNumber, owner_name_.c_str(), owner_hwid_, capture_suffix);
  }
#else
  if (owner_hwid_ == -1) {
    title = "MouseMux for Chrome";
  } else if (owner_name_.empty()) {
    title = base::StringPrintf("MouseMux for Chrome - Owner: 0x%X%s", owner_hwid_, capture_suffix);
  } else {
    title = base::StringPrintf("MouseMux for Chrome - %s (0x%X)%s",
                               owner_name_.c_str(), owner_hwid_, capture_suffix);
  }
#endif
  SetTitle(base::ASCIIToUTF16(title));

  // Force widget to update title.
  if (GetWidget()) {
    GetWidget()->UpdateWindowTitle();
  }
}

BEGIN_METADATA(MouseMuxControlDialog)
END_METADATA

}  // namespace mouse_mux
