// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/mouse_mux/mouse_mux_control_dialog.h"

#ifdef MOUSEMUX_DEBUG
#include <array>
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
// Chromium 151 removed chrome/browser/ui/browser_list.h.  Browser enumeration
// now goes through BrowserWindowInterface; Browser and BrowserWindow are no
// longer needed here at all, since the only use was reading window bounds.
#include "base/files/file_path.h"
#include "base/process/launch.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/base_window.h"
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

// std::array rather than a C array: Chromium 151 applies
// -Wunsafe-buffer-usage to chrome/browser/ui, and subscripting a raw array
// trips it even where the index is provably in range.  std::array indexes
// through a member function, which the warning does not flag.
constexpr auto kHotkeyOptions = std::to_array<HotkeyOption>({
    {u"Shift+Escape", VK_ESCAPE, true, false, false},
    {u"Ctrl+Shift+Escape", VK_ESCAPE, true, true, false},
    {u"Alt+Shift+Escape", VK_ESCAPE, true, false, true},
    {u"Shift+F12", VK_F12, true, false, false},
    {u"Alt+Shift+F12", VK_F12, true, false, true},
});

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

// Shown in the dialog footer.  MUST track the release version — 2.2.58 is
// build #58 — and MUST be bumped with it.  It sat at 54 through the 2.2.55 and
// 2.2.56 releases, so the dialog reported a build three versions older than the
// binary it was part of, which makes it impossible to tell by looking whether
// someone is running what you think they are.
constexpr int kBuildNumber = 58;

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
constexpr int kWindowIconSize = 24;

#ifdef MOUSEMUX_DEBUG
constexpr int kDialogWidth = 600;
constexpr int kDialogHeight = 500;
constexpr int kLogFlushThreshold = 5;
const char kLogFilePath[] = MOUSEMUX_DEBUG_LOG_PATH;
#else
constexpr int kDialogWidth = 560;
// One line taller than 2.2.57 for the keyboard note under the owner list.
// The note hides itself when there is nothing to report, so the extra room is
// only needed some of the time, but a dialog that changes height as users
// type reads as broken.
constexpr int kDialogHeight = 424;

// Collapsed strip: no title bar, no close button, no build footer — just the
// icon and one button, so it can be much smaller than the dialog.
constexpr int kCollapsedWidth = 150;
constexpr int kCollapsedHeight = 52;
// Date only.  A "TRACE" suffix was hardcoded here on 2026-08-04 and left in,
// so ordinary builds claimed to be trace builds that log input to disk.  The
// trace warning belongs in kProductName above, where it is #ifdef-guarded and
// cannot lie.  Do not put build flavour in this string.
const char kBuildDate[] = "2026-09-02";
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
  RebuildOwnerList();

  // Owners' window titles change as they browse and nothing reports that, so
  // poll.  One second is slow enough to be free and fast enough that the list
  // is never visibly wrong.
  owner_refresh_timer_.Start(
      FROM_HERE, base::Seconds(1),
      base::BindRepeating(&MouseMuxControlDialog::RebuildOwnerList,
                          base::Unretained(this)));

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
  //
  // MenuController::GetActiveInstance() is a PROCESS-WIDE singleton: there is
  // one active menu for the whole browser, not one per window.  Cancelling it
  // on every mouse-down from every device means user 2 clicking anywhere closes
  // the dropdown user 1 just opened.  So attribute the menu to the device that
  // opened it, and only let that device close it.
  //
  // Attribution is by inference — nothing tells us who opened a menu — but the
  // inference is sound: a menu can only have been opened by the last click
  // that happened while no menu was open.
  //
  // The deeper limit remains and cannot be fixed here: one active menu per
  // process means two users cannot have two dropdowns open at once.
  controller->SetMenuDismissCallback(base::BindRepeating(
      [](int* menu_owner_hwid, int* pending_menu_hwid, int hwid) {
        auto* menu = views::MenuController::GetActiveInstance();
        if (!menu) {
          // Nothing open.  If THIS click opens a menu, it belongs to this
          // device.
          *menu_owner_hwid = -1;
          *pending_menu_hwid = hwid;
          return;
        }
        if (*menu_owner_hwid == -1) {
          // First click seen since this menu appeared: attribute it to
          // whoever clicked last while nothing was open.
          *menu_owner_hwid = *pending_menu_hwid;
        }
        if (*menu_owner_hwid == hwid) {
          menu->Cancel(views::MenuController::ExitType::kAll);
          *menu_owner_hwid = -1;
        }
        // Otherwise it is another user's menu — leave it alone.
      },
      &menu_owner_hwid_, &pending_menu_hwid_));

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
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (const ui::BaseWindow* window = browser->GetWindow()) {
      browser_bounds = window->GetBounds();
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

  // Hard lock row.  Two states only — off is "soft", which is not a mode but
  // simply what routing by hit-test does — so a toggle rather than a picker.
  auto* lock_row = AddChildView(std::make_unique<views::View>());
  auto* lock_layout =
      lock_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  lock_layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kStart);
  lock_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  hard_lock_toggle_ =
      lock_row->AddChildView(std::make_unique<views::ToggleButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnHardLockToggled,
                              base::Unretained(this))));
  hard_lock_toggle_->SetAccessibleName(u"Lock Users To Their Window");

  auto* lock_label = lock_row->AddChildView(
      std::make_unique<views::Label>(u"Lock Users To Their Window"));
  lock_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  lock_label->SetTooltipText(
      u"Off: a user who clicks another window moves there.\n"
      u"On: clicks outside a user's own window are ignored, so each user is "
      u"confined to one window. Cursors still move freely; only clicks are "
      u"blocked. A user with no window claims the first one they click, and "
      u"closing a window frees its user to claim another.");
  lock_layout->SetFlexForView(lock_label, 1);

  hard_lock_status_label_ =
      lock_row->AddChildView(std::make_unique<views::Label>(u"Off"));

