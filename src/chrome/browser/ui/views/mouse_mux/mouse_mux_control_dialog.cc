// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/mouse_mux/mouse_mux_control_dialog.h"

#ifdef MOUSEMUX_DEBUG
#include <algorithm>
#include <array>
#include <set>
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
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "base/process/launch.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "ui/base/window_open_disposition.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/views/mouse_mux/mouse_mux_window_number.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "content/browser/renderer_host/render_widget_host_view_aura.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"
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
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/separator.h"
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

// Shown in the dialog footer.  Stamped in mouse_mux_config.h with the
// version the client reports, so the two cannot drift.
constexpr int kBuildNumber = MOUSEMUX_BUILD_NUMBER;

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

// One dialog, whatever the build.  Debug and release used to differ in size
// and in what they showed; they no longer do, and the build date is the same
// date for both.
//
// Wider than it was: the screen-and-page column is the one an operator
// actually reads, and at 560 the fixed columns and three row controls left it
// about 120px, so it elided to "(un...".
// 740: the users list gained a window column right after the name and lines
// between its columns (2026-09-04); with the fixed columns that leaves the
// window title about 190px, enough to read.
constexpr int kDialogWidth = 740;
// Measured, not guessed: the contents view comes out about 84px shorter than
// this, because the frame's button row is taken out of it.  At 424 everything
// was already at its minimum height with nothing left over, which is how the
// user list ended up 42px tall holding two rows.
constexpr int kDialogHeight = 520;

// Date only, from the stamp.  Build flavour belongs in kProductName above,
// where it is #ifdef-guarded and cannot lie.
const char kBuildDate[] = MOUSEMUX_BUILD_DATE;

#ifdef MOUSEMUX_DEBUG
constexpr int kLogFlushThreshold = 5;
// A function, not a constant: the path is resolved at runtime into %TEMP%.
std::string LogFilePath() { return MOUSEMUX_DEBUG_LOG_PATH; }
#endif

// How much of the user list is shown before it scrolls.  Outside the debug
// guard because the list exists in both builds - it was inside, so turning
// debug on stopped the dialog compiling.
constexpr int kListMinHeight = 40;
constexpr int kListMaxHeight = 240;

// Collapsed strip: no title bar, no close button, no build footer — just the
// icon and one button, so it can be much smaller than the dialog.
constexpr int kCollapsedWidth = 150;
constexpr int kCollapsedHeight = 52;
// Outside the debug guard: collapsing works in both builds, so its sizes
// cannot live in the non-debug branch.

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
// The users list is a list of records: one table definition shared by the
// header row and every user row, so the columns line up down the list and the
// vertical lines between them read as continuous.  All widths are fixed
// except the window column, which takes the rest; a checkbox or button that
// wants more than its column is clipped rather than pushing the others.
//
// Columns: status dot | name || window || keyboard || typing || Capture |
// Block native | Release | x.  "||" is a one-pixel line with 3px either side.
void AddOwnerColumns(views::TableLayout* table) {
  auto fixed = [table](int width, views::LayoutAlignment h) {
    table->AddColumn(h, views::LayoutAlignment::kCenter, 0,
                     views::TableLayout::ColumnSize::kFixed, width, width);
  };
  auto line = [table]() {
    table->AddPaddingColumn(0, 3)
        .AddColumn(views::LayoutAlignment::kCenter,
                   views::LayoutAlignment::kStretch, 0,
                   views::TableLayout::ColumnSize::kFixed, 1, 1)
        .AddPaddingColumn(0, 3);
  };
  fixed(14, views::LayoutAlignment::kCenter);  // dot
  table->AddPaddingColumn(0, 6);
  fixed(72, views::LayoutAlignment::kStart);  // name
  line();
  table->AddColumn(views::LayoutAlignment::kStretch,
                   views::LayoutAlignment::kCenter, 1.0f,
                   views::TableLayout::ColumnSize::kFixed, 0, 0);  // window
  line();
  fixed(84, views::LayoutAlignment::kStart);  // keyboard
  line();
  fixed(52, views::LayoutAlignment::kStart);  // typing
  line();
  fixed(76, views::LayoutAlignment::kStart);   // Capture
  table->AddPaddingColumn(0, 4);
  fixed(100, views::LayoutAlignment::kStart);  // Block native
  table->AddPaddingColumn(0, 4);
  fixed(60, views::LayoutAlignment::kEnd);     // Release
  table->AddPaddingColumn(0, 2);
  fixed(24, views::LayoutAlignment::kEnd);     // x
  table->AddRows(1, 0, kRowButtonHeight);
}

