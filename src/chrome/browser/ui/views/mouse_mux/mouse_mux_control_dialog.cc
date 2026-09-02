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
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
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
#include "ui/color/color_id.h"
#include "ui/compositor/compositor.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/toggle_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/window/non_client_view.h"
#ifdef MOUSEMUX_DEBUG
#include "ui/views/controls/textarea/textarea.h"
#endif
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/views/controls/menu/menu_controller.h"
#include "ui/views/layout/table_layout.h"
#include "ui/views/view_utils.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/base/models/combobox_model.h"

#ifdef MOUSEMUX_NATIVE_BLOCK
namespace content {
extern HWND g_mousemux_dialog_hwnd;
extern HWND g_mousemux_help_hwnd;
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
// Wider than it was: the screen-and-page column is the one an operator
// actually reads, and at 560 the fixed columns and three row controls left it
// about 120px, so it elided to "(un...".
constexpr int kDialogWidth = 660;
// Measured, not guessed: the contents view comes out about 84px shorter than
// this, because the frame's button row is taken out of it.  At 424 everything
// was already at its minimum height with nothing left over, which is how the
// user list ended up 42px tall holding two rows.
constexpr int kDialogHeight = 520;

// How much of the list is shown before it scrolls.  Four users fit; a fifth
// scrolls rather than squeezing everybody.
constexpr int kListMinHeight = 40;
constexpr int kListMaxHeight = 240;

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

// One button height in the whole dialog.  Five different heights were most of
// what made this look like several dialogs stacked on top of each other, and
// with the big action button gone there is nothing left that earns its own.
constexpr int kRowButtonHeight = 28;

// Panes.  The dialog covers three unrelated things - the connection, the
// people, and settings that apply to everybody - and running them together in
// one column was most of why it read as a list of switches with no shape.  A
// hairline box around each says where one subject ends and the next begins
// without adding chrome that competes with the content.
constexpr int kPaneRadius = 6;

// The help window.  Wide enough for a readable line, tall enough to show a
// section and a bit of the next, which is what tells a reader to scroll.
constexpr int kHelpWidth = 520;
constexpr int kHelpHeight = 460;
constexpr int kPanePadding = 10;

// Row status dot.  Green: captured and working.  Amber: an owner, but not
// captured, so the rest of the machinery does not hold for them.  Red:
// something is wrong that they cannot see themselves - no keyboard assigned.
constexpr SkColor kDotOk = SkColorSetRGB(0x1E, 0x8E, 0x3E);
constexpr SkColor kDotIdle = SkColorSetRGB(0xF2, 0x99, 0x00);
constexpr SkColor kDotBad = SkColorSetRGB(0xD9, 0x30, 0x25);

// The connection indicator, in the same green as a working user so the two
// read as one language.  Grey rather than red when disconnected: not being
// connected yet is a starting state, not a fault.
constexpr SkColor kLedOff = SkColorSetRGB(0x9A, 0xA0, 0xA6);


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

// What a row displays, in one place.
//
// The row is built once and then updated in place for the rest of its life, so
// every one of these has two callers.  Written as free functions rather than
// duplicated in both: the two drifting apart would show as a row that says one
// thing when it appears and another a second later.
namespace row_text {

SkColor DotColor(const content::MouseMuxInputController::OwnerInfo& owner) {
  if (owner.keyboard_hwid == 0) {
    return kDotBad;
  }
  return owner.captured ? kDotOk : kDotIdle;
}

std::u16string DotTip(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  if (owner.keyboard_hwid == 0) {
    return u"No keyboard assigned to this user in MouseMux. Their typing "
           u"cannot be told from anyone else's.";
  }
  return owner.captured
             ? std::u16string(u"Captured and working.")
             : std::u16string(u"Claimed a window, but not captured. Tick "
                              u"Capture on this row.");
}

std::u16string Name(const content::MouseMuxInputController::OwnerInfo& owner) {
  // An unnamed owner is still an owner and must be operable.
  return owner.name.empty()
             ? base::ASCIIToUTF16(
                   base::StringPrintf("device 0x%x", owner.hwid))
             : base::UTF8ToUTF16(owner.name);
}

std::u16string Keyboard(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  if (owner.keyboard_hwid == 0) {
    return u"no keyboard";
  }
  std::u16string text = base::ASCIIToUTF16(
      base::StringPrintf("kb 0x%x", owner.keyboard_hwid));
  if (owner.keyboard_typed) {
    text += u" \u2713";
  }
  return text;
}

int KeyboardStyle(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  return owner.keyboard_hwid == 0 ? views::style::STYLE_PRIMARY
                                  : views::style::STYLE_SECONDARY;
}

std::u16string KeyboardTip(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  if (owner.keyboard_hwid == 0) {
    return u"MouseMux has no keyboard on this user. Give this user a mouse "
           u"AND a keyboard in MouseMux \u2014 until then their keystrokes "
           u"cannot be told from anyone else's, and with several users they "
           u"are ignored rather than typed into somebody else's window.";
  }
  return owner.keyboard_typed
             ? std::u16string(u"This user's keyboard, and it has typed.")
             : std::u16string(u"This user's keyboard. Nothing has arrived "
                              u"from it yet.");
}

// The screen leads, because on a desk of several monitors that is how an
// operator identifies a person, and because a window title changes as they
// browse while a screen does not.
std::u16string Where(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  if (!owner.has_window) {
    return u"\u2014 not in a window yet";
  }
  std::u16string where;
  if (owner.screen_index > 0) {
    where = u"Screen ";
    where += base::ASCIIToUTF16(
        base::StringPrintf("%d", owner.screen_index));
    where += u" \u00b7 ";
  }
  where += owner.window_title.empty() ? std::u16string(u"(untitled window)")
                                      : owner.window_title;
  return where;
}

int WhereStyle(const content::MouseMuxInputController::OwnerInfo& owner) {
  return owner.has_window ? views::style::STYLE_SECONDARY
                          : views::style::STYLE_DISABLED;
}

}  // namespace row_text

// Wraps a pane in a hairline rounded box.  Border only, no fill: a filled
// panel on a dialog background reads as a control you can press.
void MakePane(views::View* pane) {
  pane->SetBorder(views::CreatePaddedBorder(
      views::CreateRoundedRectBorder(1, kPaneRadius, ui::kColorSeparator),
      gfx::Insets(kPanePadding)));
}

}  // namespace

