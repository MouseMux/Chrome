// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/mouse_mux/mouse_mux_control_dialog.h"

#ifdef MOUSEMUX_DEBUG
#include <fstream>
#endif
#include <utility>

#include <windows.h>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/win/icon_util.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/base/models/image_model.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "ui/display/screen.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/gfx/font_list.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/button/toggle_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/window/non_client_view.h"
#ifdef MOUSEMUX_DEBUG
#include "ui/views/controls/textarea/textarea.h"
#endif
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/views/controls/menu/menu_controller.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/base/models/combobox_model.h"

#ifdef MOUSEMUX_NATIVE_BLOCK
namespace content {
extern HWND g_mousemux_dialog_hwnd;
}
#endif

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

constexpr int kBuildNumber = 54;

// Product name, used for the window title.  The build number lives in the
// footer with the build date rather than the title, which should read as a
// product name and not a version string.
#ifdef MOUSEMUX_DEBUG_TRACE
// Trace builds write input coordinates and key codes to disk.  Say so in the
// title so one can never be mistaken for a normal build.
constexpr char kProductName[] =
    "MouseMux Multi-Seat Chrome Control [TRACE BUILD - LOGGING TO DISK]";
#else
constexpr char kProductName[] = "MouseMux Multi-Seat Chrome Control";
#endif

// Title bar icon, loaded from icon.ico next to chrome.exe.  Shown to the left
// of the dialog title.
constexpr int kWindowIconSize = 16;

#ifdef MOUSEMUX_DEBUG
constexpr int kDialogWidth = 600;
constexpr int kDialogHeight = 500;
constexpr int kLogFlushThreshold = 5;
const char kLogFilePath[] = MOUSEMUX_DEBUG_LOG_PATH;
#else
constexpr int kDialogWidth = 420;
constexpr int kDialogHeight = 240;

// Collapsed strip: no title bar, no close button, no build footer — just the
// icon and one button, so it can be much smaller than the dialog.
constexpr int kCollapsedWidth = 150;
constexpr int kCollapsedHeight = 52;
const char kBuildDate[] = "2026-08-04 TRACE";
#endif

constexpr int kSpacing = 12;
constexpr int kToggleSpacing = 8;

}  // namespace

namespace {

// Loads icon.ico from the executable directory at the requested pixel size.
// Returns a null image when the file is absent — icon.ico is deployed next to
// chrome.exe rather than compiled in, so a missing file is a normal condition
// and callers simply skip the icon.
gfx::ImageSkia LoadAppIcon(int size) {
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return gfx::ImageSkia();
  }
  const base::FilePath icon_path = exe_dir.Append(L"icon.ico");
  HICON hicon = static_cast<HICON>(
      ::LoadImageW(nullptr, icon_path.value().c_str(), IMAGE_ICON, size, size,
                   LR_LOADFROMFILE));
  if (!hicon) {
    return gfx::ImageSkia();
  }
  SkBitmap bitmap = IconUtil::CreateSkBitmapFromHICON(hicon);
  ::DestroyIcon(hicon);
  if (bitmap.isNull()) {
    return gfx::ImageSkia();
  }
  return gfx::ImageSkia::CreateFromBitmap(bitmap, 1.0f);
}

}  // namespace

// Static instance pointer.
MouseMuxControlDialog* MouseMuxControlDialog::instance_ = nullptr;