views::Separator* AddColumnLine(views::View* row) {
  auto* line = row->AddChildView(std::make_unique<views::Separator>());
  line->SetOrientation(views::Separator::Orientation::kVertical);
  line->SetPreferredLength(kRowButtonHeight);
  return line;
}

views::Label* AddHeaderCell(views::View* header, const std::u16string& text) {
  auto* label = header->AddChildView(std::make_unique<views::Label>(
      text, views::style::CONTEXT_DIALOG_BODY_TEXT,
      views::style::STYLE_SECONDARY));
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  return label;
}

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
// The window's number in the order the windows were opened: the first
// window is 1, the one made with "+ Window" is 2, and so on.  Stable for as
// long as the window exists, and what a person counting windows would say.
// 0 when the window is not a browser window.
int WindowNumberOf(gfx::AcceleratedWidget window) {
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    ui::BaseWindow* base_window = browser->GetWindow();
    aura::Window* native =
        base_window ? base_window->GetNativeWindow() : nullptr;
    if (native && native->GetHost() &&
        native->GetHost()->GetAcceleratedWidget() == window) {
      // The same number the window's caption shows; see the header.
      return WindowNumberForSession(browser->GetSessionID());
    }
  }
  return 0;
}

// "Window 2 \u00b7 Gmail".  Two windows on the same page used to read
// identically (the title is the active tab's); the number tells them apart.
// The screen is not shown: it does not help tell windows apart, and the
// operator can see where a window is.
std::u16string Where(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  if (!owner.has_window) {
    return u"\u2014 not in a window yet";
  }
  std::u16string where;
  if (const int number = WindowNumberOf(owner.window); number > 0) {
    where = u"Window ";
    where += base::ASCIIToUTF16(base::StringPrintf("%d", number));
    where += u" \u00b7 ";
  }
  where += owner.window_title.empty() ? std::u16string(u"(untitled window)")
                                      : owner.window_title;
  if (owner.extra_windows > 0) {
    where += base::ASCIIToUTF16(base::StringPrintf(" +%d", owner.extra_windows));
  }
  return where;
}

int WhereStyle(const content::MouseMuxInputController::OwnerInfo& owner) {
  return owner.has_window ? views::style::STYLE_SECONDARY
                          : views::style::STYLE_DISABLED;
}

// Typing activity, as one glyph. A sentence naming where everybody's keys
// went was unreadable with two users and would be absurd with twenty; this is
// the same information at a glance, per row.
std::u16string Typing(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  // A word, not a symbol. A 16px glyph in a row of text is invisible, and
  // the keyboard character may have no font at all - the operator asked for
  // this three times before it was legible.
  if (owner.typing == 1) {
    return u"typing";
  }
  if (owner.typing == 2) {
    return u"IGNORED";
  }
  return std::u16string();
}

SkColor TypingColor(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  if (owner.typing == 1) {
    return kDotOk;
  }
  if (owner.typing == 2) {
    return kDotBad;
  }
  return SkColorSetRGB(0xBD, 0xC1, 0xC6);
}