// The help window.
//
// Deliberately its own window rather than more rows in the dialog, and
// deliberately plain: it is read once, by somebody who is stuck, and then
// closed.  White ground with explicit dark text, so it stays legible whatever
// the system theme does - a help sheet that inverts with the OS theme is one
// more thing to be confused by.
class MouseMuxHelpDialog : public views::DialogDelegateView {
 public:
  MouseMuxHelpDialog() {
    SetTitle(u"MouseMux Multi-Seat Chrome - Help");
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
    SetButtonLabel(ui::mojom::DialogButton::kCancel, u"Close");
    SetModalType(ui::mojom::ModalType::kNone);

    // A window in its own right: system title bar, so it drags and closes the
    // way every other window does.  Chromium's custom dialog frame draws no
    // title bar at all, which left this openable and then stuck wherever it
    // appeared.
    set_use_custom_frame(false);
    SetShowTitle(true);
    SetShowCloseButton(true);
    SetCanResize(true);

    SetBackground(views::CreateSolidBackground(SK_ColorWHITE));
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(), 0));

    auto body = std::make_unique<views::View>();
    auto* body_layout = body->SetLayoutManager(
        std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kVertical,
            gfx::Insets::VH(4, 12), 2));
    body_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    AddSection(body.get(), u"Getting started");
    AddStep(body.get(), u"1.", u"Press Connect. Everything else stays greyed "
                                u"out until MouseMux is connected.");
    AddStep(body.get(), u"2.", u"Sign in to the site everyone will share, in "
                                u"this first window. Do this BEFORE handing "
                                u"windows out.");
    AddStep(body.get(), u"3.", u"Press + Window once per extra person. Each "
                                u"new window is a copy of the current tab, "
                                u"already signed in.");
    AddStep(body.get(), u"4.", u"Move one window to each screen.");
    AddStep(body.get(), u"5.", u"Each person clicks once in their own window "
                                u"with their own mouse. They appear in the "
                                u"Users list.");
    AddStep(body.get(), u"6.", u"Tick Capture on every row. This is required, "
                                u"not optional - see below.");

    AddSection(body.get(), u"Why Capture is required");
    AddBody(body.get(),
            u"Windows has one active window at a time, and Chrome believes "
            u"it: whichever window is active is told it has the keyboard, and "
            u"every other window is told it does not, which is why a caret "
            u"disappears and typing stops. Capture stops each device "
            u"producing ordinary Windows input, so that never happens. With "
            u"anyone left uncaptured, their clicks still interrupt everybody "
            u"else.");

    AddSection(body.get(), u"A row in the Users list");
    AddItem(body.get(), u"Green dot", u"Captured and working.");
    AddItem(body.get(), u"Amber dot",
            u"Owns a window but is not captured yet.");
    AddItem(body.get(), u"Red dot",
            u"No keyboard assigned to this user in MouseMux. Fix it there: "
            u"a user needs BOTH a mouse and a keyboard, or their typing "
            u"cannot be told from anyone else's.");
    AddItem(body.get(), u"kb 0x.. \u2713",
            u"The keyboard MouseMux has attached to this user. The tick means "
            u"something has actually arrived from it.");
    AddItem(body.get(), u"Screen 2 \u00b7 title",
            u"Which screen they are on, and the page they are looking at.");
    AddItem(body.get(), u"Capture",
            u"Stops this one device producing ordinary Windows input.");
    AddItem(body.get(), u"Release",
            u"Hands their window back. They stop driving Chrome until "
            u"somebody clicks to claim it again.");
    AddItem(body.get(), u"\u2715",
            u"Closes the window this person is working in. Closing a window "
            u"releases its user automatically.");

    AddSection(body.get(), u"Handing out windows");
    AddItem(body.get(), u"+ Window",
            u"Another window of THIS browser, copied from the current tab, so "
            u"it is already signed in and shares one session with everybody "
            u"else. This is the normal way to add a person.");
    AddItem(body.get(), u"+ Seat",
            u"A separate browser with its OWN profile and its own dialog. "
            u"Shares no logins with this one. Use it when people must each "
            u"sign in as themselves.");

    AddSection(body.get(), u"Options");
    AddItem(body.get(), u"Keep each user in their own window",
            u"Clicks outside a person's own window are ignored. Cursors still "
            u"move everywhere; only clicks are blocked. Usually what you want "
            u"with several people side by side.");
    AddItem(body.get(), u"Block native mouse input",
            u"A blunt fallback for when capture is not available. It applies "
            u"to every device at once: Windows mouse messages carry no device "
            u"identity, so this cannot be done per person. Capture, on each "
            u"row, is the per-person version.");
    AddItem(body.get(), u"Release hotkey",
            u"Releases capture from the keyboard, for when injected input is "
            u"not working and the mice cannot reach this dialog. The way out "
            u"if anything goes wrong.");
    AddItem(body.get(), u"Release all", u"Hands every window back at once.");

    AddSection(body.get(), u"Collapse and Quit");
    AddItem(body.get(), u"Collapse",
            u"Shrinks this dialog to a small strip when it is in the way. "
            u"Click Expand to bring it back.");
    AddItem(body.get(), u"Quit",
            u"Closes this dialog AND every window of this browser. Use "
            u"Collapse if you only want it out of the way.");

    AddSection(body.get(), u"If typing goes to the wrong window");
    AddBody(body.get(),
            u"Look at the line under the user list: it shows where recent "
            u"keystrokes actually landed. Then check the dots. A red dot "
            u"means that person has no keyboard assigned in MouseMux, which "
            u"is the usual cause. An amber dot means they are not captured, "
            u"and an uncaptured keyboard also types into whichever window "
            u"Windows thinks is active.");

    auto* scroll = AddChildView(std::make_unique<views::ScrollView>());
    scroll->SetContents(std::move(body));
    scroll->SetBackgroundColor(SK_ColorWHITE);
    scroll->ClipHeightTo(0, kHelpHeight);
  }

  MouseMuxHelpDialog(const MouseMuxHelpDialog&) = delete;
  MouseMuxHelpDialog& operator=(const MouseMuxHelpDialog&) = delete;
  ~MouseMuxHelpDialog() override = default;

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(kHelpWidth, kHelpHeight);
  }

 private:
  // Explicit colours throughout: the ground is forced white, so leaving the
  // text to the theme gives white on white the moment anybody runs dark mode.
  static constexpr SkColor kInk = SkColorSetRGB(0x20, 0x21, 0x24);
  static constexpr SkColor kInkSoft = SkColorSetRGB(0x5F, 0x63, 0x68);

  static void AddSection(views::View* parent, const std::u16string& text) {
    auto* label = parent->AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetEnabledColor(kInk);
    label->SetFontList(label->font_list().Derive(
        1, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
    label->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(12, 0, 2, 0)));
  }

  static void AddBody(views::View* parent, const std::u16string& text) {
    auto* label = parent->AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetEnabledColor(kInkSoft);
    label->SetMultiLine(true);
    label->SetMaximumWidth(kHelpWidth - 48);
  }

  // A term and its explanation, which is what most of this page is.
  static void AddItem(views::View* parent,
                      const std::u16string& term,
                      const std::u16string& text) {
    auto* row = parent->AddChildView(std::make_unique<views::View>());
    row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical,
        gfx::Insets::TLBR(4, 0, 0, 0), 0));

    auto* term_label = row->AddChildView(std::make_unique<views::Label>(
        term, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    term_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    term_label->SetEnabledColor(kInk);
    term_label->SetFontList(term_label->font_list().Derive(
        0, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));

    auto* body_label = row->AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    body_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    body_label->SetEnabledColor(kInkSoft);
    body_label->SetMultiLine(true);
    body_label->SetMaximumWidth(kHelpWidth - 48);
  }

  // A numbered step, laid out so the numbers line up down the left.
  static void AddStep(views::View* parent,
                      const std::u16string& number,
                      const std::u16string& text) {
    auto* row = parent->AddChildView(std::make_unique<views::View>());
    auto* row_layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal,
        gfx::Insets::TLBR(3, 0, 0, 0), 8));
    row_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStart);

    auto* num = row->AddChildView(std::make_unique<views::Label>(
        number, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    num->SetEnabledColor(kInk);
    num->SetHorizontalAlignment(gfx::ALIGN_RIGHT);
    num->SetPreferredSize(gfx::Size(18, 0));

    auto* label = row->AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetEnabledColor(kInkSoft);
    label->SetMultiLine(true);
    label->SetMaximumWidth(kHelpWidth - 66);
    row_layout->SetFlexForView(label, 1);
  }
};


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
  // Not "Close": this button ends the seat, browser windows and all.
  // Collapse is what you want when the dialog is merely in the way.
  SetButtonLabel(ui::mojom::DialogButton::kCancel, u"Quit");
  // Not the prominent style.  As the dialog's cancel button it was drawn in
  // filled blue - the loudest thing in the window - which is the wrong
  // emphasis for the one control that closes everybody's windows.
  SetButtonStyle(ui::mojom::DialogButton::kCancel, ui::ButtonStyle::kTonal);

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

  // Report the view tree to the control server, so the state of a dialog
  // that has drawn wrongly can be read from outside instead of guessed at.
  controller->SetDiagnosticsCallback(base::BindRepeating(
      [](MouseMuxControlDialog* dialog) { return dialog->ViewDiagnostics(); },
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
  expand_button->SetMinSize(gfx::Size(0, kRowButtonHeight));
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

  // ---------------------------------------------------------------------
  // Pane 1 - the connection.
  //
  // A status line and one button, not a toggle: connecting is a different
  // kind of thing from the preferences below it, and three identical toggles
  // meaning three unrelated things was most of why this dialog read as a wall
  // of switches.
  // ---------------------------------------------------------------------
  auto* connection_pane = AddChildView(std::make_unique<views::View>());
  MakePane(connection_pane);
  auto* connection_layout =
      connection_pane->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  connection_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  connection_led_ = connection_pane->AddChildView(
      std::make_unique<views::Label>(u"\u25CF",
                                     views::style::CONTEXT_DIALOG_BODY_TEXT,
                                     views::style::STYLE_PRIMARY));
  connection_led_->SetEnabledColor(kLedOff);

  mousemux_status_label_ = connection_pane->AddChildView(
      std::make_unique<views::Label>(u"Not connected to MouseMux",
                                     views::style::CONTEXT_DIALOG_BODY_TEXT,
                                     views::style::STYLE_PRIMARY));
  mousemux_status_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  mousemux_status_label_->SetElideBehavior(gfx::ELIDE_TAIL);
  connection_layout->SetFlexForView(mousemux_status_label_, 1);

  connect_button_ =
      connection_pane->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnConnectClicked,
                              base::Unretained(this)),
          u"Connect"));
  connect_button_->SetMinSize(gfx::Size(96, kRowButtonHeight));
  connect_button_->SetTooltipText(
      u"Open or close the connection to the MouseMux input service. "
      u"Everything else in this dialog depends on it.");

  // ---------------------------------------------------------------------
  // Pane 2 - the people.
  // ---------------------------------------------------------------------
  auto* users_pane = AddChildView(std::make_unique<views::View>());
  MakePane(users_pane);
  auto* users_pane_layout =
      users_pane->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
  layout->SetFlexForView(users_pane, 1);

  auto* owners_header_row =
      users_pane->AddChildView(std::make_unique<views::View>());
  auto* owners_header_layout =
      owners_header_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  owners_header_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  users_header_label_ = owners_header_row->AddChildView(
      std::make_unique<views::Label>(u"Users",
                                     views::style::CONTEXT_DIALOG_BODY_TEXT,
                                     views::style::STYLE_PRIMARY));
  users_header_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  owners_header_layout->SetFlexForView(users_header_label_, 1);

  // The two ways to hand out a window sit in the users heading, because that
  // is what they do - they add a user.  They used to share a row with the
  // hotkey dropdown, where the dropdown took the width and left both buttons
  // elided; a minimum size cannot save a row whose total minimum exceeds the
  // dialog, so that row is gone rather than tuned.
  new_window_button_ = owners_header_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnNewWindowClicked,
                              base::Unretained(this)),
          u"+ Window"));
  new_window_button_->SetMinSize(gfx::Size(104, kRowButtonHeight));
  new_window_button_->SetTooltipText(
      u"Copy the current tab into another window of THIS Chrome - already "
      u"signed in, sharing the same session as the other users. Have the "
      u"next user click in it to claim it.");

  new_seat_button_ = owners_header_row->AddChildView(
      std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnNewSeatClicked,
                              base::Unretained(this)),
          u"+ Seat"));
  new_seat_button_->SetMinSize(gfx::Size(88, kRowButtonHeight));
  new_seat_button_->SetTooltipText(
      u"Launch a separate Chrome with its OWN profile and its own dialog. "
      u"Isolated: shares no logins with this one.");

  // The list scrolls rather than shrinking.
  //
  // It used to be a plain view given whatever vertical space was left over,
  // and when that was not enough BoxLayout did not overflow - it squeezed the
  // last child.  With two users that made the second row SIX pixels tall: its
  // labels were clipped away by the row's own bounds while its checkbox and
  // buttons, which are layer-backed for their ink drops, carried on being
  // composited.  Half a row appeared, and the half that vanished was the half
  // that says who the user is.
  //
  // A scroll view cannot do that.  The rows always get their full height, and
  // when there are more than fit, the list scrolls.
  auto* owner_scroll =
      users_pane->AddChildView(std::make_unique<views::ScrollView>());
  owner_scroll->SetBackgroundColor(ui::kColorSubtleEmphasisBackground);
  owner_scroll->SetDrawOverflowIndicator(false);
  owner_scroll->ClipHeightTo(kListMinHeight, kListMaxHeight);
  users_pane_layout->SetFlexForView(owner_scroll, 1);

  owner_list_ = owner_scroll->SetContents(std::make_unique<views::View>());
  owner_list_->SetBackground(views::CreateRoundedRectBackground(
      ui::kColorSubtleEmphasisBackground, kPaneRadius));
  owner_list_->SetBorder(views::CreateEmptyBorder(gfx::Insets(4)));
  owner_list_->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 2));

  // Where typing actually went.  The rows above say where each user is
  // WORKING; this says where their KEYS landed, and the two differ exactly
  // when something is misconfigured - which is the case nobody can otherwise
  // diagnose from the outside.
  keyboard_note_label_ =
      users_pane->AddChildView(std::make_unique<views::Label>(
          std::u16string(), views::style::CONTEXT_DIALOG_BODY_TEXT,
          views::style::STYLE_SECONDARY));
  keyboard_note_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  keyboard_note_label_->SetElideBehavior(gfx::ELIDE_TAIL);
  keyboard_note_label_->SetVisible(false);

  // ---------------------------------------------------------------------
  // Pane 3 - options that apply to everybody.
  // ---------------------------------------------------------------------
  auto* options_pane = AddChildView(std::make_unique<views::View>());
  MakePane(options_pane);
  options_pane->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));

  hard_lock_checkbox_ = options_pane->AddChildView(
      std::make_unique<views::Checkbox>(
          u"Keep each user in their own window",
          base::BindRepeating(&MouseMuxControlDialog::OnHardLockToggled,
                              base::Unretained(this))));
  hard_lock_checkbox_->SetTooltipText(
      u"Off: a user who clicks another window moves there.\n"
      u"On: clicks outside a user's own window are ignored, so each user is "
      u"confined to one window. Cursors still move freely; only clicks are "
      u"blocked. A user with no window claims the first one they click, and "
      u"closing a window frees its user to claim another.");

  // Native blocking is global on purpose, and cannot sensibly be otherwise:
  // it drops Windows mouse messages, and those carry no device identity -
  // which is the entire reason MouseMux exists.  The per-user equivalent is
  // Capture, on each row, which stops one device producing native input at
  // the server where the device IS known.
  native_input_checkbox_ =
      options_pane->AddChildView(std::make_unique<views::Checkbox>(
          u"Block native mouse input (all devices)",
          base::BindRepeating(&MouseMuxControlDialog::OnNativeInputToggled,
                              base::Unretained(this))));
  native_input_checkbox_->SetTooltipText(
      u"A blunt fallback for when capture is not available. Windows mouse "
      u"messages carry no device identity, so this cannot be done per user - "
      u"capture, on each user's row, is the per-user version of it.");

  // Settings row: the hotkey, and the way out of everything.
  auto* settings_row =
      options_pane->AddChildView(std::make_unique<views::View>());
  auto* settings_layout =
      settings_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
          kToggleSpacing));
  settings_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  settings_row->AddChildView(std::make_unique<views::Label>(
      u"Release hotkey:", views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_SECONDARY));

  hotkey_model_ = std::make_unique<HotkeyComboboxModel>();
  hotkey_dropdown_ =
      settings_row->AddChildView(std::make_unique<views::Combobox>(
          hotkey_model_.get()));
  hotkey_dropdown_->SetCallback(
      base::BindRepeating(&MouseMuxControlDialog::OnHotkeyChanged,
                          base::Unretained(this)));
  hotkey_dropdown_->SetSelectedIndex(0);  // Default: Shift+Escape
  hotkey_dropdown_->SetTooltipText(
      u"Key combination that releases capture, for when injected input is not "
      u"working and the mice cannot reach this dialog.");

  auto* settings_spacer =
      settings_row->AddChildView(std::make_unique<views::View>());
  settings_layout->SetFlexForView(settings_spacer, 1);

  release_all_button_ =
      settings_row->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnReleaseOwnerClicked,
                              base::Unretained(this)),
          u"Release all"));
  release_all_button_->SetMinSize(gfx::Size(0, kRowButtonHeight));
  release_all_button_->SetTooltipText(
      u"Hand every window back. Their devices stop driving Chrome until "
      u"somebody clicks to claim again.");

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
  auto* help_button =
      footer->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnHelpClicked,
                              base::Unretained(this)),
          u"Help"));
  help_button->SetMinSize(gfx::Size(0, kRowButtonHeight));
  help_button->SetTooltipText(
      u"What every control does, and the order to do things in.");

  auto* collapse_button =
      footer->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnCollapseClicked,
                              base::Unretained(this)),
          u"Collapse"));
  collapse_button->SetMinSize(gfx::Size(84, kRowButtonHeight));
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
#endif
  // No trailing spacer: the users pane takes the slack, so the list grows
  // with the window and the options pane stays put at the bottom.
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
  const bool on = hard_lock_checkbox_ && hard_lock_checkbox_->GetChecked();
  content::MouseMuxInputController::GetInstance()->SetHardLock(on);
  LogDebug(std::string("Hard lock: ") + (on ? "ON" : "OFF"));
  RebuildOwnerList();
}