#ifdef MOUSEMUX_DEBUG
  // Info label with server address.
  auto* info_label = AddChildView(std::make_unique<views::Label>(
      u"Toggle settings take effect immediately. Server: ws://localhost:41001",
      views::style::CONTEXT_DIALOG_BODY_TEXT, views::style::STYLE_SECONDARY));
  info_label->SetHorizontalAlignment(gfx::ALIGN_CENTER);
#endif

  // ---------------------------------------------------------------------
  // Owner list — the control centre proper.  One row per user, showing which
  // window they are working in, whether they are captured, and letting the
  // operator act on that one user without disturbing the others.
  //
  // The bulk actions live in this header rather than in a row of their own.
  // They act on the list directly below them, and floating them among the
  // connection toggles made it look as though there were two unrelated ways to
  // capture.  They are the same actions as the per-row buttons, applied to
  // everyone.
  //
  // "Capture all" and "Drop all" are deliberately NOT one button: capture
  // stops a device producing native input, while dropping removes ownership
  // altogether.  Naming them alike would invite exactly that confusion.
  // ---------------------------------------------------------------------
  auto* owners_header_row = AddChildView(std::make_unique<views::View>());
  auto* owners_header_layout =
      owners_header_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  owners_header_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  auto* owners_header = owners_header_row->AddChildView(
      std::make_unique<views::Label>(u"Owners",
                                     views::style::CONTEXT_DIALOG_BODY_TEXT,
                                     views::style::STYLE_PRIMARY));
  owners_header->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  owners_header_layout->SetFlexForView(owners_header, 1);

  capture_button_ = owners_header_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnCaptureClicked,
                              base::Unretained(this)),
          u"Capture all"));
  capture_button_->SetEnabled(false);  // Disabled until we have an owner.
  capture_button_->SetMinSize(gfx::Size(96, 30));
  capture_button_->SetTooltipText(
      u"Stop every owner's device producing native Windows input. Required "
      u"for several users to work at once. Capture one at a time from its "
      u"row below.");

  release_owner_button_ = owners_header_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnReleaseOwnerClicked,
                              base::Unretained(this)),
          u"Drop all"));
  release_owner_button_->SetEnabled(false);  // Disabled until we have an owner.
  release_owner_button_->SetMinSize(gfx::Size(76, 30));
  release_owner_button_->SetTooltipText(
      u"Remove every owner. Their devices stop driving Chrome until they "
      u"click to claim again. To drop just one, use its row below.");

  owner_list_ = AddChildView(std::make_unique<views::View>());
  owner_list_->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 2));

  // Where typing actually went, or why it went nowhere.  The rows above say
  // where each user is WORKING; this says where their KEYS landed, and the
  // two differ exactly when something is misconfigured -- which is the case
  // nobody can otherwise diagnose from the outside.
  keyboard_note_label_ = AddChildView(std::make_unique<views::Label>(
      std::u16string(), views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_SECONDARY));
  keyboard_note_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  keyboard_note_label_->SetElideBehavior(gfx::ELIDE_TAIL);
  keyboard_note_label_->SetVisible(false);

  // Hand out a window.  Two buttons because they are genuinely different
  // things: a window of THIS Chrome shares cookies and logins with the other
  // users, a seat is its own process and profile and shares nothing.
  auto* handout_row = AddChildView(std::make_unique<views::View>());
  auto* handout_layout =
      handout_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  handout_layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kStart);

  // Short labels, and a min WIDTH as well as height.  The long forms
  // ("+ Window (this profile)") squeezed to ellipsis as soon as the dialog was
  // narrow, and a button whose label is elided tells the operator nothing.
  // The distinction that matters lives in the tooltips.
  auto* new_window_button = handout_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnNewWindowClicked,
                              base::Unretained(this)),
          u"+ Window"));
  new_window_button->SetMinSize(gfx::Size(110, 32));
  new_window_button->SetTooltipText(
      u"Copy the current tab into another window of THIS Chrome -- already "
      u"signed in, sharing the same session as the other users. Have the "
      u"next user click in it to claim it.");

  auto* new_seat_button = handout_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnNewSeatClicked,
                              base::Unretained(this)),
          u"+ Seat"));
  new_seat_button->SetMinSize(gfx::Size(110, 32));
  new_seat_button->SetTooltipText(
      u"Launch a separate Chrome with its OWN profile and its own dialog. "
      u"Isolated: shares no logins with this one.");

  // Absorbs slack so the two buttons keep their size and sit left rather than
  // stretching across the dialog.
  auto* handout_spacer =
      handout_row->AddChildView(std::make_unique<views::View>());
  handout_layout->SetFlexForView(handout_spacer, 1);

  // Release hotkey.  A setting, configured once and rarely touched, so it
  // belongs at the end rather than in prime position between two buttons —
  // but it is also the escape hatch out of capture, so it stays visible
  // rather than hiding behind a menu.
  handout_row->AddChildView(std::make_unique<views::Label>(
      u"Release hotkey:", views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_SECONDARY));

  hotkey_model_ = std::make_unique<HotkeyComboboxModel>();
  hotkey_dropdown_ =
      handout_row->AddChildView(std::make_unique<views::Combobox>(
          hotkey_model_.get()));
  hotkey_dropdown_->SetCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnHotkeyChanged,
                          base::Unretained(this)));
  hotkey_dropdown_->SetSelectedIndex(0);  // Default: Shift+Escape
  hotkey_dropdown_->SetTooltipText(
      u"Key combination that releases capture, for when injected input is not "
      u"working and the mice cannot reach this dialog.");