std::u16string TypingTip(
    const content::MouseMuxInputController::OwnerInfo& owner) {
  if (owner.typing == 1) {
    return u"Typing now, and their keys are reaching their window.";
  }
  if (owner.typing == 2) {
    return u"Typing now, but their keys are being ignored - see the keyboard "
           u"column. A keyboard that is not assigned to this user in MouseMux "
           u"cannot be routed.";
  }
  return u"Not typing.";
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
    AddStep(body.get(), u"0.", u"In MouseMux: Switched mode, Multi-keyboard "
                                u"ON. Both - see below.");
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

    AddSection(body.get(), u"Why Multi-keyboard is required");
    AddBody(body.get(),
            u"Chrome receives every user's keystrokes over the MouseMux "
            u"connection. Whether Windows ALSO delivers them to whatever "
            u"program is in the foreground is decided by MouseMux, not by "
            u"Chrome. In Switched mode without Multi-keyboard the keyboard "
            u"stays a normal Windows keyboard, so each key typed into a "
            u"Chrome window is typed a second time into the foreground "
            u"program - which may be somebody else's. With Multi-keyboard "
            u"on, MouseMux keeps the keyboard's input to itself and Chrome "
            u"is the only place it arrives. Block native stops the duplicate "
            u"inside Chrome's own windows; nothing in Chrome can stop it in "
            u"other programs.");

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
    AddItem(body.get(), u"Window 2 \u00b7 title",
            u"Which window they are working in - numbered in the order the "
            u"windows were opened - and the page it is showing.");
    AddItem(body.get(), u"Capture",
            u"Stops this one device producing ordinary Windows input.");
    AddItem(body.get(), u"Block native",
            u"Drops the real mouse's input inside this user's windows. On by "
            u"default - an owned window is somebody's workplace. Untick it to "
            u"let the operator's own mouse work inside them.");
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
  ~MouseMuxHelpDialog() override {
#ifdef MOUSEMUX_NATIVE_BLOCK
    // The exemption goes with the window: Windows reuses handles, and a
    // browser window that inherited this one would be exempt from blocking.
    if (GetWidget() && GetWidget()->GetNativeWindow() &&
        GetWidget()->GetNativeWindow()->GetHost() &&
        content::g_mousemux_help_hwnd ==
            GetWidget()->GetNativeWindow()->GetHost()->GetAcceleratedWidget()) {
      content::g_mousemux_help_hwnd = nullptr;
    }
#endif
  }

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

// A message from the MouseMux server (timeout warning, session ended) that
// the operator should see.  A window of its own and non-modal: a MessageBox
// would spin a nested message loop on the UI thread and stall every user's
// input until the operator dismissed it.
class MouseMuxNoticeDialog : public views::DialogDelegateView {
 public:
  MouseMuxNoticeDialog(const std::u16string& text, bool error) {
    SetTitle(error ? u"MouseMux - session ended" : u"MouseMux - warning");
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
    SetButtonLabel(ui::mojom::DialogButton::kOk, u"Close");
    SetModalType(ui::mojom::ModalType::kNone);
    set_use_custom_frame(false);
    SetShowTitle(true);
    SetShowCloseButton(true);
    SetBackground(views::CreateSolidBackground(SK_ColorWHITE));
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(16, 20), 0));
    auto* label = AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    label->SetMultiLine(true);
    label->SetMaximumWidth(360);
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetEnabledColor(SkColorSetRGB(0x20, 0x21, 0x24));
  }
  ~MouseMuxNoticeDialog() override = default;

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(400, 110);
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
  // And not the default button: the frame draws the default one prominently
  // whatever style it was given, which is how the control that closes
  // everybody's windows ended up as the brightest thing in the dialog.
  SetDefaultButton(static_cast<int>(ui::mojom::DialogButton::kNone));

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

  // Tell the controller which view belongs to the active tab of a window.
  //
  // Only this layer can answer that: a tab strip is a chrome/browser/ui
  // concept and content/ cannot see one.  Without it the controller has to
  // infer the active tab from the views it happens to have registered, and
  // that inference was wrong in a way that swallowed every keystroke.
  controller->SetActiveViewForWindowCallback(base::BindRepeating(
      [](gfx::AcceleratedWidget window)
          -> content::RenderWidgetHostViewAura* {
        if (!window) {
          return nullptr;
        }
        for (BrowserWindowInterface* browser :
             GetAllBrowserWindowInterfaces()) {
          ui::BaseWindow* base_window = browser->GetWindow();
          aura::Window* native =
              base_window ? base_window->GetNativeWindow() : nullptr;
          if (!native || !native->GetHost() ||
              native->GetHost()->GetAcceleratedWidget() != window) {
            continue;
          }
          TabStripModel* model = browser->GetTabStripModel();
          if (!model) {
            return nullptr;
          }
          content::WebContents* active = model->GetActiveWebContents();
          if (!active) {
            return nullptr;
          }
          return static_cast<content::RenderWidgetHostViewAura*>(
              active->GetRenderWidgetHostView());
        }
        return nullptr;
      }));

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
  controller->SetNoticeCallback(
      base::BindRepeating(&MouseMuxControlDialog::ShowNotice,
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
      [](int* menu_owner_hwid, int* pending_menu_hwid, int hwid,
         bool page_press) {
        auto* menu = views::MenuController::GetActiveInstance();
        if (!menu) {
          // Nothing open.  If THIS press opens a menu, it belongs to this
          // device.  Every press counts here, page or Chrome UI: the ...
          // button is UI.
          *menu_owner_hwid = -1;
          *pending_menu_hwid = hwid;
          return;
        }
        if (*menu_owner_hwid == -1) {
          // First press seen since this menu appeared: attribute it to
          // whoever pressed last while nothing was open.
          *menu_owner_hwid = *pending_menu_hwid;
        }
        // Only the owner's press on a PAGE closes it: their press on the
        // menu itself is a UI press and picks an item.  Anybody else's
        // press, anywhere, leaves it alone.
        if (page_press && *menu_owner_hwid == hwid) {
          menu->Cancel(views::MenuController::ExitType::kAll);
          *menu_owner_hwid = -1;
        }
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
  // Every callback registered in the constructor points at this object.  The
  // browser windows outlive the dialog during exit and each closing tab
  // reports an ownership change, so the controller must forget us first.
  content::MouseMuxInputController::GetInstance()->ClearUiCallbacks();
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
  std::ofstream file(LogFilePath(), std::ios::app);
  if (file.is_open()) {
    for (const auto& msg : log_buffer_) {
      file << msg << "\n";
    }
    file.close();
  }
  log_buffer_.clear();
}

void MouseMuxControlDialog::WriteToLogFile(const std::string& message) {
  std::ofstream file(LogFilePath(), std::ios::app);
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

  // Column titles above the list, on the same table definition as the rows
  // so the lines between the columns continue through the header.  The
  // control columns have no title: the checkboxes and buttons name
  // themselves.
  {
    auto* header = users_pane->AddChildView(std::make_unique<views::View>());
    AddOwnerColumns(
        header->SetLayoutManager(std::make_unique<views::TableLayout>()));
    header->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(0, 8, 0, 8)));
    // Titles only; the lines between the columns start with the rows.  In
    // the header they looked like a fence.
    AddHeaderCell(header, u"");  // dot
    AddHeaderCell(header, u"User");
    AddHeaderCell(header, u"");  // line column, empty here
    AddHeaderCell(header, u"Window");
    AddHeaderCell(header, u"");
    AddHeaderCell(header, u"Keyboard");
    AddHeaderCell(header, u"");
    AddHeaderCell(header, u"Typing");
    AddHeaderCell(header, u"");
    AddHeaderCell(header, u"");  // Capture
    AddHeaderCell(header, u"");  // Block native
    AddHeaderCell(header, u"");  // Release
    AddHeaderCell(header, u"");  // x
  }

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

  // ---------------------------------------------------------------------
  // Pane 3 - options that apply to everybody.
  // ---------------------------------------------------------------------
  auto* options_pane = AddChildView(std::make_unique<views::View>());
  MakePane(options_pane);
  options_pane->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));

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

  // Footer, in the frame's button row beside Close: build info and Collapse.
  // In every build: it used to be release-only, from the days the debug
  // dialog kept a log panel in this spot; the panel is long gone, and a debug
  // build without Help and Collapse was just a worse dialog (2026-09-04).
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

  save_layout_button_ =
      footer->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnSaveLayoutClicked,
                              base::Unretained(this)),
          u"Save layout"));
  save_layout_button_->SetMinSize(gfx::Size(0, kRowButtonHeight));
  save_layout_button_->SetTooltipText(
      u"Remember every window: where it is, what it shows, and whose it is. "
      u"One file in the profile folder.");

  // Remote seats.  Greyed out when this Chrome has no control server (no
  // --mousemux-control-port): the pages come from it.
  const bool has_control_port =
      !base::CommandLine::ForCurrentProcess()
           ->GetSwitchValueASCII("mousemux-control-port")
           .empty();
  auto* host_button =
      footer->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnHostClicked,
                              base::Unretained(this)),
          u"Host"));
  host_button->SetMinSize(gfx::Size(0, kRowButtonHeight));
  host_button->SetEnabled(has_control_port);
  host_button->SetTooltipText(
      u"Share windows of this Chrome with remote people: each shared window "
      u"gets a code, and the remote person becomes its user. Needs "
      u"--mousemux-control-port.");
  auto* view_button =
      footer->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnViewClicked,
                              base::Unretained(this)),
          u"View"));
  view_button->SetMinSize(gfx::Size(0, kRowButtonHeight));
  view_button->SetEnabled(has_control_port);
  view_button->SetTooltipText(
      u"Join a window shared from another machine by its code, and work in "
      u"it with your own mouse and keyboard.");

  load_layout_button_ =
      footer->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&MouseMuxControlDialog::OnLoadLayoutClicked,
                              base::Unretained(this)),
          u"Load layout"));
  load_layout_button_->SetMinSize(gfx::Size(0, kRowButtonHeight));
  load_layout_button_->SetTooltipText(
      u"Put the saved windows back: position, page, and owner. Needs the "
      u"connection: owners are matched by their MouseMux name. Users whose "
      u"devices are connected get their window at once; the others claim by "
      u"clicking, as usual.");

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

  // No in-dialog log panel, even in debug builds.  It duplicated the log file,
  // and as a second flex child it fought the users pane for height until the
  // panes overlapped and the options pane - checkbox and all - was pushed off
  // the bottom.  A debug dialog that is missing controls the release dialog
  // has cannot be used to test the release dialog.  The log is on disk.
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