MouseMuxControlDialog::MouseMuxControlDialog() {
  instance_ = this;

  // Wrapped in std::string deliberately: ASCIIToUTF16 static_asserts against
  // compile-time char arrays to push callers towards u"..." literals, but the
  // same constant is also needed as char for the StringPrintf titles below.
  SetTitle(base::ASCIIToUTF16(std::string(kProductName)));

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

  // Register visibility callback so the control server can show/hide us.
  controller->SetVisibilityChangedCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnVisibilityChanged,
                          base::Unretained(this)));

  // Register native blocking state callback (for control server updates).
  controller->SetNativeBlockingChangedCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnNativeBlockingChanged,
                          base::Unretained(this)));

  // Register keyboard event callback for hotkey detection.
  controller->SetKeyboardEventCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnKeyboardEvent,
                          base::Unretained(this)));

  // Register menu dismiss callback so injected mouse-down can close
  // any active context menu (content layer can't access views::MenuController).
  controller->SetMenuDismissCallback(base::BindRepeating([]() {
    auto* menu = views::MenuController::GetActiveInstance();
    if (menu) {
      menu->Cancel(views::MenuController::ExitType::kAll);
    }
  }));

  // Title bar icon.  The larger in-dialog logo is loaded by SetupContents().
  window_icon_ = LoadAppIcon(kWindowIconSize);

#ifdef MOUSEMUX_DEBUG
  LogDebug(base::StringPrintf("MouseMux Control Dialog initialized - BUILD #%d", kBuildNumber));
  FlushLogBuffer();  // Immediately write initialization message.
#endif
}

MouseMuxControlDialog::~MouseMuxControlDialog() {
#ifdef MOUSEMUX_DEBUG
  LogDebug("MouseMux Control Dialog destroyed");
#endif
#ifdef MOUSEMUX_NATIVE_BLOCK
  content::g_mousemux_dialog_hwnd = nullptr;
#endif
  instance_ = nullptr;
}

// static
namespace {

// Seat index derived from --mousemux-control-port, which is unique per
// instance.  Used to stagger dialogs: anchoring to the browser window is not
// enough on its own, because two seats whose windows happen to open in the
// same place would still get dialogs on top of each other.
int SeatIndexFromControlPort() {
  // Chrome's control ports, deliberately in a different range from the
  // MouseMux server's own port (41001) — they are unrelated systems and
  // adjacent numbers made them look like one shared range.
  constexpr unsigned kBasePort = 52000;
  constexpr unsigned kMaxSeats = 64;

  const std::string port_str =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          "mousemux-control-port");
  unsigned port = 0;
  if (port_str.empty() || !base::StringToUint(port_str, &port)) {
    return 0;  // No control port — single instance, no stagger needed.
  }
  if (port < kBasePort || (port - kBasePort) >= kMaxSeats) {
    return 0;  // Custom base port; cannot infer a seat, so don't guess.
  }
  return static_cast<int>(port - kBasePort);
}

// Places the dialog beside its own browser window.
//
// Each seat is a separate browser process, so a fixed position puts every
// dialog in the same spot and they stack once more than one seat runs.
// Anchoring to the window means the dialog travels with the seat it controls.
gfx::Rect ComputeDialogBounds() {
  gfx::Rect browser_bounds;
  for (Browser* browser : *BrowserList::GetInstance()) {
    if (browser->window()) {
      browser_bounds = browser->window()->GetBounds();
      break;
    }
  }

  // No browser window yet, or no screen to measure against — keep the
  // historical fixed position rather than guessing.
  display::Screen* screen = display::Screen::Get();
  if (browser_bounds.IsEmpty() || !screen) {
    return gfx::Rect(50, 50, kDialogWidth, kDialogHeight);
  }

  const gfx::Rect work_area =
      screen->GetDisplayMatching(browser_bounds).work_area();
  constexpr int kGap = 8;

  // Preferred spot is immediately right of the window.
  int x = browser_bounds.right() + kGap;
  if (x + kDialogWidth > work_area.right()) {
    x = browser_bounds.x() - kGap - kDialogWidth;  // Try the left instead.
  }
  if (x < work_area.x()) {
    // Neither side fits, which is the maximised case.  Tuck it inside the
    // window's top-right rather than letting it fall off screen.
    x = work_area.right() - kDialogWidth - kGap;
  }

  int y = browser_bounds.y();

  // Stagger by seat so dialogs don't land on top of each other: down the
  // screen first, then into a new column to the left.
  //
  // With up to 64 seats the grid cannot hold them all — 64 dialogs need more
  // vertical space than any monitor has — so the position cycles through the
  // cells that do fit.  Distant seats therefore share a position, which is
  // still far better than every seat piling into one clamped corner.
  const int row_step = kDialogHeight + kGap;
  const int col_step = kDialogWidth + kGap;
  int rows = work_area.height() / row_step;
  int cols = work_area.width() / col_step;
  if (rows < 1) {
    rows = 1;
  }
  if (cols < 1) {
    cols = 1;
  }
  const int cell = SeatIndexFromControlPort() % (rows * cols);
  y += (cell % rows) * row_step;
  x -= (cell / rows) * col_step;

  // Clamp last, so the stagger can never push the dialog off screen.
  if (x + kDialogWidth > work_area.right()) {
    x = work_area.right() - kDialogWidth;
  }
  if (x < work_area.x()) {
    x = work_area.x();
  }
  if (y + kDialogHeight > work_area.bottom()) {
    y = work_area.bottom() - kDialogHeight;
  }
  if (y < work_area.y()) {
    y = work_area.y();
  }

  return gfx::Rect(x, y, kDialogWidth, kDialogHeight);
}

}  // namespace