#ifndef MOUSEMUX_DEBUG
  // Footer, in the frame's button row beside Close: build info and Collapse.
  // Collapse is window management, so it belongs next to the other window
  // control rather than among the operating controls above.
  auto footer = std::make_unique<views::View>();
  auto* footer_layout =
      footer->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  footer_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  auto* build_text = footer->AddChildView(std::make_unique<views::Label>(
      base::ASCIIToUTF16(
          base::StringPrintf("Build #%d - %s", kBuildNumber, kBuildDate)),
      views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_DISABLED));
  build_text->SetHorizontalAlignment(gfx::ALIGN_LEFT);

  // Shrinks the dialog to a strip rather than hiding it, so there is always
  // something on screen to click to get back.
  auto* collapse_button =
      footer->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnCollapseClicked,
                              base::Unretained(this)),
          u"Collapse"));
  collapse_button->SetMinSize(gfx::Size(84, 30));
  collapse_button->SetTooltipText(
      u"Shrink this dialog out of the way. Click it again to restore.");

  // Kept so collapse can hide it — the extra view sits in the frame's button
  // row, which SetButtons(kNone) does not remove on its own.
  build_label_ = SetExtraView(std::move(footer));
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


void MouseMuxControlDialog::OnHardLockToggled() {
  const bool on = hard_lock_toggle_ && hard_lock_toggle_->GetIsOn();
  content::MouseMuxInputController::GetInstance()->SetHardLock(on);
  if (hard_lock_status_label_) {
    hard_lock_status_label_->SetText(on ? u"On" : u"Off");
  }
  LogDebug(std::string("Hard lock: ") + (on ? "ON" : "OFF"));
  RebuildOwnerList();
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
  RebuildOwnerList();
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
    // The combobox model only offers kHotkeyOptions.size() entries, so this
    // should already hold - but this was the one of the three index sites
    // with no bounds check at all, and it feeds an unchecked lookup below.
    if (selected_hotkey_index_ >= std::size(kHotkeyOptions)) {
      selected_hotkey_index_ = 0;
    }
    LogDebug(base::StringPrintf("Hotkey changed to index %zu: %s",
                                 selected_hotkey_index_,
                                 base::UTF16ToASCII(kHotkeyOptions[selected_hotkey_index_].label).c_str()));
  }
}