void MouseMuxControlDialog::OnOwnerBlockToggled(int hwid,
                                                views::Checkbox* box) {
  content::MouseMuxInputController::GetInstance()->SetOwnerBlockNative(
      hwid, box && box->GetChecked());
  RebuildOwnerList();
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
  if (load_layout_button_) {
    load_layout_button_->SetEnabled(on);
  }
  // Save too: without the connection there are no owners to save, and a
  // layout saved then would overwrite one that had them.
  if (save_layout_button_) {
    save_layout_button_->SetEnabled(on);
  }
  if (release_all_button_) {
    release_all_button_->SetEnabled(on);
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
    is_captured_ = false;
  }
  UpdateStatusLine();
  ScheduleRebuild();
}

void MouseMuxControlDialog::OnCaptureStateChanged(bool captured) {
  is_captured_ = captured;
  UpdateStatusLine();
  ScheduleRebuild();
  LogDebug(std::string("Capture state changed: ") + (captured ? "CAPTURED" : "RELEASED"));
}

void MouseMuxControlDialog::ShowNotice(const std::string& text, bool error) {
  LogDebug("Notice: " + text);
  views::Widget* notice = views::DialogDelegate::CreateDialogWidget(
      std::make_unique<MouseMuxNoticeDialog>(base::UTF8ToUTF16(text), error),
      /*context=*/gfx::NativeWindow(),
      GetWidget() ? GetWidget()->GetNativeView() : gfx::NativeView());
  notice->Show();
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
        "%x|%s|%x|%d|%d|%d|%s;", owner.hwid, owner.name.c_str(),
        owner.keyboard_hwid, owner.keyboard_typed ? 1 : 0,
        owner.captured ? 1 : 0, owner.has_window ? 1 : 0,
        base::UTF16ToUTF8(owner.window_title).c_str());
    sig += base::StringPrintf("%d;%d;", owner.typing,
                              owner.block_native ? 1 : 0);
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
  if (list_built_ && membership == owner_membership_ &&
      signature == owner_signature_ && owner_rows_.size() == owners.size()) {
    UpdateStatusLine();
    EnsurePainted();
    return;
  }

  // Same people, something else changed: write into the labels that are
  // already there.  This is the common case by a wide margin - every title
  // change as anybody browses lands here - and it destroys no views, so the
  // tooltip under the operator's pointer survives.
  if (list_built_ && membership == owner_membership_ &&
      owner_rows_.size() == owners.size()) {
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
        row.where->SetPreferredSize(gfx::Size(0, kRowButtonHeight));
      }
      if (row.typing) {
        row.typing->SetText(row_text::Typing(owner));
        row.typing->SetEnabledColor(row_text::TypingColor(owner));
        row.typing->SetTooltipText(row_text::TypingTip(owner));
      }
      if (row.capture && row.capture->GetChecked() != owner.captured) {
        row.capture->SetChecked(owner.captured);
      }
      if (row.block && row.block->GetChecked() != owner.block_native) {
        row.block->SetChecked(owner.block_native);
      }
      if (row.close) {
        row.close->SetEnabled(owner.has_window);
      }
    }
    owner_signature_ = signature;
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
  list_built_ = true;

  if (owners.empty()) {
    auto* empty = owner_list_->AddChildView(std::make_unique<views::Label>(
        u"No users yet \u2014 have each person click in their own window.",
        views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_DISABLED));
    empty->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    empty->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(6, 6)));
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

    AddOwnerColumns(
        row->SetLayoutManager(std::make_unique<views::TableLayout>()));

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

    AddColumnLine(row);
    entry.where = row->AddChildView(std::make_unique<views::Label>(
        row_text::Where(owner), views::style::CONTEXT_DIALOG_BODY_TEXT,
        row_text::WhereStyle(owner)));
    entry.where->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    entry.where->SetElideBehavior(gfx::ELIDE_TAIL);
    // A long page title must not decide how wide the dialog is.  The label
    // elides visually but still reports the FULL text as its preferred width,
    // which the table adds up and the window then grows to fit - one long
    // title took the dialog to 1857px.
    entry.where->SetPreferredSize(gfx::Size(0, kRowButtonHeight));
    AddColumnLine(row);
    entry.keyboard = row->AddChildView(std::make_unique<views::Label>(
        row_text::Keyboard(owner), views::style::CONTEXT_DIALOG_BODY_TEXT,
        row_text::KeyboardStyle(owner)));
    entry.keyboard->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    entry.keyboard->SetElideBehavior(gfx::ELIDE_TAIL);
    entry.keyboard->SetTooltipText(row_text::KeyboardTip(owner));

    AddColumnLine(row);
    entry.typing = row->AddChildView(std::make_unique<views::Label>(
        row_text::Typing(owner), views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    entry.typing->SetEnabledColor(row_text::TypingColor(owner));
    entry.typing->SetTooltipText(row_text::TypingTip(owner));


    // Capture is a STATE, so it is a checkbox rather than a button whose
    // label flips.  That also frees the word "Release" for the other action,
    // which is what it should always have meant: hand the window back.
    AddColumnLine(row);
    entry.capture = row->AddChildView(std::make_unique<views::Checkbox>(
        u"Capture", views::Button::PressedCallback()));
    entry.capture->SetChecked(owner.captured);
    entry.capture->SetCallback(base::BindRepeating(
        &MouseMuxControlDialog::OnOwnerCaptureToggled, base::Unretained(this),
        owner.hwid, base::Unretained(entry.capture.get())));
    entry.capture->SetTooltipText(
        u"Stop this user's device producing native Windows input. Required "
        u"for several users to work at once.");

    // Native blocking is per owner: the real mouse's input is dropped inside
    // this user's windows.  On by default; off lets the operator's own mouse
    // work inside them.
    entry.block = row->AddChildView(std::make_unique<views::Checkbox>(
        u"Block native", views::Button::PressedCallback()));
    entry.block->SetChecked(owner.block_native);
    entry.block->SetCallback(base::BindRepeating(
        &MouseMuxControlDialog::OnOwnerBlockToggled, base::Unretained(this),
        owner.hwid, base::Unretained(entry.block.get())));
    entry.block->SetTooltipText(
        u"Drop the real mouse's input inside this user's windows. On by "
        u"default: an owned window is somebody's workplace. Untick to let "
        u"the operator's own mouse work inside them.");

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

  // The routing history, which is the thing worth reading when input goes
  // somewhere unexpected.  On the dialog it is one line of elided text, which
  // is unreadable; here it can be printed properly.
  for (const auto& route :
       content::MouseMuxInputController::GetInstance()->GetKeyRoutes()) {
    out += base::StringPrintf(
        " | route kb=0x%x -> mouse=0x%x %s x%d '%s'", route.keyboard_hwid,
        route.mouse_hwid, route.dropped ? "DROPPED" : "delivered", route.count,
        base::UTF16ToUTF8(route.window_title).c_str());
  }

  out += content::MouseMuxInputController::GetInstance()->GetInjectionStats();
  out += content::MouseMuxInputController::GetInstance()->GetViewInventory();

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
  OpenWindowCopy();
}

BrowserWindowInterface* MouseMuxControlDialog::OpenWindowCopy() {
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
  // Whatever gets created below is found by elimination: neither the
  // duplicate-and-move path nor NewEmptyWindow hands the new window back.
  std::set<BrowserWindowInterface*> before;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    before.insert(browser);
  }
  auto newcomer = [&before]() -> BrowserWindowInterface* {
    for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
      if (!before.count(browser)) {
        return browser;
      }
    }
    return nullptr;
  };

  if (!source) {
    // Nothing to copy from -- first run, or every window already closed.
    for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
      if (Profile* profile = browser->GetProfile()) {
        chrome::NewEmptyWindow(profile);
        return newcomer();
      }
    }
    LogDebug("New window: no existing browser to take a profile from");
    return nullptr;
  }

  TabStripModel* model = source->GetTabStripModel();
  const int index = model->active_index();
  if (!chrome::CanDuplicateTabAt(source, index)) {
    // Some tabs cannot be duplicated -- a crashed one, for instance.  A blank
    // window is not what was asked for, but it is still a window.
    chrome::NewEmptyWindow(source->GetProfile());
    return newcomer();
  }

  content::WebContents* duplicate = chrome::DuplicateTabAt(source, index);
  if (!duplicate) {
    chrome::NewEmptyWindow(source->GetProfile());
    return newcomer();
  }

  // Find the copy by identity rather than by assuming where it landed:
  // pinning and tab groups both move it.
  const int duplicate_index = model->GetIndexOfWebContents(duplicate);
  if (duplicate_index == TabStripModel::kNoTab ||
      !chrome::CanMoveTabsToNewWindow(source, {duplicate_index})) {
    // The duplicate exists and is signed in where it is; leaving it as a tab
    // loses nothing, and the operator can still drag it out by hand.
    LogDebug("New window: duplicated tab could not be moved out");
    return nullptr;
  }
  chrome::MoveTabsToNewWindow(source, {duplicate_index});
  return newcomer();
}