void MouseMuxControlDialog::Show() {
  // A second Show() would leak the first dialog and overwrite instance_ and
  // g_mousemux_dialog_hwnd — leaving native input blocking to exempt a window
  // that is no longer the live dialog, which would lock the user out of the
  // controls.  Surface the existing one instead.
  if (instance_) {
    if (views::Widget* existing = instance_->GetWidget()) {
      existing->Show();
      existing->Activate();
    }
    return;
  }

  auto* dialog = new MouseMuxControlDialog();
  auto* widget = views::DialogDelegate::CreateDialogWidget(
      dialog, gfx::NativeWindow(), gfx::NativeView());

  // Anchored to this seat's browser window so multiple instances don't stack.
  widget->SetBounds(ComputeDialogBounds());

  // Show and activate.
  widget->Show();
  widget->Activate();

#ifdef MOUSEMUX_NATIVE_BLOCK
  // Register our HWND so PreHandleMSG exempts the dialog from native
  // mouse blocking — otherwise we lock the user out of the controls.
  content::g_mousemux_dialog_hwnd =
      widget->GetNativeWindow()->GetHost()->GetAcceleratedWidget();
#endif
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

  // The entire collapsed UI: icon plus one button.  Added first so it sits at
  // the top; hidden until collapse.  The title bar is dropped while collapsed,
  // so the icon lives here rather than in the frame.
  expand_row_ = AddChildView(std::make_unique<views::View>());
  auto* expand_layout =
      expand_row_->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  expand_layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kStart);
  expand_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  gfx::ImageSkia strip_icon = LoadAppIcon(kWindowIconSize);
  if (!strip_icon.isNull()) {
    expand_row_->AddChildView(std::make_unique<views::ImageView>(
        ui::ImageModel::FromImageSkia(strip_icon)));
  }

  auto* expand_button = expand_row_->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnCollapseClicked,
                              base::Unretained(this)),
          u"Expand"));
  expand_button->SetMinSize(gfx::Size(0, 28));
  expand_row_->SetVisible(false);

#ifdef MOUSEMUX_DEBUG
  // Title label.
  auto* title_label = AddChildView(std::make_unique<views::Label>(
      u"MouseMux Integration Controls", views::style::CONTEXT_DIALOG_TITLE,
      views::style::STYLE_PRIMARY));
  title_label->SetFontList(
      gfx::FontList().Derive(4, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
  title_label->SetHorizontalAlignment(gfx::ALIGN_CENTER);
#endif

  // MouseMux toggle row.  First, because connecting is the prerequisite for
  // everything below it — nothing else in this dialog does anything until the
  // connection is up.
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
  capture_button_->SetMinSize(gfx::Size(0, 32));

  // Shrinks the dialog to a strip rather than hiding it, so there is always
  // something on screen to click to get back.
  auto* collapse_button = capture_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnCollapseClicked,
                              base::Unretained(this)),
          u"Collapse"));
  collapse_button->SetMinSize(gfx::Size(0, 32));
  collapse_button->SetTooltipText(
      u"Shrink this dialog out of the way. Click it again to restore.");

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
  release_owner_button_->SetMinSize(gfx::Size(0, 32));