void MouseMuxControlDialog::OnNativeInputToggled() {
  bool is_on = native_input_checkbox_ && native_input_checkbox_->GetChecked();
  LogDebug(std::string("Native input blocking: ") + (is_on ? "ENABLED" : "DISABLED"));

  // Apply immediately to controller.
  auto* controller = content::MouseMuxInputController::GetInstance();
  controller->SetNativeInputBlocked(is_on);
}

void MouseMuxControlDialog::OnNativeBlockingChanged(bool blocked) {
  if (native_input_checkbox_ &&
      native_input_checkbox_->GetChecked() != blocked) {
    native_input_checkbox_->SetChecked(blocked);
  }
}

void MouseMuxControlDialog::OnConnectClicked() {
  auto* controller = content::MouseMuxInputController::GetInstance();
  const bool connect = !controller->IsMouseMuxEnabled();
  if (mousemux_status_label_) {
    mousemux_status_label_->SetText(connect ? u"Connecting to MouseMux..."
                                            : u"Not connected to MouseMux");
  }
  LogDebug(std::string("MouseMux connection: ") +
           (connect ? "CONNECTING" : "DISCONNECTING"));
  controller->SetMouseMuxEnabled(connect);
  UpdateStatusLine();
}

void MouseMuxControlDialog::UpdateStatusLine() {
  auto* controller = content::MouseMuxInputController::GetInstance();
  const bool connected = controller->IsMouseMuxEnabled();

  if (mousemux_status_label_) {
    std::u16string text = u"Not connected to MouseMux";
    if (connected) {
      // Name the version we are actually talking to.  "Which MouseMux are you
      // running" is the first question of every support case, and the server
      // tells us on connect, so nobody should have to go and look.
      const std::string version = controller->GetServerVersion();
      text = version.empty()
                 ? std::u16string(u"Connected to MouseMux")
                 : base::UTF8ToUTF16("Connected to MouseMux " + version);
    }
    mousemux_status_label_->SetText(text);
  }

  if (connection_led_) {
    connection_led_->SetEnabledColor(connected ? kDotOk : kLedOff);
    connection_led_->SetTooltipText(
        connected ? u"Connected to the MouseMux input service."
                  : u"Not connected. Nothing else in this dialog works until "
                    u"it is.");
  }

  if (connect_button_) {
    connect_button_->SetText(connected ? u"Disconnect" : u"Connect");
  }

  if (users_header_label_) {
    const size_t count = controller->GetOwners().size();
    users_header_label_->SetText(
        count == 0 ? std::u16string(u"Users")
                   : base::ASCIIToUTF16(
                         base::StringPrintf("Users (%zu)", count)));
  }

  UpdateEnabledState();
}