namespace {

// Where the layout lives: next to the profile it describes, so a seat with
// its own profile has its own layout.
base::FilePath LayoutPath() {
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (Profile* profile = browser->GetProfile()) {
      return profile->GetPath().Append(FILE_PATH_LITERAL("mousemux-layout.json"));
    }
  }
  return base::FilePath();
}

gfx::AcceleratedWidget WindowOf(BrowserWindowInterface* browser) {
  Browser* b = browser->GetBrowserForMigrationOnly();
  BrowserView* view = b ? BrowserView::GetBrowserViewForBrowser(b) : nullptr;
  if (!view) {
    return gfx::kNullAcceleratedWidget;
  }
  aura::Window* native = view->GetNativeWindow();
  return native && native->GetHost() ? native->GetHost()->GetAcceleratedWidget()
                                     : gfx::kNullAcceleratedWidget;
}

}  // namespace

void MouseMuxControlDialog::OnSaveLayoutClicked() {
  auto* controller = content::MouseMuxInputController::GetInstance();
  const std::vector<content::MouseMuxInputController::OwnerInfo> owners =
      controller->GetOwners();

  // One entry per normal window, in window-number order, which is the order
  // the windows were opened - the same numbers the Users list shows.
  std::vector<std::pair<int, base::DictValue>> entries;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
        !browser->GetWindow() || !browser->GetTabStripModel()) {
      continue;
    }
    const gfx::AcceleratedWidget window = WindowOf(browser);
    base::DictValue entry;
    entry.Set("number", WindowNumberForSession(browser->GetSessionID()));
    const gfx::Rect bounds = browser->GetWindow()->GetRestoredBounds();
    entry.Set("x", bounds.x());
    entry.Set("y", bounds.y());
    entry.Set("width", bounds.width());
    entry.Set("height", bounds.height());
    entry.Set("maximized", browser->GetWindow()->IsMaximized());
    if (content::WebContents* active =
            browser->GetTabStripModel()->GetActiveWebContents()) {
      entry.Set("url", active->GetLastCommittedURL().spec());
    }
    const int owner_hwid = controller->OwnerOfWindow(window);
    for (const auto& owner : owners) {
      if (owner.hwid == owner_hwid) {
        entry.Set("owner", owner.name);
        entry.Set("captured", owner.captured);
        entry.Set("block_native", owner.block_native);
        break;
      }
    }
    entries.emplace_back(WindowNumberForSession(browser->GetSessionID()),
                         std::move(entry));
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  base::ListValue windows;
  for (auto& [number, entry] : entries) {
    windows.Append(std::move(entry));
  }
  base::DictValue layout;
  layout.Set("version", 1);
  layout.Set("windows", std::move(windows));

  const base::FilePath path = LayoutPath();
  if (path.empty()) {
    LogDebug("Save layout: no profile to save next to");
    return;
  }
  std::string json;
  base::JSONWriter::WriteWithOptions(
      layout, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
  LogDebug(base::StringPrintf("Save layout: %zu windows -> %s", entries.size(),
                              path.AsUTF8Unsafe().c_str()));
  // Disk is not the UI thread's business.
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](const base::FilePath& path, const std::string& json) {
            base::WriteFile(path, json);
          },
          path, std::move(json)));
}