#ifndef MOUSEMUX_DEBUG
  // Build date label in the button row at the lower left, light gray.
  auto build_label = std::make_unique<views::Label>(
      base::ASCIIToUTF16(
          base::StringPrintf("Build #%d - %s", kBuildNumber, kBuildDate)),
      views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_DISABLED);
  build_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  // Kept so collapse can hide it — the extra view sits in the frame's button
  // row, which SetButtons(kNone) does not remove on its own.
  build_label_ = SetExtraView(std::move(build_label));
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
#else
  // Spacer absorbs extra vertical space so rows don't get squeezed.
  auto* vertical_spacer = AddChildView(std::make_unique<views::View>());
  layout->SetFlexForView(vertical_spacer, 1);
#endif
}

gfx::Size MouseMuxControlDialog::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  // Collapsed is forced: the strip must actually be small, and without this
  // the frame sizes to content and SetBounds gets overridden.
  if (collapsed_) {
    return gfx::Size(kCollapsedWidth, kCollapsedHeight);
  }

  // These size the CONTENT, not the window.  The frame adds its own title bar,
  // border and button row on top, which is why the window measures roughly
  // 590x254 against a 420x240 content size — the constants are not being
  // ignored, they simply do not include frame decoration.
  return gfx::Size(kDialogWidth, kDialogHeight);
}

bool MouseMuxControlDialog::ShouldShowWindowTitle() const {
  // Collapsed drops the whole title bar; the icon moves into the strip itself
  // so the frame has nothing left to lay out.
  return !collapsed_;
}

bool MouseMuxControlDialog::ShouldShowWindowIcon() const {
  return !collapsed_ && !window_icon_.isNull();
}

bool MouseMuxControlDialog::ShouldShowCloseButton() const {
  return !collapsed_;
}

ui::ImageModel MouseMuxControlDialog::GetWindowIcon() {
  if (window_icon_.isNull()) {
    return ui::ImageModel();
  }
  return ui::ImageModel::FromImageSkia(window_icon_);
}


void MouseMuxControlDialog::OnNativeInputToggled() {
  bool is_on = native_input_toggle_->GetIsOn();
  native_input_status_label_->SetText(is_on ? u"Blocking" : u"Off");

  LogDebug(std::string("Native input blocking: ") + (is_on ? "ENABLED" : "DISABLED"));

  // Apply immediately to controller.
  auto* controller = content::MouseMuxInputController::GetInstance();
  controller->SetNativeInputBlocked(is_on);
}

void MouseMuxControlDialog::OnNativeBlockingChanged(bool blocked) {
  if (native_input_toggle_ && native_input_toggle_->GetIsOn() != blocked) {
    native_input_toggle_->SetIsOn(blocked);
    if (native_input_status_label_) {
      native_input_status_label_->SetText(blocked ? u"Blocking" : u"Off");
    }
  }
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
  // Sync toggle to match actual state (e.g. control server changed it).
  if (connected && mousemux_toggle_ && !mousemux_toggle_->GetIsOn()) {
    mousemux_toggle_->SetIsOn(true);
  }
  if (!connected) {
    // Reset the toggle back to off so the user can retry.
    if (mousemux_toggle_ && mousemux_toggle_->GetIsOn()) {
      mousemux_toggle_->SetIsOn(false);
    }
    // Clear all stale UI state — controller already reset its side.
    owner_hwid_ = -1;
    owner_name_.clear();
    is_captured_ = false;
    UpdateCaptureButton();
    UpdateTitle();
    if (release_owner_button_) {
      release_owner_button_->SetEnabled(false);
    }
    // Controller unblocked native input on disconnect — sync the toggle.
    if (native_input_toggle_ && native_input_toggle_->GetIsOn()) {
      native_input_toggle_->SetIsOn(false);
    }
    if (native_input_status_label_) {
      native_input_status_label_->SetText(u"Off");
    }
  }
}