void MouseMuxControlDialog::RebuildOwnerList() {
  if (!owner_list_) {
    return;
  }
  owner_list_->RemoveAllChildViews();

  auto owners =
      content::MouseMuxInputController::GetInstance()->GetOwners();

  if (owners.empty()) {
    auto* empty = owner_list_->AddChildView(std::make_unique<views::Label>(
        u"No owners yet — have a user click in a window to claim it.",
        views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_DISABLED));
    empty->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    // Nobody owns anything, so any routing note left over from the last
    // session of use describes people who are no longer here.
    if (keyboard_note_label_) {
      keyboard_note_label_->SetVisible(false);
    }
    owner_list_->InvalidateLayout();
    return;
  }

  for (const auto& owner : owners) {
    auto* row = owner_list_->AddChildView(std::make_unique<views::View>());
    auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 6));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    // Name, falling back to the hwid when the SDK user list has not named
    // this device — an unnamed owner is still an owner and must be operable.
    std::u16string label_text =
        owner.name.empty()
            ? base::ASCIIToUTF16(base::StringPrintf("device 0x%x", owner.hwid))
            : base::UTF8ToUTF16(owner.name);
    if (owner.is_primary) {
      label_text += u" *";
    }
    auto* name_label = row->AddChildView(std::make_unique<views::Label>(
        label_text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    name_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    if (owner.is_primary) {
      name_label->SetTooltipText(
          u"Primary owner — the one reported to the control server and the "
          u"single-owner API.");
    }

    // Which window this user is working in.
    std::u16string where =
        owner.has_window
            ? (owner.window_title.empty() ? u"(untitled window)"
                                          : owner.window_title)
            : u"— not in a window yet";
    auto* where_label = row->AddChildView(std::make_unique<views::Label>(
        where, views::style::CONTEXT_DIALOG_BODY_TEXT,
        owner.has_window ? views::style::STYLE_SECONDARY
                         : views::style::STYLE_DISABLED));
    where_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    where_label->SetElideBehavior(gfx::ELIDE_TAIL);
    layout->SetFlexForView(where_label, 1);

    // The keyboard MouseMux has attached to this user.  A user with a mouse
    // and no keyboard still owns a window and looks perfectly healthy in
    // every other column, but their typing cannot be told from anyone
    // else's -- so it is called out here rather than left to be deduced.
    std::u16string kb_text;
    int kb_style = views::style::STYLE_SECONDARY;
    if (owner.keyboard_hwid == 0) {
      kb_text = u"no keyboard";
      kb_style = views::style::STYLE_PRIMARY;
    } else if (owner.keyboard_typed) {
      kb_text = base::ASCIIToUTF16(
                    base::StringPrintf("kb 0x%x", owner.keyboard_hwid)) +
                u" \u2713";
    } else {
      kb_text = base::ASCIIToUTF16(
          base::StringPrintf("kb 0x%x", owner.keyboard_hwid));
    }
    auto* kb_label = row->AddChildView(std::make_unique<views::Label>(
        kb_text, views::style::CONTEXT_DIALOG_BODY_TEXT, kb_style));
    kb_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    kb_label->SetTooltipText(
        owner.keyboard_hwid == 0
            ? u"MouseMux has no keyboard on this user. Give this user a "
              u"mouse AND a keyboard in MouseMux -- until then their "
              u"keystrokes cannot be told from anyone else's, and with "
              u"several users they are ignored rather than typed into "
              u"somebody else's window."
            : (owner.keyboard_typed
                   ? u"This user's keyboard, and it has typed."
                   : u"This user's keyboard. Nothing has arrived from it "
                     u"yet."));

    auto* capture_btn = row->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&MouseMuxControlDialog::OnOwnerCaptureClicked,
                            base::Unretained(this), owner.hwid),
        owner.captured ? u"Release" : u"Capture"));
    capture_btn->SetMinSize(gfx::Size(76, 28));
    capture_btn->SetTooltipText(
        owner.captured
            ? u"Give this user's mouse back to Windows."
            : u"Stop this user's device producing native Windows input. "
              u"Required for several users to work at once.");

    auto* close_btn = row->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&MouseMuxControlDialog::OnOwnerCloseWindowClicked,
                            base::Unretained(this), owner.window),
        u"Close win"));
    close_btn->SetMinSize(gfx::Size(76, 28));
    close_btn->SetEnabled(owner.has_window);
    close_btn->SetTooltipText(u"Close the window this user is working in.");

    auto* release_btn = row->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&MouseMuxControlDialog::OnOwnerReleaseClicked,
                            base::Unretained(this), owner.hwid),
        u"Drop"));
    release_btn->SetMinSize(gfx::Size(58, 28));
    release_btn->SetTooltipText(
        u"Remove this owner. Their device stops driving Chrome until they "
        u"click to claim again.");
  }
  UpdateKeyboardNote();
  owner_list_->InvalidateLayout();
}