void MouseMuxControlDialog::OnHostClicked() {
  OpenControlPage("host.html");
}

void MouseMuxControlDialog::OnViewClicked() {
  OpenControlPage("view.html");
}

void MouseMuxControlDialog::OpenControlPage(const char* file) {
  const std::string port =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          "mousemux-control-port");
  if (port.empty()) {
    LogDebug("Remote seats: no --mousemux-control-port, nothing to open");
    return;
  }
  Profile* profile = nullptr;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if ((profile = browser->GetProfile())) {
      break;
    }
  }
  if (!profile) {
    return;
  }
  // A window of its own: the page must not sit in a window somebody owns,
  // or that user's clicks would land in it.
  NavigateParams params(profile,
                        GURL("http://127.0.0.1:" + port + "/" + file),
                        ui::PAGE_TRANSITION_TYPED);
  params.disposition = WindowOpenDisposition::NEW_WINDOW;
  Navigate(&params);
}

void MouseMuxControlDialog::OnLoadLayoutClicked() {
  const base::FilePath path = LayoutPath();
  if (path.empty()) {
    LogDebug("Load layout: no profile to load from");
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce([](const base::FilePath& path)
                         -> std::optional<base::DictValue> {
        std::string json;
        if (!base::ReadFileToString(path, &json)) {
          return std::nullopt;
        }
        return base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
      }, path),
      base::BindOnce(&MouseMuxControlDialog::ApplyLayout,
                     weak_factory_.GetWeakPtr()));
}