void MouseMuxControlDialog::UpdateEnabledState() {
  // Nothing in here does anything without the connection: handing out a
  // window nobody can claim, or arming a lock with no users to lock, is a
  // control that answers a click by doing nothing at all.  Greying them out
  // says which one thing to press instead.
  //
  // Connect is exempt, obviously, and so are Collapse and the window's close
  // button, which belong to the window rather than to MouseMux.
  const bool on =
      content::MouseMuxInputController::GetInstance()->IsMouseMuxEnabled();

  if (new_window_button_) {
    new_window_button_->SetEnabled(on);
  }
  if (new_seat_button_) {
    new_seat_button_->SetEnabled(on);
  }
  if (release_all_button_) {
    release_all_button_->SetEnabled(on);
  }
  if (hard_lock_checkbox_) {
    hard_lock_checkbox_->SetEnabled(on);
  }
  if (native_input_checkbox_) {
    native_input_checkbox_->SetEnabled(on);
  }
  if (hotkey_dropdown_) {
    hotkey_dropdown_->SetEnabled(on);
  }
}

void MouseMuxControlDialog::WindowClosing() {
  // Closing this dialog ends the seat.
  //
  // The windows it hands out have no controls of their own for any of this:
  // once several people are captured, the dialog is the only way to release
  // them, and leaving it closed with users still captured strands everybody
  // with input going somewhere they cannot see.  So the browser goes with it,
  // which is also what "close" means for a single-purpose application.
  //
  // Safe during shutdown: if the browser is already going away, there is
  // nothing left to close and this does nothing.
  // Already shutting down: the dialog is closing BECAUSE the browser is, and
  // asking for another exit from inside one is how you get re-entrancy.
  if (browser_shutdown::IsTryingToQuit()) {
    return;
  }
  LogDebug("Dialog closed - closing this browser");
  chrome::AttemptUserExit();
}