void MouseMuxControlDialog::UpdateKeyboardNote() {
  if (!keyboard_note_label_) {
    return;
  }
  const auto owners =
      content::MouseMuxInputController::GetInstance()->GetOwners();

  // A missing pairing outranks anything else this line could say: it is the
  // cause, and the routing underneath it would only be the symptom.
  bool missing_keyboard = false;
  for (const auto& owner : owners) {
    if (owner.keyboard_hwid == 0) {
      missing_keyboard = true;
      break;
    }
  }
  if (missing_keyboard) {
    keyboard_note_label_->SetText(
        u"\u26a0 A user above has no keyboard. In MouseMux, give every user "
        u"a mouse AND a keyboard, or their typing cannot be routed.");
    keyboard_note_label_->SetVisible(true);
    return;
  }

  auto routes =
      content::MouseMuxInputController::GetInstance()->GetKeyRoutes();
  if (routes.empty()) {
    keyboard_note_label_->SetVisible(false);
    return;
  }

  // Name the destination by the user it belongs to, not by hwid: this line is
  // read next to a list of names.
  std::u16string text = u"Typing: ";
  bool first = true;
  for (const auto& route : routes) {
    if (!first) {
      text += u"  \u00b7  ";
    }
    first = false;

    std::u16string who;
    for (const auto& owner : owners) {
      if (owner.keyboard_hwid == route.keyboard_hwid) {
        who = owner.name.empty()
                  ? base::ASCIIToUTF16(
                        base::StringPrintf("0x%x", route.keyboard_hwid))
                  : base::UTF8ToUTF16(owner.name);
        break;
      }
    }
    if (who.empty()) {
      who = base::ASCIIToUTF16(
          base::StringPrintf("kb 0x%x", route.keyboard_hwid));
    }

    text += who;
    if (route.dropped) {
      text += u" \u2192 ignored (keyboard not assigned to a user)";
    } else {
      text += u" \u2192 ";
      text += route.window_title.empty() ? std::u16string(u"(untitled window)")
                                         : route.window_title;
    }
    if (route.count > 1) {
      text += base::ASCIIToUTF16(base::StringPrintf(" x%d", route.count));
    }
  }
  keyboard_note_label_->SetText(text);
  keyboard_note_label_->SetVisible(true);
}

void MouseMuxControlDialog::OnOwnerCaptureClicked(int hwid) {
  auto* controller = content::MouseMuxInputController::GetInstance();
  // Read the current state rather than trusting the button's caption: the
  // row may have been drawn before the control server changed things.
  bool captured = false;
  for (const auto& owner : controller->GetOwners()) {
    if (owner.hwid == hwid) {
      captured = owner.captured;
      break;
    }
  }
  if (captured) {
    controller->ReleaseCaptureHwid(hwid);
  } else {
    controller->CaptureOwnerHwid(hwid);
    // Capture steals OS focus to this dialog's window; put it back so the
    // user can type straight away.
    controller->FocusKeyboardTargetView();
  }
  RebuildOwnerList();
}