void MouseMuxControlDialog::ApplyLayout(std::optional<base::DictValue> layout) {
  if (!layout) {
    LogDebug("Load layout: no layout file, or not JSON");
    return;
  }
  const base::ListValue* windows = layout->FindList("windows");
  if (!windows) {
    LogDebug("Load layout: no windows in the file");
    return;
  }

  // Saved entries are handed to the windows that exist, in window-number
  // order (the order both were opened in), and the rest are opened as
  // copies of the signed-in tab - the same window "+ Window" hands out, so
  // the session carries over - and then navigated to the saved page.
  // Navigating the copy keeps its session storage; opening the URL in a
  // fresh window would be a new sign-in.
  std::vector<BrowserWindowInterface*> existing;
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
        browser->GetWindow() && browser->GetTabStripModel()) {
      existing.push_back(browser);
    }
  }
  std::sort(existing.begin(), existing.end(),
            [](BrowserWindowInterface* a, BrowserWindowInterface* b) {
              return WindowNumberForSession(a->GetSessionID()) <
                     WindowNumberForSession(b->GetSessionID());
            });

  auto* controller = content::MouseMuxInputController::GetInstance();
  size_t used = 0;
  int applied = 0;
  int assigned = 0;
  for (const base::Value& value : *windows) {
    const base::DictValue* entry = value.GetIfDict();
    if (!entry) {
      continue;
    }
    BrowserWindowInterface* target =
        used < existing.size() ? existing[used++] : OpenWindowCopy();
    if (!target || !target->GetWindow() || !target->GetTabStripModel()) {
      LogDebug("Load layout: no window for an entry");
      continue;
    }
    ++applied;

    const std::optional<int> x = entry->FindInt("x");
    const std::optional<int> y = entry->FindInt("y");
    const std::optional<int> width = entry->FindInt("width");
    const std::optional<int> height = entry->FindInt("height");
    if (x && y && width && height && *width > 0 && *height > 0) {
      target->GetWindow()->SetBounds(gfx::Rect(*x, *y, *width, *height));
    }
    if (entry->FindBool("maximized").value_or(false)) {
      target->GetWindow()->Maximize();
    }

    if (const std::string* url = entry->FindString("url")) {
      content::WebContents* active =
          target->GetTabStripModel()->GetActiveWebContents();
      const GURL gurl(*url);
      if (active && gurl.is_valid() && active->GetLastCommittedURL() != gurl) {
        active->GetController().LoadURL(gurl, content::Referrer(),
                                        ui::PAGE_TRANSITION_TYPED,
                                        std::string());
      }
    }

    if (const std::string* owner = entry->FindString("owner")) {
      if (controller->AssignWindow(*owner, WindowOf(target),
                                   entry->FindBool("captured").value_or(false),
                                   entry->FindBool("block_native").value_or(true))) {
        ++assigned;
      }
    }
  }
  LogDebug(base::StringPrintf("Load layout: %d windows applied, %d assigned",
                              applied, assigned));
  ScheduleRebuild();
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
  if (hwid != -1) {
    LogDebug(base::StringPrintf("Ownership changed: hwid=0x%x name=%s",
                                 hwid, name.empty() ? "(unknown)" : name.c_str()));
  } else {
    LogDebug("Ownership released - waiting for first click");
  }

  // The UI work is posted, not done here.  This is raised from inside
  // injected input (a press claiming a window) and from inside a page's
  // destructor (a closing tab releasing its user), and it walks every
  // browser window updating captions and toolbars.  One claim raises it
  // twice (AddOwner, then AdoptWindow); a pending flag folds those into one
  // refresh.
  if (ownership_refresh_pending_) {
    return;
  }
  ownership_refresh_pending_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MouseMuxControlDialog::RefreshOwnershipUi,
                                weak_factory_.GetWeakPtr()));
}