void MouseMuxControlDialog::OnOwnerCaptureToggled(int hwid,
                                                  views::Checkbox* box) {
  auto* controller = content::MouseMuxInputController::GetInstance();
  const bool want = box && box->GetChecked();
  if (want) {
    controller->CaptureOwnerHwid(hwid);
    // Capture gave this dialog OS focus by way of the click that started it;
    // hand it back, or the first thing that user types goes nowhere.
    controller->FocusKeyboardTargetView();
  } else {
    controller->ReleaseCaptureHwid(hwid);
  }
  RebuildOwnerList();
}

void MouseMuxControlDialog::OnConnectionStateChanged(bool connected) {
  if (mousemux_status_label_) {
    mousemux_status_label_->SetText(connected
                                        ? u"Connected to MouseMux"
                                        : u"Not connected to MouseMux");
  }
  if (!connected) {
    // Clear all stale UI state — controller already reset its side.
    owner_hwid_ = -1;
    owner_name_.clear();
    is_captured_ = false;
    UpdateTitle();
    // Controller unblocked native input on disconnect — sync the toggle.
    if (native_input_checkbox_ && native_input_checkbox_->GetChecked()) {
      native_input_checkbox_->SetChecked(false);
    }
  }
  UpdateStatusLine();
  ScheduleRebuild();
}