void MouseMuxControlDialog::OnCaptureStateChanged(bool captured) {
  is_captured_ = captured;
  UpdateCaptureButton();
  UpdateTitle();
  LogDebug(std::string("Capture state changed: ") + (captured ? "CAPTURED" : "RELEASED"));
}

void MouseMuxControlDialog::OnCollapseClicked() {
  SetCollapsed(!collapsed_);
}

void MouseMuxControlDialog::SetCollapsed(bool collapsed) {
  if (collapsed == collapsed_) {
    return;
  }
  collapsed_ = collapsed;

  // Every row hides except the expand strip, which is the sole thing left to
  // click — that is what keeps collapse recoverable without automation.
  for (views::View* child : children()) {
    child->SetVisible(child == expand_row_ ? collapsed : !collapsed);
  }

  // The Close button and the extra (build info) view belong to the dialog
  // frame rather than our children, so they need dropping separately or the
  // collapsed strip is not actually small.
  SetButtons(collapsed
                 ? static_cast<int>(ui::mojom::DialogButton::kNone)
                 : static_cast<int>(ui::mojom::DialogButton::kCancel));
  if (build_label_) {
    build_label_->SetVisible(!collapsed);
  }
  DialogModelChanged();

  // Title, icon and close button are all driven by the ShouldShow* overrides
  // reading collapsed_; the frame has to be told to re-read them.
  if (views::Widget* widget = GetWidget()) {
    widget->UpdateWindowTitle();
    widget->UpdateWindowIcon();
    if (views::NonClientView* frame = widget->non_client_view()) {
      frame->InvalidateLayout();
    }

    // Resize in place: keeping the origin means the strip stays where the
    // user last put the dialog, and keeps the per-seat stagger meaningful.
    const gfx::Rect bounds = widget->GetWindowBoundsInScreen();
    widget->SetBounds(gfx::Rect(
        bounds.origin(),
        collapsed ? gfx::Size(kCollapsedWidth, kCollapsedHeight)
                  : gfx::Size(kDialogWidth, kDialogHeight)));
  }

  LogDebug(collapsed ? "Dialog collapsed" : "Dialog expanded");
}

void MouseMuxControlDialog::OnVisibilityChanged(bool visible) {
  views::Widget* widget = GetWidget();
  if (!widget) {
    return;
  }
  if (visible) {
    widget->Show();
    widget->Activate();
  } else {
    widget->Hide();
  }
  LogDebug(visible ? "Dialog shown" : "Dialog hidden");
}

void MouseMuxControlDialog::OnCaptureClicked() {
  auto* controller = content::MouseMuxInputController::GetInstance();
  if (is_captured_) {
    LogDebug("Release Capture button clicked");
    controller->ReleaseCapture();
  } else {
    LogDebug("Capture Mouse button clicked");
    controller->CaptureOwner();

    // After capturing, give focus back to the browser window so keyboard
    // events reach the web content.  Clicking this button gave the dialog
    // OS focus, which blocks keyboard injection from reaching the renderer.
    controller->FocusKeyboardTargetView();
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
  if (owner_hwid_ == -1) {
    title = base::StringPrintf("%s (No Owner)", kProductName);
  } else if (owner_name_.empty()) {
    title = base::StringPrintf("%s - Owner: 0x%X%s", kProductName, owner_hwid_,
                               capture_suffix);
  } else {
    title = base::StringPrintf("%s - Owner: %s (0x%X)%s", kProductName,
                               owner_name_.c_str(), owner_hwid_,
                               capture_suffix);
  }
  SetTitle(base::ASCIIToUTF16(title));

  // Force widget to update title.
  if (GetWidget()) {
    GetWidget()->UpdateWindowTitle();
  }
}

BEGIN_METADATA(MouseMuxControlDialog)
END_METADATA

}  // namespace mouse_mux