void MouseMuxControlDialog::RefreshOwnershipUi() {
  ownership_refresh_pending_ = false;

  // Window captions and toolbar chips carry the owner's name; every one may
  // have changed.
  const std::vector<content::MouseMuxInputController::OwnerInfo> owners =
      content::MouseMuxInputController::GetInstance()->GetOwners();
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    Browser* b = browser->GetBrowserForMigrationOnly();
    if (!b || !b->window()) {
      continue;
    }
    b->window()->UpdateTitleBar();
    BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(b);
    if (!browser_view || !browser_view->toolbar()) {
      continue;
    }
    std::u16string chip_name;
    SkColor chip_color = SK_ColorBLACK;
    if (aura::Window* native = browser_view->GetNativeWindow();
        native && native->GetHost()) {
      const gfx::AcceleratedWidget window =
          native->GetHost()->GetAcceleratedWidget();
      const int owner_hwid =
          content::MouseMuxInputController::GetInstance()->OwnerOfWindow(
              window);
      for (const auto& owner : owners) {
        if (owner.hwid == owner_hwid) {
          chip_name = base::UTF8ToUTF16(owner.name.empty() ? "(unnamed)"
                                                            : owner.name);
          chip_color = row_text::DotColor(owner);
          break;
        }
      }
    }
    browser_view->toolbar()->SetMouseMuxOwner(chip_name, chip_color);
  }

  UpdateStatusLine();
  // Already on a fresh task; the rebuild can run inline.
  RebuildOwnerList();
}

BEGIN_METADATA(MouseMuxControlDialog)
END_METADATA

}  // namespace mouse_mux