void MouseMuxControlDialog::OnCaptureStateChanged(bool captured) {
  is_captured_ = captured;
  UpdateStatusLine();
  ScheduleRebuild();
  UpdateTitle();
  LogDebug(std::string("Capture state changed: ") + (captured ? "CAPTURED" : "RELEASED"));
}

void MouseMuxControlDialog::OnHelpClicked() {
  // Parented to this dialog so it travels with the seat it explains, and
  // non-modal so the operator can follow the instructions while reading them.
  views::Widget* help = views::DialogDelegate::CreateDialogWidget(
      std::make_unique<MouseMuxHelpDialog>(),
      /*context=*/gfx::NativeWindow(),
      GetWidget() ? GetWidget()->GetNativeView() : gfx::NativeView());

#ifdef MOUSEMUX_NATIVE_BLOCK
  // Exempt from native input blocking, like the control dialog: this window is
  // ours and the operator drives it with a real mouse.
  if (help && help->GetNativeWindow() && help->GetNativeWindow()->GetHost()) {
    content::g_mousemux_help_hwnd =
        help->GetNativeWindow()->GetHost()->GetAcceleratedWidget();
  }
#endif

  help->Show();
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

  // The keyboard note decides its own visibility and must not be shown again
  // just because everything else was; it is hidden when it has nothing to
  // say.  RebuildOwnerList settles that.
  if (!collapsed) {
    RebuildOwnerList();
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

std::string MouseMuxControlDialog::OwnerMembership() const {
  std::string key;
  for (const auto& owner :
       content::MouseMuxInputController::GetInstance()->GetOwners()) {
    key += base::StringPrintf("%x;", owner.hwid);
  }
  return key;
}

std::string MouseMuxControlDialog::OwnerSignature() const {
  std::string sig;
  for (const auto& owner :
       content::MouseMuxInputController::GetInstance()->GetOwners()) {
    sig += base::StringPrintf(
        "%x|%s|%x|%d|%d|%d|%d|%s;", owner.hwid, owner.name.c_str(),
        owner.keyboard_hwid, owner.keyboard_typed ? 1 : 0,
        owner.captured ? 1 : 0, owner.has_window ? 1 : 0, owner.screen_index,
        base::UTF16ToUTF8(owner.window_title).c_str());
  }
  return sig;
}

void MouseMuxControlDialog::RebuildOwnerList() {
  if (!owner_list_) {
    return;
  }

  const auto owners =
      content::MouseMuxInputController::GetInstance()->GetOwners();
  const std::string membership = OwnerMembership();
  const std::string signature = OwnerSignature();

  // Same people, same everything: there is nothing to write.  Still re-assert
  // the frame, because a rebuild that failed to reach the screen must not be
  // left uncorrected until something else happens to redraw.
  if (membership == owner_membership_ && signature == owner_signature_ &&
      owner_rows_.size() == owners.size()) {
    UpdateKeyboardNote();
    UpdateStatusLine();
    EnsurePainted();
    return;
  }

  // Same people, something else changed: write into the labels that are
  // already there.  This is the common case by a wide margin - every title
  // change as anybody browses lands here - and it destroys no views, so the
  // tooltip under the operator's pointer survives.
  if (membership == owner_membership_ && owner_rows_.size() == owners.size()) {
    for (size_t i = 0; i < owners.size(); ++i) {
      const auto& owner = owners[i];
      OwnerRow& row = owner_rows_[i];
      if (row.dot) {
        row.dot->SetEnabledColor(row_text::DotColor(owner));
        row.dot->SetTooltipText(row_text::DotTip(owner));
      }
      if (row.name) {
        row.name->SetText(row_text::Name(owner));
      }
      if (row.keyboard) {
        row.keyboard->SetText(row_text::Keyboard(owner));
        row.keyboard->SetTextStyle(row_text::KeyboardStyle(owner));
        row.keyboard->SetTooltipText(row_text::KeyboardTip(owner));
      }
      if (row.where) {
        row.where->SetText(row_text::Where(owner));
        row.where->SetTextStyle(row_text::WhereStyle(owner));
      }
      if (row.capture && row.capture->GetChecked() != owner.captured) {
        row.capture->SetChecked(owner.captured);
      }
      if (row.close) {
        row.close->SetEnabled(owner.has_window);
      }
    }
    owner_signature_ = signature;
    UpdateKeyboardNote();
    UpdateStatusLine();
    EnsurePainted();
    return;
  }

  // The set of users changed: build the rows.
  owner_rows_.clear();
  owner_list_->RemoveAllChildViews();
  owner_list_->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 0));
  owner_membership_ = membership;
  owner_signature_ = signature;

  if (owners.empty()) {
    auto* empty = owner_list_->AddChildView(std::make_unique<views::Label>(
        u"No users yet \u2014 have each person click in their own window.",
        views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_DISABLED));
    empty->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    empty->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(6, 6)));
    // Nobody owns anything, so any routing note left over from the last
    // session of use describes people who are no longer here.
    if (keyboard_note_label_) {
      keyboard_note_label_->SetVisible(false);
    }
    UpdateStatusLine();
    EnsurePainted();
    return;
  }

  bool alternate = false;
  for (const auto& owner : owners) {
    OwnerRow entry;
    entry.hwid = owner.hwid;

    // One row per user, banded and separated, so the list reads as a list of
    // records rather than as more rows of dialog.  The columns share a table
    // so they line up down the list: every row used to be an independent box,
    // and a long name moved that row's title somewhere a short name did not.
    auto* row = owner_list_->AddChildView(std::make_unique<views::View>());
    if (alternate) {
      row->SetBackground(views::CreateRoundedRectBackground(
          ui::kColorSysNeutralContainer, 4));
    }
    alternate = !alternate;

    auto* table = row->SetLayoutManager(std::make_unique<views::TableLayout>());
    table->AddColumn(views::LayoutAlignment::kCenter,
                     views::LayoutAlignment::kCenter, 0,
                     views::TableLayout::ColumnSize::kFixed, 14, 14)
        .AddPaddingColumn(0, 6)
        .AddColumn(views::LayoutAlignment::kStart,
                   views::LayoutAlignment::kCenter, 0,
                   views::TableLayout::ColumnSize::kFixed, 78, 78)
        .AddPaddingColumn(0, 8)
        .AddColumn(views::LayoutAlignment::kStart,
                   views::LayoutAlignment::kCenter, 0,
                   views::TableLayout::ColumnSize::kFixed, 88, 88)
        .AddPaddingColumn(0, 8)
        .AddColumn(views::LayoutAlignment::kStretch,
                   views::LayoutAlignment::kCenter, 1.0f,
                   views::TableLayout::ColumnSize::kFixed, 0, 0)
        .AddPaddingColumn(0, 8)
        .AddColumn(views::LayoutAlignment::kStart,
                   views::LayoutAlignment::kCenter, 0,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0)
        .AddPaddingColumn(0, 4)
        .AddColumn(views::LayoutAlignment::kEnd,
                   views::LayoutAlignment::kCenter, 0,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0)
        .AddPaddingColumn(0, 2)
        .AddColumn(views::LayoutAlignment::kEnd,
                   views::LayoutAlignment::kCenter, 0,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0)
        .AddRows(1, 0, kRowButtonHeight);

    // Status, as one glyph.  Green: captured, which is the only state in
    // which several users actually work.  Amber: an owner, but not captured,
    // so Windows is still moving focus behind our back.  Red: no keyboard,
    // which nobody can see from their own seat.
    entry.dot = row->AddChildView(std::make_unique<views::Label>(
        u"\u25CF", views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    entry.dot->SetEnabledColor(row_text::DotColor(owner));
    entry.dot->SetTooltipText(row_text::DotTip(owner));

    entry.name = row->AddChildView(std::make_unique<views::Label>(
        row_text::Name(owner), views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    entry.name->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    entry.name->SetElideBehavior(gfx::ELIDE_TAIL);
    if (owner.is_primary) {
      entry.name->SetTooltipText(
          u"Primary owner \u2014 the one reported to the control server and "
          u"the single-owner API.");
    }

    entry.keyboard = row->AddChildView(std::make_unique<views::Label>(
        row_text::Keyboard(owner), views::style::CONTEXT_DIALOG_BODY_TEXT,
        row_text::KeyboardStyle(owner)));
    entry.keyboard->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    entry.keyboard->SetElideBehavior(gfx::ELIDE_TAIL);
    entry.keyboard->SetTooltipText(row_text::KeyboardTip(owner));

    entry.where = row->AddChildView(std::make_unique<views::Label>(
        row_text::Where(owner), views::style::CONTEXT_DIALOG_BODY_TEXT,
        row_text::WhereStyle(owner)));
    entry.where->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    entry.where->SetElideBehavior(gfx::ELIDE_TAIL);

    // Capture is a STATE, so it is a checkbox rather than a button whose
    // label flips.  That also frees the word "Release" for the other action,
    // which is what it should always have meant: hand the window back.
    entry.capture = row->AddChildView(std::make_unique<views::Checkbox>(
        u"Capture", views::Button::PressedCallback()));
    entry.capture->SetChecked(owner.captured);
    entry.capture->SetCallback(base::BindRepeating(
        &MouseMuxControlDialog::OnOwnerCaptureToggled, base::Unretained(this),
        owner.hwid, base::Unretained(entry.capture.get())));
    entry.capture->SetTooltipText(
        u"Stop this user's device producing native Windows input. Required "
        u"for several users to work at once, and the per-user version of the "
        u"global blocking option below.");

    auto* release_btn = row->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&MouseMuxControlDialog::OnOwnerReleaseClicked,
                            base::Unretained(this), owner.hwid),
        u"Release"));
    release_btn->SetStyle(ui::ButtonStyle::kText);
    // A row action, not a call to action: the default button padding made it
    // the widest thing on the row and pulled the eye to the one control you
    // want people to use least.
    release_btn->SetCustomPadding(gfx::Insets::VH(0, 6));
    release_btn->SetMinSize(gfx::Size(0, kRowButtonHeight));
    release_btn->SetTooltipText(
        u"Hand this window back. Their device stops driving Chrome until "
        u"somebody clicks to claim it again.");

    entry.close = row->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&MouseMuxControlDialog::OnOwnerCloseWindowClicked,
                            base::Unretained(this), owner.hwid),
        u"\u2715"));
    entry.close->SetStyle(ui::ButtonStyle::kText);
    entry.close->SetCustomPadding(gfx::Insets::VH(0, 4));
    entry.close->SetMinSize(gfx::Size(24, kRowButtonHeight));
    entry.close->SetEnabled(owner.has_window);
    entry.close->SetTooltipText(u"Close the window this user is working in.");

    owner_rows_.push_back(entry);
  }

  UpdateKeyboardNote();
  UpdateStatusLine();
  owner_list_->InvalidateLayout();
  EnsurePainted();
}