void MouseMuxControlDialog::OnOwnerReleaseClicked(int hwid) {
  // ReleaseOwnerHwid releases capture for this device before dropping it.
  content::MouseMuxInputController::GetInstance()->ReleaseOwnerHwid(hwid);
  RebuildOwnerList();
}

void MouseMuxControlDialog::OnOwnerCloseWindowClicked(
    gfx::AcceleratedWidget window) {
  if (!window || !::IsWindow(window)) {
    return;
  }
  // WM_CLOSE, not DestroyWindow: this asks Chrome to close the browser window
  // through its own path, so beforeunload handlers and session state behave
  // as they would if the user had clicked the X.
  ::PostMessage(window, WM_CLOSE, 0, 0);
}

void MouseMuxControlDialog::OnNewWindowClicked() {
  // Hand out a window that is ALREADY SIGNED IN, by duplicating the current
  // tab and moving the copy out -- not by opening a blank one.
  //
  // A blank window shares less than it appears to.  Cookies come from the
  // profile, so most sites carry over, but session storage belongs to the
  // TAB, and a web app that keeps its token there sees a new window as a new
  // sign-in.  On a site that permits one session at a time -- which is the
  // case this product exists for -- that new sign-in ends everybody else's.
  //
  // Duplicating clones the tab's session storage, exactly as Chrome's own
  // "Duplicate tab" does, and moving the duplicate to a new window carries
  // the live page across without a reload.  This is the manual dance
  // (duplicate, then drag the tab out) in one click, and it is the only way
  // to hand somebody a window that is genuinely already authenticated.
  //
  // The source is the FIRST browser window, deliberately, rather than the
  // most recently active one: with everyone captured nothing is activating
  // windows at all, so activation order is stale, while the first window is
  // the one the operator signed in to before handing any out.
  BrowserWindowInterface* source = nullptr;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
        browser->GetTabStripModel() &&
        browser->GetTabStripModel()->active_index() != TabStripModel::kNoTab) {
      source = browser;
      break;
    }
  }
  if (!source) {
    // Nothing to copy from -- first run, or every window already closed.
    for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
      if (Profile* profile = browser->GetProfile()) {
        chrome::NewEmptyWindow(profile);
        return;
      }
    }
    LogDebug("New window: no existing browser to take a profile from");
    return;
  }

  TabStripModel* model = source->GetTabStripModel();
  const int index = model->active_index();
  if (!chrome::CanDuplicateTabAt(source, index)) {
    // Some tabs cannot be duplicated -- a crashed one, for instance.  A blank
    // window is not what was asked for, but it is still a window.
    chrome::NewEmptyWindow(source->GetProfile());
    return;
  }

  content::WebContents* duplicate = chrome::DuplicateTabAt(source, index);
  if (!duplicate) {
    chrome::NewEmptyWindow(source->GetProfile());
    return;
  }

  // Find the copy by identity rather than by assuming where it landed:
  // pinning and tab groups both move it.
  const int duplicate_index = model->GetIndexOfWebContents(duplicate);
  if (duplicate_index == TabStripModel::kNoTab ||
      !chrome::CanMoveTabsToNewWindow(source, {duplicate_index})) {
    // The duplicate exists and is signed in where it is; leaving it as a tab
    // loses nothing, and the operator can still drag it out by hand.
    LogDebug("New window: duplicated tab could not be moved out");
    return;
  }
  chrome::MoveTabsToNewWindow(source, {duplicate_index});
}

void MouseMuxControlDialog::OnNewSeatClicked() {
  // Chrome starts Chrome. launcher.exe existed only to hold a seat mutex for a
  // browser it could not modify; the browser now holds its own.
  if (!content::MouseMuxInputController::GetInstance()
           ->LaunchAdditionalSeat()) {
    LogDebug("New seat: failed to start (all seats taken, or launch failed)");
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
  RebuildOwnerList();

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
  // The title no longer carries owner or capture state.  Both are per-user
  // now, and a single title can only ever describe one of them — it would show
  // the primary owner and quietly misrepresent everyone else.  The owner list
  // says it properly, one row per user.
  SetTitle(base::ASCIIToUTF16(std::string(kProductName)));

  // Force widget to update title.
  if (GetWidget()) {
    GetWidget()->UpdateWindowTitle();
  }
}

BEGIN_METADATA(MouseMuxControlDialog)
END_METADATA

}  // namespace mouse_mux