void MouseMuxControlDialog::ScheduleRebuild() {
  // Posted, not called.
  //
  // Rebuilding inline from a controller callback means rebuilding on whatever
  // stack raised it - and for the callback that matters most, a user claiming
  // a window, that is the injected-input stack.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MouseMuxControlDialog::RebuildOwnerList,
                                weak_factory_.GetWeakPtr()));
}

std::string MouseMuxControlDialog::ViewDiagnostics() const {
  std::string out;
  const views::Widget* widget = GetWidget();
  out += base::StringPrintf(
      "widget=%d visible=%d active=%d minimized=%d contents=%s list=%s rows=%d",
      widget ? 1 : 0, widget && widget->IsVisible() ? 1 : 0,
      widget && widget->IsActive() ? 1 : 0,
      widget && widget->IsMinimized() ? 1 : 0,
      bounds().ToString().c_str(),
      owner_list_ ? owner_list_->bounds().ToString().c_str() : "none",
      owner_list_ ? static_cast<int>(owner_list_->children().size()) : -1);

  if (!owner_list_) {
    return out;
  }

  int row_index = 0;
  for (const views::View* row : owner_list_->children()) {
    out += base::StringPrintf(
        " | row%d %s vis=%d drawn=%d children=%d:", row_index++,
        row->bounds().ToString().c_str(), row->GetVisible() ? 1 : 0,
        row->IsDrawn() ? 1 : 0, static_cast<int>(row->children().size()));
    for (const views::View* cell : row->children()) {
      // Text where there is any: a label with correct bounds that draws
      // nothing would mean the text is empty, which is a different bug from a
      // label that is never painted.
      std::string text;
      if (const auto* label = views::AsViewClass<views::Label>(cell)) {
        text = "'" + base::UTF16ToUTF8(label->GetText()) + "'";
      }
      out += base::StringPrintf(" [%s %s vis=%d drawn=%d %s]",
                                std::string(cell->GetClassName()).c_str(),
                                cell->bounds().ToString().c_str(),
                                cell->GetVisible() ? 1 : 0,
                                cell->IsDrawn() ? 1 : 0, text.c_str());
    }
  }
  return out;
}

void MouseMuxControlDialog::EnsurePainted() {
  // Called on every refresh, not only the ones that changed something: a
  // rebuild that failed to reach the screen must not stay uncorrected until
  // something else happens to redraw the window.  It destroys no views, so
  // tooltips survive it.
  if (views::Widget* widget = GetWidget()) {
    widget->LayoutRootViewIfNecessary();
    if (ui::Compositor* compositor = widget->GetCompositor()) {
      compositor->ScheduleFullRedraw();
    }
    widget->GetRootView()->SchedulePaint();
  }
  SchedulePaint();
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
    // The row already says "no keyboard" against the user it belongs to, and
    // its tooltip explains what to do about it. Repeating the explanation
    // underneath said the same thing twice and made a two-user dialog look
    // like an error report.
    keyboard_note_label_->SetVisible(false);
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

void MouseMuxControlDialog::OnOwnerReleaseClicked(int hwid) {
  // ReleaseOwnerHwid releases capture for this device before dropping it.
  content::MouseMuxInputController::GetInstance()->ReleaseOwnerHwid(hwid);
  RebuildOwnerList();
}

void MouseMuxControlDialog::OnOwnerCloseWindowClicked(int hwid) {
  // Resolved now, not when the row was built.  Rows are long-lived and people
  // move between windows, so a handle captured at build time can name a window
  // that user has left - or one that no longer exists.
  gfx::AcceleratedWidget window = gfx::AcceleratedWidget();
  for (const auto& owner :
       content::MouseMuxInputController::GetInstance()->GetOwners()) {
    if (owner.hwid == hwid) {
      window = owner.window;
      break;
    }
  }
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

  UpdateStatusLine();
  ScheduleRebuild();

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
