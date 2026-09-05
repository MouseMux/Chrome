// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/widget/desktop_aura/desktop_window_tree_host_win.h"

#include <dwmapi.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <set>

#include "base/auto_reset.h"
#include "base/debug/stack_trace.h"
#include "base/check_op.h"
#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/debug/dump_without_crashing.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/trace_event/trace_event.h"
#include "base/win/win_util.h"
#include "base/win/windows_version.h"
#include "skia/ext/skia_utils_win.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/client/cursor_client.h"
#include "ui/aura/client/focus_client.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/base/class_property.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/platform_cursor.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/views/controls/menu/menu_controller.h"
#include "ui/views/window/non_client_view.h"
#include "ui/base/mojom/menu_source_type.mojom-shared.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/base/win/event_creation_utils.h"
#include "ui/base/win/hwnd_metrics.h"
#include "ui/base/win/win_cursor.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider_key.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/paint_context.h"
#include "ui/display/win/dpi.h"
#include "ui/display/win/screen_win.h"
#include "ui/events/keyboard_hook.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/keyboard_code_conversion_win.h"
#include "ui/events/keycodes/dom/dom_keyboard_layout_map.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/gfx/path_win.h"
#include "ui/views/corewm/tooltip_aura.h"
#include "ui/views/views_features.h"
#include "ui/views/views_switches.h"
#include "ui/views/widget/desktop_aura/desktop_drag_drop_client_win.h"
#include "ui/views/widget/desktop_aura/desktop_native_cursor_manager.h"
#include "ui/views/widget/desktop_aura/desktop_native_cursor_manager_win.h"
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/root_view.h"
#include "ui/views/widget/widget_activation_delegate.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_hwnd_utils.h"
#include "ui/views/win/fullscreen_handler.h"
#include "ui/views/win/hwnd_message_handler.h"
#include "ui/views/win/hwnd_util.h"
#include "ui/views/window/native_frame_view.h"
#include "ui/wm/core/compound_event_filter.h"
#include "ui/wm/core/window_animations.h"
#include "ui/wm/public/activation_client.h"
#include "ui/wm/public/scoped_tooltip_disabler.h"

#include "ui/events/event_utils.h"
#include "ui/gfx/win/msg_util.h"

// MouseMux defines — duplicated from mouse_mux_config.h to avoid a
// content/ → ui/views layering violation.  Keep in sync with config.h.
//
// EVERY MouseMux define used in this file must appear below, including the
// disabled ones.  A define that is #ifdef'd here but missing from this list is
// silently dead — the guard never fires, and enabling it in config.h does
// nothing at all, with no warning.  MOUSEMUX_EXPERIMENT_NATIVE_BLOCK_HARD was
// exactly that from v13 to v15.
#define MOUSEMUX_NATIVE_BLOCK
// #define MOUSEMUX_EXPERIMENT_NATIVE_BLOCK_HARD
// #define MOUSEMUX_EXPERIMENT_NC_HANDLING
// #define MOUSEMUX_EXPERIMENT_PEN_TOUCH_BLOCK
#define MOUSEMUX_DEBUG_TRACE  // ON for the 2026-09-03 field debug build

#ifdef MOUSEMUX_DEBUG_TRACE
// Duplicated from mouse_mux_config.h for the same reason as the defines:
// ui/views cannot include content/.  Keep in sync.
#include <stdarg.h>
#include <stdio.h>

// Scoped to this debug-only helper: Chromium 151 rejects fprintf and
// va_list, and the alternative is exempting this whole file from the
// unsafe-buffer checks permanently. Mirrors the same block in
// mouse_mux_config.h.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
static void MouseMuxTraceViews(const char* stage, const char* fmt, ...) {
  // %TEMP%, never a drive letter: this build is sent to other machines, and a
  // hardcoded path that does not exist there fails silently.  Same file as
  // the content-side trace, so the two interleave.
  char path[MAX_PATH];
  const DWORD n = GetTempPathA(MAX_PATH, path);
  if (n == 0 || n >= MAX_PATH) {
    return;
  }
  strncat_s(path, sizeof(path), "chrome-log.txt", _TRUNCATE);
  FILE* f = fopen(path, "a");
  if (!f) {
    return;
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(f, "[%02d:%02d:%02d.%03d|p%lu] %-22s ", st.wHour, st.wMinute,
          st.wSecond, st.wMilliseconds, GetCurrentProcessId(), stage);
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fputc('\n', f);
  fclose(f);
}
#pragma clang diagnostic pop

#define MMTRACE_VIEWS(stage, ...) MouseMuxTraceViews(stage, __VA_ARGS__)
#else
#define MMTRACE_VIEWS(stage, ...) ((void)0)
#endif
#define WM_MOUSEMUX_LBUTTONDOWN  (WM_APP + 0x100)
#define WM_MOUSEMUX_LBUTTONUP    (WM_APP + 0x101)
#define WM_MOUSEMUX_RBUTTONDOWN  (WM_APP + 0x102)
#define WM_MOUSEMUX_RBUTTONUP    (WM_APP + 0x103)
#define WM_MOUSEMUX_MBUTTONDOWN  (WM_APP + 0x104)
#define WM_MOUSEMUX_MBUTTONUP    (WM_APP + 0x105)
// Hover for Chrome UI and menus: moves over anything that is not page
// content, and a leave when the pointer goes back to the page or to another
// window, so a highlight does not stick.
#define WM_MOUSEMUX_MOUSEMOVE    (WM_APP + 0x106)
#define WM_MOUSEMUX_MOUSELEAVE   (WM_APP + 0x107)
// Keyboard, on the same principle.  wParam: low word the virtual key
// (KEYDOWN/KEYUP) or the UTF-16 unit (CHAR); high word the ui::EventFlags
// modifiers of the device that typed it, carried in the message because
// GetKeyState() knows one keyboard and there are several.  lParam: low 32
// bits the ui::DomCode, bits 32-47 the virtual key (CHAR only).  lParam is
// 64 bits wide on x64, which is the only place this runs.
#define WM_MOUSEMUX_KEYDOWN      (WM_APP + 0x110)
#define WM_MOUSEMUX_KEYUP        (WM_APP + 0x111)
#define WM_MOUSEMUX_CHAR         (WM_APP + 0x112)

// True while PreHandleMSG is handling one of the keyboard messages above,
// and while the controller gives a page Chrome's focus for an injected
// click.  Restoring a window's focused view makes the focus manager clear
// native focus, which is a Win32 SetFocus on the HWND, and SetFocus
// activates an inactive top-level window.  With two users typing, that is
// the OS foreground flipping between their windows on every burst of keys.
// While this is set, ClearNativeFocus() and Activate() do nothing and
// IsActive() says yes: under capture there is no hardware keyboard whose
// focus would need moving.  In namespace content like the other shared
// flags, so the controller can set it.
namespace content {
bool g_mousemux_synthetic_key = false;
}  // namespace content

#ifdef MOUSEMUX_NATIVE_BLOCK
// Global flag set by MouseMuxInputController::SetNativeInputBlocked().
// When true, PreHandleMSG drops native mouse button messages so that
// only SDK custom messages (WM_MOUSEMUX_*) reach the views UI layer.
// Lives in namespace content to match the extern in the controller.
namespace content {
bool g_mousemux_native_input_blocked = false;
// The windows that drop native input on their own: every window of every
// owner who has "Block native" on (the default).  Maintained by
// MouseMuxInputController::ApplyNativeBlocking().  The bool above is the
// legacy everything-at-once flag the control server can still set.
std::set<HWND>* g_mousemux_blocked_windows = nullptr;
// True while PreHandleMSG is dispatching one of our custom mouse messages,
// so the controller's forward log can tell "injected via Chrome UI" from
// "native": both reach the page without the kFromDebugger modifier.
bool g_mousemux_in_custom_dispatch = false;
// Windows whose caption (non-client) button is currently pressed by the real
// mouse.  Non-client presses pass the block - the operator may close,
// minimize or resize an owned window - but the RELEASE that ends the press
// arrives as a client-area button-up, which the block dropped: the caption
// button stayed pressed, Chrome's frame kept the mouse, and the window
// looked dead until an activation change reset it (2026-09-04 21:22).  The
// release passes while this is set.
std::set<HWND>* g_mousemux_caption_press = nullptr;
HWND g_mousemux_dialog_hwnd = nullptr;  // Exempt from native blocking.
// The help window, likewise: it is one of ours, the operator drives it with a
// real mouse, and a help window nobody can click is worse than no help window.
HWND g_mousemux_help_hwnd = nullptr;
// True while any injected mouse holds a button, maintained by
// MouseMuxInputController::NoteSdkButtonState().  Read where a drag and drop
// would start: a drag that begins while this is set was begun by an injected
// mouse and must not become an OLE drag, because DoDragDrop is a modal Win32
// loop that ends only on a real button release, which an injected mouse never
// produces - the browser process hangs in that loop.
bool g_mousemux_sdk_button_held = false;

// Whether native input to `self` is blocked: the window is on the list, or
// the window that OWNS it is.  Menus, bubbles and other popups are windows
// of their own, owned by the browser window they serve, and Windows re-posts
// the idle real mouse's position to whichever of them holds capture - the
// menu flicker of 2026-09-04, which the per-owner list let back in because
// it named only the browser windows (2026-09-05: 934 native moves under
// menus in one session, every popup "in-open-window").
bool MouseMuxWindowBlocked(HWND self) {
  if (g_mousemux_native_input_blocked) {
    return true;
  }
  if (!g_mousemux_blocked_windows || !self) {
    return false;
  }
  if (g_mousemux_blocked_windows->count(self) > 0) {
    return true;
  }
  const HWND owner = ::GetAncestor(self, GA_ROOTOWNER);
  return owner && owner != self && g_mousemux_blocked_windows->count(owner) > 0;
}
}  // namespace content
#endif

DEFINE_UI_CLASS_PROPERTY_TYPE(views::DesktopWindowTreeHostWin*)

namespace views {

namespace {

// While the mouse is locked we want the invisible mouse to stay within the
// confines of the screen so we keep it in a capture region the size of the
// screen.  However, on windows when the mouse hits the edge of the screen some
// events trigger and cause strange issues to occur. To stop those events from
// occurring we add a small border around the edge of the capture region.
// This constant controls how many pixels wide that border is.
const int kMouseCaptureRegionBorder = 5;

// Updates the cursor clip region. Used for mouse locking.
void UpdateMouseLockRegion(aura::Window* window, bool locked) {
  if (!locked) {
    ::ClipCursor(nullptr);
    return;
  }

  RECT window_rect =
      display::Screen::Get()
          ->DIPToScreenRectInWindow(window, window->GetBoundsInScreen())
          .ToRECT();
  window_rect.left += kMouseCaptureRegionBorder;
  window_rect.right -= kMouseCaptureRegionBorder;
  window_rect.top += kMouseCaptureRegionBorder;
  window_rect.bottom -= kMouseCaptureRegionBorder;
  ::ClipCursor(&window_rect);
}

}  // namespace

DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(base::WeakPtr<aura::Window>,
                                   kContentWindowForRootWindow)

// Identifies the DesktopWindowTreeHostWin associated with the
// WindowEventDispatcher.
DEFINE_UI_CLASS_PROPERTY_KEY(DesktopWindowTreeHostWin*,
                             kDesktopWindowTreeHostKey,
                             NULL)

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWin, public:

bool DesktopWindowTreeHostWin::is_cursor_visible_ = true;

DesktopWindowTreeHostWin::DesktopWindowTreeHostWin(
    internal::NativeWidgetDelegate* native_widget_delegate,
    DesktopNativeWidgetAura* desktop_native_widget_aura)
    : native_widget_delegate_(native_widget_delegate->AsWidget()->GetWeakPtr()),
      desktop_native_widget_aura_(desktop_native_widget_aura),
      drag_drop_client_(nullptr),
      should_animate_window_close_(false),
      pending_close_(false),
      has_non_client_view_(false) {}

DesktopWindowTreeHostWin::~DesktopWindowTreeHostWin() {
  ClearBackgroundPaintBrush();
  desktop_native_widget_aura_->OnDesktopWindowTreeHostDestroyed(this);
  // Normally HandleDestroying() destroys the compositor (which is called
  // from WM_DESTROY) but it appears in some situations we can get
  // WM_NCDESTROY (which calls this function) without a WM_DESTROY. As a result
  // DestroyCompositor() is called from both places.
  DestroyCompositor();
  DestroyDispatcher();

  if (HWNDMessageHandler* raw_handler = message_handler_.release()) {
    raw_handler->DestroyHandler();
  }
}

// static
aura::Window* DesktopWindowTreeHostWin::GetContentWindowForHWND(HWND hwnd) {
  // All HWND's we create should have WindowTreeHost instances associated with
  // them. There are exceptions like the content layer creating HWND's which
  // are not associated with WindowTreeHost instances.
  aura::WindowTreeHost* host =
      aura::WindowTreeHost::GetForAcceleratedWidget(hwnd);
  if (!host) {
    return nullptr;
  }
  base::WeakPtr<aura::Window>* weak_ptr =
      host->window()->GetProperty(kContentWindowForRootWindow);
  return weak_ptr ? weak_ptr->get() : nullptr;
}

void DesktopWindowTreeHostWin::StartTouchDrag(gfx::Point screen_point) {
  // Send a mouse down and mouse move before do drag drop runs its own event
  // loop. This is required for ::DoDragDrop to start the drag.
  ui::SendMouseEvent(screen_point,
                     (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_VIRTUALDESK));
  ui::SendMouseEvent(screen_point,
                     (MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK));
  in_touch_drag_ = true;
}

void DesktopWindowTreeHostWin::FinishTouchDrag(gfx::Point screen_point) {
  if (in_touch_drag_) {
    in_touch_drag_ = false;
    ui::SendMouseEvent(screen_point,
                       (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_VIRTUALDESK));
  }
}

bool DesktopWindowTreeHostWin::IsInNativeMoveResizeLoop() const {
  return message_handler_ && (message_handler_->IsInNativeMoveResizeLoop() ||
                              message_handler_->IsInNativeMenuLoop());
}

// DesktopWindowTreeHostWin, DesktopWindowTreeHost implementation:

void DesktopWindowTreeHostWin::Init(const Widget::InitParams& params) {
  wm::SetAnimationHost(content_window(), this);
  if (params.type == Widget::InitParams::TYPE_WINDOW &&
      !params.remove_standard_frame) {
    content_window()->SetProperty(aura::client::kAnimationsDisabledKey, true);
  }

  message_handler_ = HWNDMessageHandler::Create(
      this, native_widget_delegate_->AsWidget()->GetName());

  ConfigureWindowStyles(message_handler_.get(), params,
                        GetWidget()->widget_delegate(),
                        native_widget_delegate_.get());

  HWND parent_hwnd = nullptr;
  if (params.parent && params.parent->GetHost()) {
    parent_hwnd = params.parent->GetHost()->GetAcceleratedWidget();
  }

  remove_standard_frame_ = params.remove_standard_frame;
  has_non_client_view_ = Widget::RequiresNonClientView(params.type);
  z_order_ = params.EffectiveZOrderLevel();

  // We don't have an HWND yet, so scale relative to the nearest screen.
  gfx::Rect pixel_bounds =
      display::win::GetScreenWin()->DIPToScreenRect(nullptr, params.bounds);
  message_handler_->Init(parent_hwnd, pixel_bounds);

  if (ShouldAddDWMBackdrop()) {
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    HRESULT hr = DwmSetWindowAttribute(GetHWND(), DWMWA_SYSTEMBACKDROP_TYPE,
                                       &backdrop, sizeof(backdrop));
    if (FAILED(hr)) {
      // If DwmSetWindowAttribute fails, it indicates that there was a problem
      // setting the system backdrop type. In this state, the backdrop is not
      // applied, and the worst that can happen is a transparent window appears
      // while the GPU process is being started.
      LOG(ERROR) << "Failed to set DWM system backdrop type: "
                 << logging::SystemErrorCodeToString(
                        static_cast<logging::SystemErrorCode>(hr));
    }
  }

  UpdateBackdropColorMode();

  CreateCompositor(params.force_software_compositing);
  OnAcceleratedWidgetAvailable();
  InitHost();
  window()->Show();

  // Stack immediately above its parent so that it does not cover other
  // root-level windows, with the exception of menus, to allow them to be
  // displayed on top of other windows.
  if (params.parent && params.type != views::Widget::InitParams::TYPE_MENU) {
    StackAbove(params.parent);
  }
}

void DesktopWindowTreeHostWin::OnNativeWidgetCreated(
    const Widget::InitParams& params) {
  // The cursor is not necessarily visible when the root window is created.
  aura::client::CursorClient* cursor_client =
      aura::client::GetCursorClient(window());
  if (cursor_client) {
    is_cursor_visible_ = cursor_client->IsCursorVisible();
  }

  window()->SetProperty(kContentWindowForRootWindow,
                        content_window()->GetWeakPtrAsWindow());
  window()->SetProperty(kDesktopWindowTreeHostKey, this);

  should_animate_window_close_ =
      content_window()->GetType() != aura::client::WINDOW_TYPE_NORMAL &&
      !wm::WindowAnimationsDisabled(content_window());
}

void DesktopWindowTreeHostWin::OnActiveWindowChanged(bool active) {}

void DesktopWindowTreeHostWin::OnWidgetInitDone() {}

void DesktopWindowTreeHostWin::SetBackgroundColor(SkColor background_color) {
  UpdateBackdropColorMode();
  ClearBackgroundPaintBrush();
  background_paint_brush_ =
      CreateSolidBrush(skia::SkColorToCOLORREF(background_color));
}

std::unique_ptr<corewm::Tooltip> DesktopWindowTreeHostWin::CreateTooltip() {
  return std::make_unique<corewm::TooltipAura>();
}

std::unique_ptr<aura::client::DragDropClient>
DesktopWindowTreeHostWin::CreateDragDropClient() {
  auto res =
      std::make_unique<DesktopDragDropClientWin>(window(), GetHWND(), this);
  drag_drop_client_ = res->GetWeakPtr();
  return std::move(res);
}

void DesktopWindowTreeHostWin::Close() {
  // Do not generate synthesized events during shutdown.
  dispatcher()->Shutdown();

  // Calling Hide() can detach the content window's layer, so store it
  // beforehand so we can access it below.
  auto* window_layer = content_window()->layer();

  content_window()->Hide();
  // TODO(beng): Move this entire branch to DNWA so it can be shared with X11.
  if (should_animate_window_close_) {
    pending_close_ = true;
    // Animation may not start for a number of reasons.
    if (!window_layer->GetAnimator()->is_animating()) {
      message_handler_->Close();
    }
    // else case, OnWindowHidingAnimationCompleted does the actual Close.
  } else {
    message_handler_->Close();
  }
}

void DesktopWindowTreeHostWin::CloseNow() {
  message_handler_->CloseNow();
}

aura::WindowTreeHost* DesktopWindowTreeHostWin::AsWindowTreeHost() {
  return this;
}

DesktopWindowTreeHost::WindowTreeHosts
DesktopWindowTreeHostWin::GetOwnedWindowTreeHosts() {
  WindowTreeHosts window_tree_hosts;
  std::vector<HWND> owned_hwns = message_handler_->GetOwnedWindows();
  for (HWND hwnd : owned_hwns) {
    if (aura::WindowTreeHost* host =
            aura::WindowTreeHost::GetForAcceleratedWidget(hwnd)) {
      window_tree_hosts.insert(host);
    }
  }
  return window_tree_hosts;
}

void DesktopWindowTreeHostWin::Show(ui::mojom::WindowShowState show_state,
                                    const gfx::Rect& restore_bounds) {
  OnAcceleratedWidgetMadeVisible(true);

  gfx::Rect pixel_restore_bounds;
  if (show_state == ui::mojom::WindowShowState::kMaximized) {
    // The window parameter is intentionally passed as nullptr because a
    // non-null window parameter causes errors when restoring windows to saved
    // positions in variable-DPI situations. See https://crbug.com/1252564 for
    // details.
    pixel_restore_bounds =
        display::win::GetScreenWin()->DIPToScreenRect(nullptr, restore_bounds);
  }

  // Show content window first so that Widget::IsVisible() returns true during
  // the synchronous HandleVisibilityChanged(true) triggered by ShowWindow().
  content_window()->Show();
  message_handler_->Show(show_state, pixel_restore_bounds);

  if (WidgetActivationDelegate::Get()) {
    WidgetActivationDelegate::Get()->MaybeActivate(
        GetWidget(), GetWidget()->CanActivate() &&
                         show_state != ui::mojom::WindowShowState::kInactive &&
                         show_state != ui::mojom::WindowShowState::kMinimized);
  }
}

bool DesktopWindowTreeHostWin::IsVisible() const {
  return message_handler_->IsVisible();
}

void DesktopWindowTreeHostWin::SetSize(const gfx::Size& size) {
  const gfx::Size size_in_pixels =
      display::win::GetScreenWin()->DIPToScreenSize(GetHWND(), size);
  message_handler_->SetSize(size_in_pixels);
}

void DesktopWindowTreeHostWin::StackAbove(aura::Window* window) {
  HWND hwnd = HWNDForNativeView(window);
  if (hwnd) {
    message_handler_->StackAbove(hwnd);
  }
}

void DesktopWindowTreeHostWin::StackAtTop() {
  message_handler_->StackAtTop();
}

void DesktopWindowTreeHostWin::CenterWindow(const gfx::Size& size) {
  const gfx::Size size_in_pixels =
      display::win::GetScreenWin()->DIPToScreenSize(GetHWND(), size);
  message_handler_->CenterWindow(size_in_pixels);
}

void DesktopWindowTreeHostWin::GetWindowPlacement(
    gfx::Rect* bounds,
    ui::mojom::WindowShowState* show_state) const {
  message_handler_->GetWindowPlacement(bounds, show_state);
  *bounds = display::win::GetScreenWin()->ScreenToDIPRect(GetHWND(), *bounds);
}

gfx::Rect DesktopWindowTreeHostWin::GetWindowBoundsInScreen() const {
  gfx::Rect pixel_bounds = message_handler_->GetWindowBoundsInScreen();
  return display::win::GetScreenWin()->ScreenToDIPRect(GetHWND(), pixel_bounds);
}

gfx::Rect DesktopWindowTreeHostWin::GetClientAreaBoundsInScreen() const {
  gfx::Rect pixel_bounds = message_handler_->GetClientAreaBoundsInScreen();
  return display::win::GetScreenWin()->ScreenToDIPRect(GetHWND(), pixel_bounds);
}

gfx::Rect DesktopWindowTreeHostWin::GetRestoredBounds() const {
  gfx::Rect pixel_bounds = message_handler_->GetRestoredBounds();
  return display::win::GetScreenWin()->ScreenToDIPRect(GetHWND(), pixel_bounds);
}

std::string DesktopWindowTreeHostWin::GetWorkspace() const {
  return std::string();
}

gfx::Rect DesktopWindowTreeHostWin::GetWorkAreaBoundsInScreen() const {
  MONITORINFO monitor_info;
  monitor_info.cbSize = sizeof(monitor_info);
  GetMonitorInfo(
      MonitorFromWindow(message_handler_->hwnd(), MONITOR_DEFAULTTONEAREST),
      &monitor_info);
  gfx::Rect pixel_bounds = gfx::Rect(monitor_info.rcWork);
  return display::win::GetScreenWin()->ScreenToDIPRect(GetHWND(), pixel_bounds);
}

void DesktopWindowTreeHostWin::SetShape(
    std::unique_ptr<Widget::ShapeRects> native_shape) {
  if (!native_shape || native_shape->empty()) {
    message_handler_->SetRegion(nullptr);
    return;
  }

  // TODO(wez): This would be a lot simpler if we were passed an SkPath.
  // See crbug.com/410593.
  SkRegion shape;
  const float scale =
      display::win::GetScreenWin()->GetScaleFactorForHWND(GetHWND());
  if (scale > 1.0) {
    std::vector<SkIRect> sk_rects;
    for (const gfx::Rect& rect : *native_shape) {
      const SkIRect sk_rect = gfx::RectToSkIRect(rect);
      SkRect scaled_rect =
          SkRect::MakeLTRB(sk_rect.left() * scale, sk_rect.top() * scale,
                           sk_rect.right() * scale, sk_rect.bottom() * scale);
      SkIRect rounded_scaled_rect;
      scaled_rect.roundOut(&rounded_scaled_rect);
      sk_rects.push_back(rounded_scaled_rect);
    }
    shape.setRects(&sk_rects[0], static_cast<int>(sk_rects.size()));
  } else {
    for (const gfx::Rect& rect : *native_shape) {
      shape.op(gfx::RectToSkIRect(rect), SkRegion::kUnion_Op);
    }
  }

  message_handler_->SetRegion(gfx::CreateHRGNFromSkRegion(shape));
}

void DesktopWindowTreeHostWin::SetParent(gfx::AcceleratedWidget parent) {
  message_handler_->SetParentOrOwner(parent);
}

void DesktopWindowTreeHostWin::Activate() {
  if (content::g_mousemux_synthetic_key) {
    // The focus manager activates the widget before it will focus a view in
    // an OS-inactive window.  During a synthetic keystroke that would move
    // the OS foreground to whichever user typed last; IsActive() below tells
    // the focus manager the window is already active, and this makes sure
    // nothing activates it for real.
    return;
  }
  if (WidgetActivationDelegate::Get()) {
    WidgetActivationDelegate::Get()->MaybeActivate(GetWidget(),
                                                   /*activate=*/true);
  }
  message_handler_->Activate();
}

void DesktopWindowTreeHostWin::Deactivate() {
  if (WidgetActivationDelegate::Get()) {
    WidgetActivationDelegate::Get()->Deactivate(GetWidget());
  } else {
    message_handler_->Deactivate();
  }
}

bool DesktopWindowTreeHostWin::IsActive() const {
  if (content::g_mousemux_synthetic_key) {
    return true;  // See Activate().
  }
  if (WidgetActivationDelegate::Get()) {
    return WidgetActivationDelegate::Get()->IsActive(GetWidget());
  }
  return message_handler_->IsActive();
}

void DesktopWindowTreeHostWin::PaintAsActiveChanged() {
  message_handler_->PaintAsActiveChanged();
}

void DesktopWindowTreeHostWin::Maximize() {
  message_handler_->Maximize();
}

void DesktopWindowTreeHostWin::Minimize() {
  message_handler_->Minimize();
}

void DesktopWindowTreeHostWin::Restore() {
  message_handler_->Restore();
}

bool DesktopWindowTreeHostWin::IsMaximized() const {
  return message_handler_->IsMaximized();
}

bool DesktopWindowTreeHostWin::IsMinimized() const {
  return message_handler_->IsMinimized();
}

bool DesktopWindowTreeHostWin::HasCapture() const {
  return message_handler_->HasCapture();
}

void DesktopWindowTreeHostWin::SetZOrderLevel(ui::ZOrderLevel order) {
  z_order_ = order;
  // Emulate the multiple window levels provided by other platforms by
  // collapsing the z-order enum into kNormal = normal, everything else = always
  // on top.
  message_handler_->SetAlwaysOnTop(order != ui::ZOrderLevel::kNormal);
}

ui::ZOrderLevel DesktopWindowTreeHostWin::GetZOrderLevel() const {
  bool window_always_on_top = message_handler_->IsAlwaysOnTop();
  bool level_always_on_top = z_order_ != ui::ZOrderLevel::kNormal;

  if (window_always_on_top == level_always_on_top) {
    return z_order_;
  }

  // If something external has forced a window to be always-on-top, map it to
  // kFloatingWindow as a reasonable equivalent.
  return window_always_on_top ? ui::ZOrderLevel::kFloatingWindow
                              : ui::ZOrderLevel::kNormal;
}

bool DesktopWindowTreeHostWin::IsStackedAbove(aura::Window* window) {
  HWND above = GetHWND();
  HWND below = window->GetHost()->GetAcceleratedWidget();

  // Child windows are always above their parent windows.
  // Check to see if HWNDs have a Parent-Child relationship.
  if (IsChild(below, above)) {
    return true;
  }

  if (IsChild(above, below)) {
    return false;
  }

  // Check all HWNDs with lower z order than current HWND
  // to see if it matches or is a parent to the "below" HWND.
  bool result = false;
  HWND parent = above;
  while (parent && parent != GetDesktopWindow()) {
    HWND next = parent;
    while (next) {
      // GW_HWNDNEXT retrieves the next HWND below z order level.
      next = GetWindow(next, GW_HWNDNEXT);
      if (next == below || IsChild(next, below)) {
        result = true;
        break;
      }
    }
    parent = GetAncestor(parent, GA_PARENT);
  }

  return result;
}

void DesktopWindowTreeHostWin::SetVisibleOnAllWorkspaces(bool always_visible) {
  // Chrome does not yet support Windows 10 desktops.
}

bool DesktopWindowTreeHostWin::IsVisibleOnAllWorkspaces() const {
  return false;
}

bool DesktopWindowTreeHostWin::SetWindowTitle(const std::u16string& title) {
  return message_handler_->SetTitle(title);
}

void DesktopWindowTreeHostWin::ClearNativeFocus() {
  if (content::g_mousemux_synthetic_key) {
    return;  // See content::g_mousemux_synthetic_key.
  }
  message_handler_->ClearNativeFocus();
}

Widget::MoveLoopResult DesktopWindowTreeHostWin::RunMoveLoop(
    const gfx::Vector2d& drag_offset,
    Widget::MoveLoopSource source,
    Widget::MoveLoopEscapeBehavior escape_behavior) {
  const bool hide_on_escape =
      escape_behavior == Widget::MoveLoopEscapeBehavior::kHide;
  return message_handler_->RunMoveLoop(drag_offset, hide_on_escape)
             ? Widget::MoveLoopResult::kSuccessful
             : Widget::MoveLoopResult::kCanceled;
}

void DesktopWindowTreeHostWin::EndMoveLoop() {
  message_handler_->EndMoveLoop();
}

void DesktopWindowTreeHostWin::SetVisibilityChangedAnimationsEnabled(
    bool value) {
  message_handler_->SetVisibilityChangedAnimationsEnabled(value);
  if (desktop_native_widget_aura_->widget_type() !=
          Widget::InitParams::TYPE_WINDOW ||
      remove_standard_frame_) {
    content_window()->SetProperty(aura::client::kAnimationsDisabledKey, !value);
  }
}

std::unique_ptr<FrameView> DesktopWindowTreeHostWin::CreateFrameView() {
  return (ShouldUseNativeFrame() && native_widget_delegate_)
             ? std::make_unique<NativeFrameView>(
                   native_widget_delegate_->AsWidget())
             : nullptr;
}

bool DesktopWindowTreeHostWin::ShouldUseNativeFrame() const {
  return true;
}

bool DesktopWindowTreeHostWin::ShouldWindowContentsBeTransparent() const {
  // The window contents need to be transparent when the titlebar area is drawn
  // by the DWM rather than Chrome, so that area can show through.  This
  // function does not describe the transparency of the whole window appearance,
  // but merely of the content Chrome draws, so even when the system titlebars
  // appear opaque (Win 8+), the content above them needs to be transparent, or
  // they'll be covered by a black (undrawn) region.
  return ShouldUseNativeFrame() && !IsFullscreen();
}

void DesktopWindowTreeHostWin::FrameTypeChanged() {
  message_handler_->FrameTypeChanged();
}

void DesktopWindowTreeHostWin::SetFullscreen(bool fullscreen,
                                             int64_t target_display_id) {
  auto weak_ptr = GetWeakPtr();
  message_handler_->SetFullscreen(fullscreen, target_display_id);
  if (!weak_ptr) {
    return;
  }
  // TODO(sky): workaround for ScopedFullscreenVisibility showing window
  // directly. Instead of this should listen for visibility changes and then
  // update window.
  if (message_handler_->IsVisible() && !content_window()->TargetVisibility()) {
    OnAcceleratedWidgetMadeVisible(true);
    content_window()->Show();
  }
  desktop_native_widget_aura_->UpdateWindowTransparency();
}

bool DesktopWindowTreeHostWin::IsFullscreen() const {
  return message_handler_->IsFullscreen();
}

void DesktopWindowTreeHostWin::SetOpacity(float opacity) {
  content_window()->layer()->SetOpacity(opacity);
}

void DesktopWindowTreeHostWin::SetAspectRatio(
    const gfx::SizeF& aspect_ratio,
    const gfx::Size& excluded_margin) {
  DCHECK(!aspect_ratio.IsEmpty());
  message_handler_->SetAspectRatio(aspect_ratio.width() / aspect_ratio.height(),
                                   excluded_margin);
}

void DesktopWindowTreeHostWin::SetWindowIcons(const gfx::ImageSkia& window_icon,
                                              const gfx::ImageSkia& app_icon) {
  message_handler_->SetWindowIcons(window_icon, app_icon);
}

void DesktopWindowTreeHostWin::InitModalType(ui::mojom::ModalType modal_type) {
  message_handler_->InitModalType(modal_type);
}

void DesktopWindowTreeHostWin::FlashFrame(bool flash_frame) {
  message_handler_->FlashFrame(flash_frame);
}

bool DesktopWindowTreeHostWin::IsAnimatingClosed() const {
  return pending_close_;
}

void DesktopWindowTreeHostWin::SizeConstraintsChanged() {
  message_handler_->SizeConstraintsChanged();
}

bool DesktopWindowTreeHostWin::ShouldUpdateWindowTransparency() const {
  return true;
}

bool DesktopWindowTreeHostWin::ShouldUseDesktopNativeCursorManager() const {
  return true;
}

bool DesktopWindowTreeHostWin::ShouldCreateVisibilityController() const {
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWin, WindowTreeHost implementation:

ui::EventSource* DesktopWindowTreeHostWin::GetEventSource() {
  return this;
}

gfx::AcceleratedWidget DesktopWindowTreeHostWin::GetAcceleratedWidget() {
  return message_handler_->hwnd();
}

void DesktopWindowTreeHostWin::ShowImpl() {
  Show(ui::mojom::WindowShowState::kNormal, gfx::Rect());
}

void DesktopWindowTreeHostWin::HideImpl() {
  if (!pending_close_) {
    message_handler_->Hide();
  }
}

// GetBoundsInPixels and SetBoundsInPixels work in pixel coordinates, whereas
// other get/set methods work in DIP.

gfx::Rect DesktopWindowTreeHostWin::GetBoundsInPixels() const {
  const gfx::Rect bounds_px(message_handler_->GetClientAreaBounds());
  // If the window bounds were expanded we need to return the original bounds
  // To achieve this we do the reverse of the expansion, i.e. add the
  // window_expansion_top_left_delta_ to the origin and subtract the
  // window_expansion_bottom_right_delta_ from the width and height.
  const gfx::Rect without_expansion_bounds_px(
      bounds_px.x() + window_expansion_top_left_delta_.x(),
      bounds_px.y() + window_expansion_top_left_delta_.y(),
      bounds_px.width() - window_expansion_bottom_right_delta_.x(),
      bounds_px.height() - window_expansion_bottom_right_delta_.y());
  return without_expansion_bounds_px;
}

void DesktopWindowTreeHostWin::SetBoundsInPixels(
    const gfx::Rect& bounds_in_pixels) {
  // If the window bounds have to be expanded we need to subtract the
  // window_expansion_top_left_delta_ from the origin and add the
  // window_expansion_bottom_right_delta_ to the width and height
  const gfx::Size old_content_size_px = GetBoundsInPixels().size();

  const gfx::Rect expanded_bounds_px(
      bounds_in_pixels.x() - window_expansion_top_left_delta_.x(),
      bounds_in_pixels.y() - window_expansion_top_left_delta_.y(),
      bounds_in_pixels.width() + window_expansion_bottom_right_delta_.x(),
      bounds_in_pixels.height() + window_expansion_bottom_right_delta_.y());

  // When `expanded_bounds_px` causes the window to be moved to a display with a
  // different DSF, HWNDMessageHandler::OnDpiChanged() will be called and the
  // window size will be scaled automatically.
  message_handler_->SetBounds(expanded_bounds_px,
                              old_content_size_px != bounds_in_pixels.size());
}

gfx::Rect
DesktopWindowTreeHostWin::GetBoundsInAcceleratedWidgetPixelCoordinates() {
  if (message_handler_->IsMinimized()) {
    return gfx::Rect();
  }
  const gfx::Rect client_bounds =
      message_handler_->GetClientAreaBoundsInScreen();
  const gfx::Rect window_bounds = message_handler_->GetWindowBoundsInScreen();
  if (window_bounds == client_bounds) {
    return gfx::Rect(window_bounds.size());
  }
  const gfx::Vector2d offset = client_bounds.origin() - window_bounds.origin();
  DCHECK_GE(offset.x(), 0);
  DCHECK_GE(offset.y(), 0);
  return gfx::Rect(gfx::Point() + offset, client_bounds.size());
}

gfx::Point DesktopWindowTreeHostWin::GetLocationOnScreenInPixels() const {
  return GetBoundsInPixels().origin();
}

void DesktopWindowTreeHostWin::SetCapture() {
  message_handler_->SetCapture();
}

void DesktopWindowTreeHostWin::ReleaseCapture() {
  message_handler_->ReleaseCapture();
}

bool DesktopWindowTreeHostWin::CaptureSystemKeyEventsImpl(
    std::optional<base::flat_set<ui::DomCode>> dom_codes) {
  // Only one KeyboardHook should be active at a time, otherwise there will be
  // problems with event routing (i.e. which Hook takes precedence) and
  // destruction ordering.
  DCHECK(!keyboard_hook_);
  keyboard_hook_ = ui::KeyboardHook::CreateModifierKeyboardHook(
      std::move(dom_codes), GetAcceleratedWidget(),
      base::BindRepeating(&DesktopWindowTreeHostWin::HandleKeyEvent,
                          base::Unretained(this)));

  return keyboard_hook_ != nullptr;
}

void DesktopWindowTreeHostWin::ReleaseSystemKeyEventCapture() {
  keyboard_hook_.reset();
}

bool DesktopWindowTreeHostWin::IsKeyLocked(ui::DomCode dom_code) {
  return keyboard_hook_ && keyboard_hook_->IsKeyLocked(dom_code);
}

base::flat_map<std::string, std::string>
DesktopWindowTreeHostWin::GetKeyboardLayoutMap() {
  return ui::GenerateDomKeyboardLayoutMap();
}

void DesktopWindowTreeHostWin::SetCursorNative(gfx::NativeCursor cursor) {
  TRACE_EVENT1("ui,input", "DesktopWindowTreeHostWin::SetCursorNative",
               "cursor", cursor.type());

  message_handler_->SetCursor(
      ui::WinCursor::FromPlatformCursor(cursor.platform()));
}

void DesktopWindowTreeHostWin::OnCursorVisibilityChangedNative(bool show) {
  if (is_cursor_visible_ == show) {
    return;
  }
  is_cursor_visible_ = show;
  ::ShowCursor(!!show);
}

void DesktopWindowTreeHostWin::MoveCursorToScreenLocationInPixels(
    const gfx::Point& location_in_pixels) {
  POINT cursor_location = location_in_pixels.ToPOINT();
  ::ClientToScreen(GetHWND(), &cursor_location);
  ::SetCursorPos(cursor_location.x, cursor_location.y);
}

std::unique_ptr<aura::ScopedEnableUnadjustedMouseEvents>
DesktopWindowTreeHostWin::RequestUnadjustedMovement() {
  return message_handler_->RegisterUnadjustedMouseEvent();
}

void DesktopWindowTreeHostWin::LockMouse(aura::Window* window) {
  UpdateMouseLockRegion(window, true /*locked*/);
  message_handler_->set_mouse_locked(true);
  WindowTreeHost::LockMouse(window);
}

void DesktopWindowTreeHostWin::UnlockMouse(aura::Window* window) {
  UpdateMouseLockRegion(window, false /*locked*/);
  message_handler_->set_mouse_locked(false);
  WindowTreeHost::UnlockMouse(window);
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWin, wm::AnimationHost implementation:

void DesktopWindowTreeHostWin::SetHostTransitionOffsets(
    const gfx::Vector2d& top_left_delta,
    const gfx::Vector2d& bottom_right_delta) {
  gfx::Rect bounds_without_expansion = GetBoundsInPixels();
  window_expansion_top_left_delta_ = top_left_delta;
  window_expansion_bottom_right_delta_ = bottom_right_delta;
  SetBoundsInPixels(bounds_without_expansion);
}

void DesktopWindowTreeHostWin::OnWindowHidingAnimationCompleted() {
  if (pending_close_) {
    message_handler_->Close();
  }
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWin, HWNDMessageHandlerDelegate implementation:

ui::InputMethod* DesktopWindowTreeHostWin::GetHWNDMessageDelegateInputMethod() {
  return GetInputMethod();
}

bool DesktopWindowTreeHostWin::HasNonClientView() const {
  return has_non_client_view_;
}

FrameMode DesktopWindowTreeHostWin::GetFrameMode() const {
  if (const Widget* widget = GetWidget()) {
    return widget->ShouldUseNativeFrame() ? FrameMode::SYSTEM_DRAWN
                                          : FrameMode::CUSTOM_DRAWN;
  }
  return FrameMode::SYSTEM_DRAWN;
}

void DesktopWindowTreeHostWin::ShowCustomSystemMenu(
    const gfx::Point& screen_point) {}

bool DesktopWindowTreeHostWin::UsesNativeSystemMenu() const {
  return true;
}

bool DesktopWindowTreeHostWin::HasFrame() const {
  return !remove_standard_frame_;
}

void DesktopWindowTreeHostWin::SchedulePaint() {
  if (Widget* widget = GetWidget()) {
    widget->GetRootView()->SchedulePaint();
  }
}

bool DesktopWindowTreeHostWin::ShouldPaintAsActive() const {
  if (const Widget* widget = GetWidget()) {
    return widget->ShouldPaintAsActive();
  }
  return false;
}

bool DesktopWindowTreeHostWin::CanResize() const {
  if (const Widget* widget = GetWidget(); widget && widget->widget_delegate()) {
    return widget->widget_delegate()->CanResize();
  }
  return false;
}

bool DesktopWindowTreeHostWin::CanMaximize() const {
  if (const Widget* widget = GetWidget(); widget && widget->widget_delegate()) {
    return widget->widget_delegate()->CanMaximize();
  }
  return false;
}

bool DesktopWindowTreeHostWin::CanMinimize() const {
  if (const Widget* widget = GetWidget(); widget && widget->widget_delegate()) {
    return widget->widget_delegate()->CanMinimize();
  }
  return false;
}

bool DesktopWindowTreeHostWin::CanActivate() const {
  if (IsModalWindowActive()) {
    return true;
  }
  return native_widget_delegate_ ? native_widget_delegate_->CanActivate()
                                 : false;
}

bool DesktopWindowTreeHostWin::WidgetSizeIsClientSize() const {
  if (IsMaximized()) {
    return true;
  }
  if (const Widget* widget = GetWidget()) {
    return widget->ShouldUseNativeFrame();
  }
  return false;
}

bool DesktopWindowTreeHostWin::IsModal() const {
  return native_widget_delegate_ ? native_widget_delegate_->IsModal() : false;
}

int DesktopWindowTreeHostWin::GetInitialShowState() const {
  return CanActivate() ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE;
}

int DesktopWindowTreeHostWin::GetNonClientComponent(
    const gfx::Point& point) const {
  if (!native_widget_delegate_) {
    return HTTRANSPARENT;
  }
  gfx::Point dip_position =
      display::win::GetScreenWin()->ClientToDIPPoint(GetHWND(), point);
  return native_widget_delegate_->GetNonClientComponent(dip_position);
}

void DesktopWindowTreeHostWin::GetWindowMask(const gfx::Size& size_px,
                                             SkPath* path) {
  Widget* widget = GetWidget();
  if (!widget || !widget->non_client_view()) {
    return;
  }

  widget->non_client_view()->GetWindowMask(
      display::win::GetScreenWin()->ScreenToDIPSize(GetHWND(), size_px), path);
  // Convert path in DIPs to pixels.
  if (!path->isEmpty()) {
    const float scale =
        display::win::GetScreenWin()->GetScaleFactorForHWND(GetHWND());
    *path = path->makeTransform(SkMatrix::Scale(scale, scale));
  }
}

bool DesktopWindowTreeHostWin::GetClientAreaInsets(gfx::Insets* insets,
                                                   int frame_thickness) const {
  // WS_THICKFRAME style has a system titlebar. Remove this titlebar for
  // borderless windows.
  if (desktop_native_widget_aura_->widget_type() ==
          Widget::InitParams::TYPE_WINDOW_FRAMELESS &&
      (GetWindowLong(GetHWND(), GWL_STYLE) & WS_THICKFRAME)) {
    *insets = gfx::Insets(frame_thickness);
    // In non-maximized window, the top-border inset must be zero, otherwise
    // Windows will draw a full native titlebar.
    if (!IsMaximized()) {
      insets->set_top(0);
    }

    return true;
  }

  return false;
}

bool DesktopWindowTreeHostWin::GetDwmFrameInsetsInPixels(
    gfx::Insets* insets) const {
  return false;
}

void DesktopWindowTreeHostWin::GetMinMaxSize(gfx::Size* min_size,
                                             gfx::Size* max_size) const {
  if (!native_widget_delegate_) {
    return;
  }

  *min_size = native_widget_delegate_->GetMinimumSize();
  *max_size = native_widget_delegate_->GetMaximumSize();
}

gfx::Size DesktopWindowTreeHostWin::GetRootViewSize() const {
  if (const Widget* widget = GetWidget()) {
    return widget->GetRootView()->size();
  }
  return gfx::Size();
}

gfx::Size DesktopWindowTreeHostWin::DIPToScreenSize(
    const gfx::Size& dip_size) const {
  return display::win::GetScreenWin()->DIPToScreenSize(GetHWND(), dip_size);
}

void DesktopWindowTreeHostWin::ResetWindowControls() {
  if (Widget* widget = GetWidget(); widget && widget->non_client_view()) {
    widget->non_client_view()->ResetWindowControls();
  }
}

gfx::NativeViewAccessible DesktopWindowTreeHostWin::GetNativeViewAccessible() {
  // This function may be called during shutdown when the |RootView| is nullptr.
  if (Widget* widget = GetWidget()) {
    return widget->GetRootView()->GetNativeViewAccessible();
  }
  return nullptr;
}

gfx::NativeViewAccessible
DesktopWindowTreeHostWin::GetParentNativeViewAccessible() {
  views::Widget* widget = GetWidget();
  if (!widget) {
    return nullptr;
  }

  views::Widget* parent_widget = widget->parent();
  if (!parent_widget) {
    return nullptr;
  }

  views::View* parent_root = parent_widget->GetRootView();
  return parent_root ? parent_root->GetNativeViewAccessible() : nullptr;
}

void DesktopWindowTreeHostWin::HandleActivationChanged(bool active) {
#ifdef MOUSEMUX_DEBUG_TRACE
  // Who activates windows while users type: the stack names the caller.
  {
    const std::string stack = base::debug::StackTrace().ToString();
    MouseMuxTraceViews("ACTIVATION", "hwnd=%p active=%d synthetic_key=%d\n%s",
                       static_cast<void*>(GetAcceleratedWidget()), active ? 1 : 0,
                       content::g_mousemux_synthetic_key ? 1 : 0, stack.c_str());
  }
#endif
  if (WidgetActivationDelegate::Get()) {
    return;
  }

  // This can be invoked from HWNDMessageHandler::Init(), at which point we're
  // not in a good state and need to ignore it.
  // TODO(beng): Do we need this still now the host owns the dispatcher?
  if (!dispatcher()) {
    return;
  }

  desktop_native_widget_aura_->HandleActivationChanged(active);
}

bool DesktopWindowTreeHostWin::HandleAppCommand(int command) {
  // We treat APPCOMMAND ids as an extension of our command namespace, and just
  // let the delegate figure out what to do...
  if (Widget* widget = GetWidget(); widget && widget->widget_delegate()) {
    return widget->widget_delegate()->ExecuteWindowsCommand(command);
  }
  return false;
}

void DesktopWindowTreeHostWin::HandleCancelMode() {
  dispatcher()->DispatchCancelModeEvent();
}

void DesktopWindowTreeHostWin::HandleCaptureLost() {
  OnHostLostWindowCapture();
}

void DesktopWindowTreeHostWin::HandleClose() {
  if (Widget* widget = GetWidget()) {
    widget->Close();
  }
}

bool DesktopWindowTreeHostWin::HandleCommand(int command) {
  if (Widget* widget = GetWidget(); widget && widget->widget_delegate()) {
    return widget->widget_delegate()->ExecuteWindowsCommand(command);
  }
  return false;
}

void DesktopWindowTreeHostWin::HandleAccelerator(
    const ui::Accelerator& accelerator) {
  if (Widget* widget = GetWidget()) {
    widget->GetFocusManager()->ProcessAccelerator(accelerator);
  }
}

void DesktopWindowTreeHostWin::HandleCreate() {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetCreated();
  }
}

void DesktopWindowTreeHostWin::HandleDestroying() {
  drag_drop_client_->OnNativeWidgetDestroying(GetHWND());
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetDestroying();
  }

  // Destroy the compositor before destroying the HWND since shutdown
  // may try to swap to the window.
  DestroyCompositor();
}

void DesktopWindowTreeHostWin::HandleDestroyed() {
  desktop_native_widget_aura_->OnHostClosed();
}

bool DesktopWindowTreeHostWin::HandleInitialFocus(
    ui::mojom::WindowShowState show_state) {
  if (Widget* widget = GetWidget()) {
    return widget->SetInitialFocus(show_state);
  }
  return false;
}

void DesktopWindowTreeHostWin::HandleDisplayChange() {
  if (Widget* widget = GetWidget(); widget && widget->widget_delegate()) {
    widget->widget_delegate()->OnDisplayChanged();
  }
}

void DesktopWindowTreeHostWin::HandleBeginWMSizeMove() {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetBeginUserBoundsChange();
  }
}

void DesktopWindowTreeHostWin::HandleEndWMSizeMove() {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetEndUserBoundsChange();
  }
}

void DesktopWindowTreeHostWin::HandleBeginUserResize() {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetUserResizeStarted();
  }
}

void DesktopWindowTreeHostWin::HandleEndUserResize() {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetUserResizeEnded();
  }
}

void DesktopWindowTreeHostWin::HandleBeginUserDrag() {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetUserDragStarted();
  }
}

void DesktopWindowTreeHostWin::HandleEndUserDrag() {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetUserDragEnded();
  }
}

void DesktopWindowTreeHostWin::HandleMove() {
  // Adding/removing a monitor, or changing the primary monitor can cause a
  // WM_MOVE message before `OnDisplayChanged()`. Without this call, we would
  // DCHECK due to stale `DisplayInfo`s. See https:://crbug.com/1413940.
  auto weak_ptr = GetWeakPtr();
  display::win::GetScreenWin()->UpdateDisplayInfosIfNeeded();
  if (!weak_ptr) {
    return;
  }
  CheckForMonitorChange();
  OnHostMovedInPixels();
}

void DesktopWindowTreeHostWin::HandleWorkAreaChanged() {
  CheckForMonitorChange();
  if (Widget* widget = GetWidget(); widget && widget->widget_delegate()) {
    widget->widget_delegate()->OnWorkAreaChanged();
  }
}

void DesktopWindowTreeHostWin::HandleVisibilityChanged(bool visible) {
  if (native_widget_delegate_) {
    native_widget_delegate_->OnNativeWidgetVisibilityChanged(visible);
  }
  if (visible) {
    UpdateDisplayAffinity();
  }
}

void DesktopWindowTreeHostWin::HandleWindowMinimizedOrRestored(bool restored) {
  // Ignore minimize/restore events that happen before widget initialization is
  // done. If a window is created minimized, and then activated, restoring
  // focus will fail because the root window is not visible, which is exposed by
  // ExtensionWindowCreateTest.AcceptState.
  if (!native_widget_delegate_ ||
      !native_widget_delegate_->IsNativeWidgetInitialized()) {
    return;
  }

  if (restored) {
    window()->Show();
  } else {
    window()->Hide();
  }
}

void DesktopWindowTreeHostWin::HandleClientSizeChanged(
    const gfx::Size& new_size) {
  CheckForMonitorChange();
  if (dispatcher()) {
    OnHostResizedInPixels(new_size);
  }
}

void DesktopWindowTreeHostWin::HandleFrameChanged() {
  // Replace the frame and layout the contents.
  if (Widget* widget = GetWidget(); widget && widget->non_client_view()) {
    widget->non_client_view()->UpdateFrame();
  }
}

void DesktopWindowTreeHostWin::HandleNativeFocus(HWND last_focused_window) {
  // TODO(beng): inform the native_widget_delegate_.
}

void DesktopWindowTreeHostWin::HandleNativeBlur(HWND focused_window) {
  // TODO(beng): inform the native_widget_delegate_.
}

bool DesktopWindowTreeHostWin::HandleMouseEvent(ui::MouseEvent* event) {
  // Ignore native platform events for test purposes
  if (ui::PlatformEventSource::ShouldIgnoreNativePlatformEvents()) {
    return true;
  }

  SendEventToSink(event);
  return event->handled();
}

void DesktopWindowTreeHostWin::HandleKeyEvent(ui::KeyEvent* event) {
  // Bypass normal handling of alt-space, which would otherwise consume the
  // corresponding WM_SYSCHAR.  This allows HandleIMEMessage() to show the
  // system menu in this case.  If we instead showed the system menu here, the
  // WM_SYSCHAR would trigger a beep when processed by the native event handler.
  if ((event->type() == ui::EventType::kKeyPressed) &&
      (event->key_code() == ui::VKEY_SPACE) &&
      (event->flags() & ui::EF_ALT_DOWN) &&
      !(event->flags() & ui::EF_CONTROL_DOWN)) {
    if (Widget* widget = GetWidget(); widget && widget->non_client_view()) {
      if (!UsesNativeSystemMenu()) {
        // Show the Views version of the window frame context menu if it should
        // be used instead of the OS native version. Default location for the
        // menu is the origin (0, 0) of the browser.
        gfx::Point point = widget->non_client_view()
                               ->frame_view()
                               ->GetKeyboardContextMenuLocation();
        ShowCustomSystemMenu(point);
        event->SetHandled();
        return;
      }

      return;
    }
  }

  SendEventToSink(event);
}

void DesktopWindowTreeHostWin::HandleTouchEvent(ui::TouchEvent* event) {
  // HWNDMessageHandler asynchronously processes touch events. Because of this
  // it's possible for the aura::WindowEventDispatcher to have been destroyed
  // by the time we attempt to process them.
  Widget* widget = GetWidget();
  if (!widget || !widget->GetNativeView()) {
    return;
  }
  if (in_touch_drag_) {
    POINT event_point(event->location().x(), event->location().y());
    ::ClientToScreen(GetHWND(), &event_point);
    gfx::Point screen_point(event_point);
    // When dragging, Windows requires that touch pointer events are translated
    // to mouse pointer events. The drag controller (`DesktopDragDropClientWin`)
    // will manage gesture states until a drop happens.
    if (event->type() == ui::EventType::kTouchMoved) {
      ui::SendMouseEvent(screen_point,
                         (MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK));
    } else if (event->type() == ui::EventType::kTouchReleased) {
      FinishTouchDrag(screen_point);
    }
    return;
  }
  if (event->type() == ui::EventType::kTouchPressed) {
    display::Screen* screen = display::Screen::Get();
    CHECK(screen);
    aura::Window* window =
        screen->GetWindowAtScreenPoint(screen->GetCursorScreenPoint());
    bool touch_drag_cursor_sync =
        base::FeatureList::IsEnabled(features::kEnableTouchDragCursorSync);
    // If a window is not found at the cursor's screen point, then the mouse is
    // outside of the browser's window and we need to sync the pointer to enable
    // drag and drop. Setting the cursor to the touch location doesn't move the
    // mouse pointer. If the user has their mouse outside of the window when
    // this sync happens, when they move their mouse again it will show up in
    // it's original location.
    if (touch_drag_cursor_sync && !window) {
      POINT event_point(event->location().x(), event->location().y());
      ::ClientToScreen(GetHWND(), &event_point);
      ::SetCursorPos(event_point.x, event_point.y);
    }
  }
  // Currently we assume the window that has capture gets touch events too.
  aura::WindowTreeHost* host =
      aura::WindowTreeHost::GetForAcceleratedWidget(GetCapture());
  if (host) {
    DesktopWindowTreeHostWin* target =
        host->window()->GetProperty(kDesktopWindowTreeHostKey);
    if (target && target->HasCapture() && target != this) {
      POINT target_location(event->location().ToPOINT());
      ClientToScreen(GetHWND(), &target_location);
      ScreenToClient(target->GetHWND(), &target_location);
      ui::TouchEvent target_event(*event, static_cast<View*>(nullptr),
                                  static_cast<View*>(nullptr));
      target_event.set_location(gfx::Point(target_location));
      target_event.set_root_location(target_event.location());
      target->SendEventToSink(&target_event);
      return;
    }
  }
  SendEventToSink(event);
}

bool DesktopWindowTreeHostWin::HandleIMEMessage(UINT message,
                                                WPARAM w_param,
                                                LPARAM l_param,
                                                LRESULT* result) {
  // Show the system menu at an appropriate location on alt-space.
  if ((message == WM_SYSCHAR) && (w_param == VK_SPACE)) {
    if (Widget* widget = GetWidget(); widget && widget->non_client_view()) {
      const auto* frame = GetWidget()->non_client_view()->frame_view();
      ShowSystemMenuAtScreenPixelLocation(
          GetHWND(), frame->GetSystemMenuScreenPixelLocation());
      return true;
    }
  }

  CHROME_MSG msg = {};
  msg.hwnd = GetHWND();
  msg.message = message;
  msg.wParam = w_param;
  msg.lParam = l_param;
  return GetInputMethod()->OnUntranslatedIMEMessage(msg, result);
}

void DesktopWindowTreeHostWin::HandleInputLanguageChange(
    DWORD character_set,
    HKL input_language_id) {
  GetInputMethod()->OnInputLocaleChanged();
}

void DesktopWindowTreeHostWin::HandlePaintAccelerated(
    const gfx::Rect& invalid_rect) {
  if (compositor()) {
    compositor()->ScheduleRedrawRect(invalid_rect);
  }
}

void DesktopWindowTreeHostWin::HandleMenuLoop(bool in_menu_loop) {
  if (in_menu_loop) {
    tooltip_disabler_ = std::make_unique<wm::ScopedTooltipDisabler>(window());
  } else {
    tooltip_disabler_.reset();
  }
}

bool DesktopWindowTreeHostWin::PreHandleMSG(UINT message,
                                            WPARAM w_param,
                                            LPARAM l_param,
                                            LRESULT* result) {
#ifdef MOUSEMUX_DEBUG_TRACE
  // Every native input message that reaches a Chrome window - mouse, key,
  // pointer (pen, touch, and mouse when Windows sends it as pointer),
  // touch, gesture, raw input - so "nothing responds" can be read off the
  // log: what arrived, at which window, and whether that window is in the
  // blocked set.  Whether it was actually dropped is the "BLOCKED msg="
  // line that follows, from the block list itself; this line predicts
  // nothing.  Moves (WM_MOUSEMOVE, WM_POINTERUPDATE) every 50th per window.
  {
    const bool is_mouse =
        (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_NCLBUTTONDOWN && message <= WM_NCMBUTTONDBLCLK) ||
        message == WM_NCXBUTTONDOWN || message == WM_NCXBUTTONUP ||
        message == WM_NCXBUTTONDBLCLK;
    const bool is_pointer =
        (message >= 0x0241 && message <= 0x024F) ||  // WM_NCPOINTER*, WM_POINTER*
        message == WM_TOUCH || message == WM_GESTURE ||
        message == WM_GESTURENOTIFY || message == WM_INPUT;
    const bool is_key = message == WM_KEYDOWN || message == WM_KEYUP ||
                        message == WM_CHAR || message == WM_SYSKEYDOWN ||
                        message == WM_SYSKEYUP;
    if (is_mouse || is_pointer || is_key) {
      const HWND self = GetAcceleratedWidget();
      const bool exempt = self == content::g_mousemux_dialog_hwnd ||
                          self == content::g_mousemux_help_hwnd;
      const bool in_blocked_window =
          !exempt && content::MouseMuxWindowBlocked(self);
      static std::map<HWND, int>* move_counts = new std::map<HWND, int>();
      const bool is_move = message == WM_MOUSEMOVE || message == 0x0245;
      if (!is_move || (++(*move_counts)[self] % 50) == 1) {
        const char* kind = is_key ? "key" : (is_pointer ? "pointer/touch" : "mouse");
        MouseMuxTraceViews(
            "NATIVE/In", "%s msg=0x%04x hwnd=%p client(%d,%d) %s", kind,
            message, static_cast<void*>(self),
            is_mouse ? static_cast<int>(static_cast<int16_t>(LOWORD(l_param))) : 0,
            is_mouse ? static_cast<int>(static_cast<int16_t>(HIWORD(l_param))) : 0,
            exempt ? "dialog/help (exempt)"
                   : (in_blocked_window ? "in-blocked-window" : "in-open-window"));
      }
    }
  }
#endif

#ifdef MOUSEMUX_NATIVE_BLOCK
  // When native input is blocked, drop native mouse and keyboard messages so
  // only SDK custom messages (WM_MOUSEMUX_*) reach the views UI.
  if (content::MouseMuxWindowBlocked(GetAcceleratedWidget()) &&
      GetAcceleratedWidget() != content::g_mousemux_dialog_hwnd &&
      GetAcceleratedWidget() != content::g_mousemux_help_hwnd) {
    // A native click on a blocked window must not ACTIVATE it either.  The
    // activation request precedes the button message and never reached the
    // block list, so the click was dropped but the window still came to the
    // front and the previously active window's page lost focus - the caret
    // vanishing under a blocked click (measured 2026-09-04 21:12: thirteen
    // page blurs in three minutes, all via HandleActivationChanged, zero
    // clicks delivered).  Client-area only: caption buttons and edges keep
    // their native behaviour.
    if (message == WM_MOUSEACTIVATE && LOWORD(l_param) == HTCLIENT) {
      *result = MA_NOACTIVATEANDEAT;
      return true;
    }
    if (message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN ||
        message == WM_NCMBUTTONDOWN) {
      if (!content::g_mousemux_caption_press) {
        content::g_mousemux_caption_press = new std::set<HWND>();
      }
      content::g_mousemux_caption_press->insert(GetAcceleratedWidget());
    }
    if ((message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
         message == WM_MBUTTONUP) &&
        content::g_mousemux_caption_press &&
        content::g_mousemux_caption_press->erase(GetAcceleratedWidget()) > 0) {
      MMTRACE_VIEWS("NATIVE/PreHandleMSG",
                    "release after caption press passes msg=0x%04x hwnd=%p",
                    message, (void*)GetAcceleratedWidget());
      return false;  // The caption button gets its release.
    }
    switch (message) {
      // Keyboard.  Blocked native means blocked native: the page already
      // dropped native keys (its event handler checks the same flag), but a
      // views text field - the address bar, the find bar - took them, so a
      // keyboard whose native input the server had not stopped typed every
      // character twice there (2026-09-05 13:07, MouseMux in switched mode
      // without multi-keyboard).  Our own WM_MOUSEMUX_KEY* messages are
      // different ids and pass.  WM_SYSKEY* included: Alt+F4 from a
      // keyboard this window does not belong to is still not its business.
      case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_DEADCHAR:
      case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR: case WM_SYSDEADCHAR:
      case WM_UNICHAR:
      // Client-area mouse buttons.
      case WM_LBUTTONDOWN: case WM_LBUTTONUP:
      case WM_RBUTTONDOWN: case WM_RBUTTONUP:
      case WM_MBUTTONDOWN: case WM_MBUTTONUP:
      // Double clicks and the side buttons too (2026-09-04).  A fast second
      // press arrives as WM_LBUTTONDBLCLK, not WM_LBUTTONDOWN, and views
      // treats it as a press: 16 of them reached a blocked window's tab strip
      // and switched tabs while every single press was dropped.
      case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
      case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
      // Moves and wheel too (2026-09-04).  Windows re-posts an idle mouse's
      // position to the window holding capture - a menu - and every such
      // move reset the selection under an injected pointer.  Blocked means
      // blocked.
      case WM_MOUSEMOVE:
      case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
#ifdef MOUSEMUX_EXPERIMENT_PEN_TOUCH_BLOCK
      // Pen and touch.  Windows delivers these as WM_POINTER*/WM_TOUCH
      // rather than mouse messages, so the cases above never catch them —
      // without this, native pen/touch still reaches Chrome while blocked.
      case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP:
      case WM_POINTERENTER: case WM_POINTERLEAVE:
      case WM_TOUCH:
#endif
#ifdef MOUSEMUX_EXPERIMENT_NC_HANDLING
      // Non-client area mouse buttons (title bar, caption buttons, resize).
      // Must block these too — otherwise native clicks on the title bar
      // activate/raise the wrong window via DefWindowProc.
      case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP:
      case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP:
      case WM_NCMBUTTONDOWN: case WM_NCMBUTTONUP:
#ifdef MOUSEMUX_EXPERIMENT_PEN_TOUCH_BLOCK
      // Non-client pen/touch, blocked on the same terms as NC mouse buttons.
      case WM_NCPOINTERDOWN: case WM_NCPOINTERUP:
#endif
#endif
#ifdef MOUSEMUX_EXPERIMENT_NATIVE_BLOCK_HARD
      // Mouse movement (prevents hover/highlight from other mice).
      case WM_MOUSEMOVE:
      case WM_NCMOUSEMOVE:
#endif
        MMTRACE_VIEWS("NATIVE/PreHandleMSG",
                      "BLOCKED msg=0x%04x hwnd=%p", message,
                      (void*)GetAcceleratedWidget());
        *result = 0;
        return true;  // Swallow native input.
      default:
        break;
    }
  }
#endif

#ifdef MOUSEMUX_DEBUG_TRACE
  // Diagnostic: does the OS still deliver native mouse moves while a menu is
  // open?  The menu's window holds mouse capture, and a native move there
  // would move the selection to wherever the real cursor sits.
  if (message == WM_MOUSEMOVE && views::MenuController::GetActiveInstance()) {
    MouseMuxTraceViews("NATIVE/MoveUnderMenu", "hwnd=%p client(%d,%d) wparam=0x%x",
                       static_cast<void*>(GetAcceleratedWidget()),
                       static_cast<int>(static_cast<int16_t>(LOWORD(l_param))),
                       static_cast<int>(static_cast<int16_t>(HIWORD(l_param))),
                       static_cast<unsigned>(w_param));
  }
#endif

  // MouseMux keyboard messages: an SDK keystroke enters here as a real one
  // would.  Two things make it behave like native input rather than a test
  // event:
  //
  //  - The window may not be OS-active.  With several users typing at once
  //    at most one window is, and a window that lost activation has had its
  //    focused view stored away and its aura focus cleared, so a key sent to
  //    it lands nowhere.  Re-activating the views side - exactly what the OS
  //    activation path runs - restores that view without touching the OS
  //    foreground, so the other users' windows are left alone.
  //  - Modifiers travel in the message.  GetKeyState() would report one
  //    keyboard's Shift for everybody.
  //
  // The character is inserted straight into the focused TextInputClient.
  // That is what the input method's WM_CHAR handler does, minus the input
  // method itself, which would rebuild the event from OS key state and lose
  // the EF_IS_SYNTHESIZED flag that lets it past native blocking.  Key
  // presses and releases go through the sink like WM_KEYDOWN/UP: the aura
  // dispatcher skips the input method for synthesized events and hands them
  // to the focus manager, so accelerators fire and the focused view gets
  // them.
  if (message == WM_MOUSEMUX_KEYDOWN || message == WM_MOUSEMUX_KEYUP ||
      message == WM_MOUSEMUX_CHAR) {
    base::AutoReset<bool> synthetic_key(&content::g_mousemux_synthetic_key, true);
    if (wm::ActivationClient* activation = wm::GetActivationClient(window());
        activation && !activation->GetActiveWindow()) {
      HandleActivationChanged(true);
    }
    const int flags =
        static_cast<int>(HIWORD(w_param)) | ui::EF_IS_SYNTHESIZED;
    const ui::DomCode dom_code = static_cast<ui::DomCode>(
        static_cast<uint32_t>(l_param & 0xFFFFFFFF));
#ifdef MOUSEMUX_DEBUG_TRACE
    // Where this key is about to go.  "Keys arrive, nothing happens" is
    // this line: a window with no focused view swallows them silently.
    {
      aura::Window* active = nullptr;
      if (wm::ActivationClient* activation =
              wm::GetActivationClient(window())) {
        active = activation->GetActiveWindow();
      }
      aura::Window* focused = nullptr;
      if (aura::client::FocusClient* focus_client =
              aura::client::GetFocusClient(window())) {
        focused = focus_client->GetFocusedWindow();
      }
      views::View* focused_view = nullptr;
      if (GetWidget() && GetWidget()->GetFocusManager()) {
        focused_view = GetWidget()->GetFocusManager()->GetFocusedView();
      }
      MouseMuxTraceViews(
          "KEY/In", "msg=0x%04x hwnd=%p active_window=%p focused_window=%p "
          "focused_view=%s",
          message, static_cast<void*>(GetAcceleratedWidget()),
          static_cast<void*>(active), static_cast<void*>(focused),
          focused_view ? std::string(focused_view->GetClassName()).c_str()
                       : "(none)");
    }
#endif
    if (message == WM_MOUSEMUX_CHAR) {
      ui::InputMethod* input_method = GetInputMethod();
      ui::TextInputClient* target =
          input_method ? input_method->GetTextInputClient() : nullptr;
      if (target) {
        const ui::KeyboardCode vkey = ui::KeyboardCodeForWindowsKeyCode(
            static_cast<WORD>((l_param >> 32) & 0xFFFF));
        ui::KeyEvent event = ui::KeyEvent::FromCharacter(
            static_cast<char16_t>(LOWORD(w_param)), vkey, dom_code, flags);
        target->InsertChar(event);
      }
    } else {
      ui::KeyEvent event(message == WM_MOUSEMUX_KEYDOWN
                             ? ui::EventType::kKeyPressed
                             : ui::EventType::kKeyReleased,
                         ui::KeyboardCodeForWindowsKeyCode(LOWORD(w_param)),
                         dom_code, flags, base::TimeTicks::Now());
      SendEventToSink(&event);
    }
    *result = 0;
    return true;
  }

  // MouseMux custom messages: convert to native mouse events and process.
  // The controller posts WM_MOUSEMUX_* instead of WM_LBUTTONDOWN etc. so
  // SDK clicks never collide with native mouse messages.
  if (message >= WM_MOUSEMUX_LBUTTONDOWN &&
      message <= WM_MOUSEMUX_MOUSELEAVE) {
    // A switch rather than an indexed lookup table: Chromium 151 applies
    // -Wunsafe-buffer-usage to ui/views, and `kNativeMsg[message - base]`
    // trips it.  The index was in range (the condition above bounds it), but
    // the compiler cannot prove that, and an explicit mapping is clearer than
    // pointer arithmetic that happens to be correct.
    UINT native_msg = 0;
    switch (message) {
      case WM_MOUSEMUX_LBUTTONDOWN: native_msg = WM_LBUTTONDOWN; break;
      case WM_MOUSEMUX_LBUTTONUP:   native_msg = WM_LBUTTONUP;   break;
      case WM_MOUSEMUX_RBUTTONDOWN: native_msg = WM_RBUTTONDOWN; break;
      case WM_MOUSEMUX_RBUTTONUP:   native_msg = WM_RBUTTONUP;   break;
      case WM_MOUSEMUX_MBUTTONDOWN: native_msg = WM_MBUTTONDOWN; break;
      case WM_MOUSEMUX_MBUTTONUP:   native_msg = WM_MBUTTONUP;   break;
      case WM_MOUSEMUX_MOUSEMOVE:   native_msg = WM_MOUSEMOVE;   break;
      case WM_MOUSEMUX_MOUSELEAVE:  native_msg = WM_MOUSELEAVE;  break;
      default:
        // Unreachable: the range check above admits only the messages above.
        return false;
    }

    HWND hwnd = GetAcceleratedWidget();
    [[maybe_unused]] const bool is_down = (native_msg == WM_LBUTTONDOWN ||
                    native_msg == WM_RBUTTONDOWN ||
                    native_msg == WM_MBUTTONDOWN);

    // Only the client area is a user's to click: tabs, the new-tab button,
    // the toolbar, the page.  The caption buttons, the drag strip beside
    // the tabs and the resize edges are window management, the operator's
    // job with the real mouse, and an injected click there used to reach
    // Chrome's painted caption buttons as an ordinary press and minimize or
    // close somebody's window.  The frame's own hit test - the one Windows
    // consults for WM_NCHITTEST - says which is which; anything but HTCLIENT
    // is dropped, moves included, so those buttons do not even highlight.
    // (2026-09-04)
    if (native_msg != WM_MOUSELEAVE) {
      if (views::Widget* widget = GetWidget();
          widget && widget->non_client_view()) {
        gfx::Point client_point(CR_GET_X_LPARAM(l_param),
                                CR_GET_Y_LPARAM(l_param));
        ConvertPixelsToDIP(&client_point);
        const int hit = widget->non_client_view()->NonClientHitTest(client_point);
        if (hit != HTCLIENT) {
#ifdef MOUSEMUX_DEBUG_TRACE
          MouseMuxTraceViews("NATIVE/CustomMouse",
                             "DROPPED non-client hit=%d msg=0x%x hwnd=%p "
                             "client(%d,%d)",
                             hit, native_msg,
                             static_cast<void*>(GetAcceleratedWidget()),
                             client_point.x(), client_point.y());
#endif
          *result = 0;
          return true;
        }
      }
    }

    // Convert client coords (lParam) to screen coords.
    POINT screen_pt = {CR_GET_X_LPARAM(l_param), CR_GET_Y_LPARAM(l_param)};
    ::ClientToScreen(hwnd, &screen_pt);

#ifdef MOUSEMUX_EXPERIMENT_NC_HANDLING
    // Hit-test to check if the click lands in the non-client area
    // (title bar, caption buttons, resize edges).  SendEventToSink does
    // NOT handle non-client events — no Views exist there — so we must
    // handle activation, close, min/max via Win32 APIs directly.
    LRESULT hit_test = ::SendMessage(
        hwnd, WM_NCHITTEST, 0, MAKELPARAM(screen_pt.x, screen_pt.y));

    if (hit_test != HTCLIENT) {
      // Non-client area.
      if (is_down) {
        ::SetForegroundWindow(hwnd);
        ::BringWindowToTop(hwnd);
      } else {
        // Trigger the action on mouseup (matches normal Windows behaviour).
        switch (hit_test) {
          case HTCLOSE:
            ::PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
          case HTMINBUTTON:
            ::PostMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            break;
          case HTMAXBUTTON:
            if (::IsZoomed(hwnd))
              ::PostMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            else
              ::PostMessage(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
            break;
          default:
            break;
        }
      }
      *result = 0;
      return true;  // Consumed.
    }
#endif  // MOUSEMUX_EXPERIMENT_NC_HANDLING

    // Client area: build a CHROME_MSG so MouseEventFromMSG handles
    // coordinate transforms, DPI scaling, and button detection correctly.
    CHROME_MSG fake_msg = {};
    fake_msg.hwnd = hwnd;
    fake_msg.message = native_msg;
    fake_msg.wParam = w_param;
    fake_msg.lParam = l_param;
    fake_msg.time = static_cast<DWORD>(::GetMessageTime());
    fake_msg.pt = {screen_pt.x, screen_pt.y};

    // On mousedown, raise the window — normal WM_LBUTTONDOWN does this via
    // DefWindowProc, but our custom path bypasses that.
#ifdef MOUSEMUX_DEBUG_TRACE
    const HWND fg_before = ::GetForegroundWindow();
    const bool menu_before = views::MenuController::GetActiveInstance() != nullptr;
#endif
    // No OS activation on a synthetic click (2026-09-04).  This used to
    // SetForegroundWindow(hwnd) on every down, from the days when keys went
    // straight to the renderer and needed the window OS-active.  Keys now
    // enter through the window's own focus manager, so activation buys
    // nothing, and it cost the menus: activating a menu's popup window
    // deactivates the browser window that owns it, and the menu closes under
    // the click (measured: menu_before=1, menu_after=0).  Left out entirely
    // for now; if clicked windows need raising, SetWindowPos(HWND_TOP,
    // SWP_NOACTIVATE) is the call, not this.
    // if (is_down) {
    //   ::SetForegroundWindow(hwnd);
    // }

    ui::MouseEvent event = ui::MouseEventFromMSG(fake_msg);
    {
      base::AutoReset<bool> dispatching(&content::g_mousemux_in_custom_dispatch,
                                        true);
      SendEventToSink(&event);
    }
#ifdef MOUSEMUX_DEBUG_TRACE
    MouseMuxTraceViews(
        "NATIVE/CustomMouse",
        "msg=0x%x hwnd=%p client(%d,%d) flags=0x%x handled=%d fg_before=%p "
        "fg_after=%p menu_before=%d menu_after=%d",
        native_msg, static_cast<void*>(hwnd), static_cast<int>(static_cast<int16_t>(LOWORD(l_param))),
        static_cast<int>(static_cast<int16_t>(HIWORD(l_param))), event.flags(),
        event.handled() ? 1 : 0, static_cast<void*>(fg_before),
        static_cast<void*>(::GetForegroundWindow()), menu_before ? 1 : 0,
        views::MenuController::GetActiveInstance() != nullptr ? 1 : 0);
#endif
    *result = 0;
    return true;  // Consumed — do not pass to default handler.
  }
  return false;
}

void DesktopWindowTreeHostWin::PostHandleMSG(UINT message,
                                             WPARAM w_param,
                                             LPARAM l_param) {}

bool DesktopWindowTreeHostWin::HandleScrollEvent(ui::ScrollEvent* event) {
  SendEventToSink(event);
  return event->handled();
}

bool DesktopWindowTreeHostWin::HandleGestureEvent(ui::GestureEvent* event) {
  SendEventToSink(event);
  return event->handled();
}

void DesktopWindowTreeHostWin::HandleWindowSizeChanging() {
  if (compositor()) {
    compositor()->DisableSwapUntilResize();
  }
}

void DesktopWindowTreeHostWin::HandleWindowSizeUnchanged() {
  // A resize may not have occurred if the window size happened not to have
  // changed (can occur on Windows 10 when snapping a window to the side of
  // the screen). In that case do a resize to the current size to reenable
  // swaps.
  if (compositor()) {
    compositor()->ReenableSwap();
  }
}

void DesktopWindowTreeHostWin::HandleWindowScaleFactorChanged(
    float window_scale_factor) {
  // TODO(ccameron): This will violate surface invariants, and is insane.
  // Shouldn't the scale factor and window pixel size changes be sent
  // atomically? And how does this interact with updates to display::Display?
  // Should we expect the display::Display to be updated before this? If so,
  // why can't we use the DisplayObserver that the base WindowTreeHost is
  // using?
  if (compositor()) {
    compositor()->SetScaleAndSize(
        window_scale_factor, message_handler_->GetClientAreaBounds().size(),
        window()->GetLocalSurfaceId());
  }
}

void DesktopWindowTreeHostWin::HandleHeadlessWindowBoundsChanged(
    const gfx::Rect& bounds) {
  window()->SetProperty(aura::client::kHeadlessBoundsKey, bounds);
}

HBRUSH DesktopWindowTreeHostWin::GetBackgroundPaintBrush() {
  return background_paint_brush_;
}

DesktopNativeCursorManager*
DesktopWindowTreeHostWin::GetSingletonDesktopNativeCursorManager() {
  return new DesktopNativeCursorManagerWin();
}

void DesktopWindowTreeHostWin::SetBoundsInDIP(const gfx::Rect& bounds) {
  // The window parameter is intentionally passed as nullptr on Windows because
  // a non-null window parameter causes errors when restoring windows to saved
  // positions in variable-DPI situations. See https://crbug.com/1224715 for
  // details.
  aura::Window* root = nullptr;
  const gfx::Rect bounds_in_pixels =
      display::Screen::Get()->DIPToScreenRectInWindow(
          root, AdjustedContentBounds(bounds));
  AsWindowTreeHost()->SetBoundsInPixels(bounds_in_pixels);
}

void DesktopWindowTreeHostWin::SetAllowScreenshots(bool allow) {
  if (allow_screenshots_ == allow) {
    return;
  }

  allow_screenshots_ = allow;

  // If the window is not visible, do not set the window display affinity
  // because `SetWindowDisplayAffinity` will attempt to compose the window,
  // resulting in a blank window. Instead, we will update it in the `Show`
  // function.
  if (!IsVisible()) {
    return;
  }

  UpdateDisplayAffinity();
}

bool DesktopWindowTreeHostWin::AreScreenshotsAllowed() {
  return allow_screenshots_;
}

void DesktopWindowTreeHostWin::SetExcludeFromScreenCapture(bool exclude) {
  if (exclude_from_capture_ == exclude) {
    return;
  }

  exclude_from_capture_ = exclude;

  if (!IsVisible()) {
    return;
  }

  UpdateDisplayAffinity();
}

void DesktopWindowTreeHostWin::ClientDestroyedWidget() {
  if (native_widget_delegate_) {
    // Send an explicit visibility change event when the delegate is reset. This
    // ensures delegate's visibility state is updated appropriately before the
    // delegate pointer is nullified.
    native_widget_delegate_->OnNativeWidgetVisibilityChanged(false);
    native_widget_delegate_ = nullptr;
  }
  DesktopWindowTreeHost::ClientDestroyedWidget();
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHostWin, private:

Widget* DesktopWindowTreeHostWin::GetWidget() {
  return native_widget_delegate_ ? native_widget_delegate_->AsWidget()
                                 : nullptr;
}

const Widget* DesktopWindowTreeHostWin::GetWidget() const {
  return native_widget_delegate_ ? native_widget_delegate_->AsWidget()
                                 : nullptr;
}

HWND DesktopWindowTreeHostWin::GetHWND() const {
  return message_handler_->hwnd();
}

bool DesktopWindowTreeHostWin::IsModalWindowActive() const {
  // This function can get called during window creation which occurs before
  // dispatcher() has been created.
  if (!dispatcher()) {
    return false;
  }

  const auto is_active = [](const aura::Window* child) {
    return child->GetProperty(aura::client::kModalKey) !=
               ui::mojom::ModalType::kNone &&
           child->TargetVisibility();
  };
  return std::ranges::any_of(window()->children(), is_active);
}

void DesktopWindowTreeHostWin::CheckForMonitorChange() {
  display::Display nearest_display =
      display::Screen::Get()->GetDisplayNearestWindow(window());
  if (nearest_display == last_nearest_display_) {
    return;
  }
  last_nearest_display_ = nearest_display;

  OnHostDisplayChanged();
}

gfx::Rect DesktopWindowTreeHostWin::AdjustedContentBounds(
    const gfx::Rect& bounds) {
  gfx::Size minimum_size;
  gfx::Size maximum_size;
  GetMinMaxSize(&minimum_size, &maximum_size);

  if (WidgetSizeIsClientSize()) {
    // Constraints are sized to the client area, not the HWND (see
    // OnGetMinMaxInfo), so inflate otherwise the max size will be too small.
    display::win::ScreenWin* screen = display::win::GetScreenWin();
    gfx::Size min_px = screen->DIPToScreenSize(GetHWND(), minimum_size);
    gfx::Size max_px = screen->DIPToScreenSize(GetHWND(), maximum_size);
    InflateClientSizeConstraintsInPixels(GetHWND(), min_px, max_px);
    minimum_size = screen->ScreenToDIPSize(GetHWND(), min_px);
    maximum_size = screen->ScreenToDIPSize(GetHWND(), max_px);
  }

  gfx::Size bounds_size = bounds.size();

  if (!maximum_size.IsEmpty()) {
    bounds_size.SetToMin(maximum_size);
  }

  if (!minimum_size.IsEmpty()) {
    bounds_size.SetToMax(minimum_size);
  }

  gfx::Rect adjusted_bounds = bounds;
  adjusted_bounds.set_size(bounds_size);
  return adjusted_bounds;
}

aura::Window* DesktopWindowTreeHostWin::content_window() {
  return desktop_native_widget_aura_->content_window();
}

void DesktopWindowTreeHostWin::UpdateDisplayAffinity() {
  DWORD affinity = WDA_NONE;
  if (exclude_from_capture_ && IsCaptureExclusionAllowed()) {
    // `exclude_from_capture_` is used to exclude the window completely from
    // screen capture. On Windows 10 20H1 and newer, we use
    // WDA_EXCLUDEFROMCAPTURE which hides the window from capture while keeping
    // it visible to the user.
    affinity = (base::win::GetVersion() >= base::win::Version::WIN10_20H1)
                   ? WDA_EXCLUDEFROMCAPTURE
                   : WDA_MONITOR;
  } else if (!allow_screenshots_) {
    // `allow_screenshots_` is used to avoid capturing sensitive content.
    // When screenshots are not allowed, we set the affinity to WDA_MONITOR
    // rather than WDA_EXCLUDEFROMCAPTURE. WDA_MONITOR obscures the window with
    // a black rectangle in the capture, explicitly signaling to the user that
    // the content is intentionally hidden. In contrast, WDA_EXCLUDEFROMCAPTURE
    // completely removes the window from the capture stream, leaving no visual
    // cue.
    affinity = WDA_MONITOR;
  }

  SetWindowDisplayAffinity(GetHWND(), affinity);
}

bool DesktopWindowTreeHostWin::IsCaptureExclusionAllowed() const {
  const bool is_remote_session = remote_session_for_testing_.value_or(
      ::GetSystemMetrics(SM_REMOTESESSION) != 0);

  // We allow exclusion if it's a local session, OR if the feature flag
  // overrides the remote session restriction.
  return !is_remote_session ||
         base::FeatureList::IsEnabled(
             views::features::kAllowWindowCaptureExclusionInRemoteSessions);
}

void DesktopWindowTreeHostWin::UpdateBackdropColorMode() {
  // Update backdrop theme using DWMWA_USE_IMMERSIVE_DARK_MODE.
  if (!ShouldAddDWMBackdrop()) {
    return;
  }

  // Ensure that the backdrop honors the OS dark mode setting.
  BOOL use_dark_mode =
      GetWidget()->GetColorMode() == ui::ColorProviderKey::ColorMode::kDark;
  HRESULT hr = DwmSetWindowAttribute(GetHWND(), DWMWA_USE_IMMERSIVE_DARK_MODE,
                                     &use_dark_mode, sizeof(use_dark_mode));
  if (FAILED(hr)) {
    // If DwmSetWindowAttribute fails, it indicates that there was a problem
    // setting dark mode for the window. In this state, the mode change is not
    // applied and the backdrop will remain in its previous state.
    LOG(ERROR) << "Failed to set DWM immersive dark mode: "
               << logging::SystemErrorCodeToString(
                      static_cast<logging::SystemErrorCode>(hr));
  }
}

bool DesktopWindowTreeHostWin::ShouldAddDWMBackdrop() {
  // If the Redirection Surface is removed, there needs to be a replacement
  // "background" of the Chromium window. `DWM_SYSTEMBACKDROP_TYPE` tells DWM
  // to blur the contents behind the chromium window to yield a translucent
  // "frosted glass" effect. This will show whenever the GPU crashes or is not
  // ready by the time the window updates size or shape. Translucent windows
  // do not need a backdrop as it would show up in unexpected ways - i.e. a
  // gutter. Additionally, ensure that this effect is only applied to top level
  // windows since child windows are not supported.
  return ((message_handler_->window_ex_style() & WS_EX_NOREDIRECTIONBITMAP) ==
          WS_EX_NOREDIRECTIONBITMAP) &&
         !message_handler_->is_translucent() &&
         (GetHWND() == GetAncestor(GetHWND(), GA_ROOT));
}

void DesktopWindowTreeHostWin::ClearBackgroundPaintBrush() {
  if (background_paint_brush_) {
    DeleteObject(background_paint_brush_);
    background_paint_brush_ = nullptr;
  }
}

////////////////////////////////////////////////////////////////////////////////
// DesktopWindowTreeHost, public:

// static
DesktopWindowTreeHost* DesktopWindowTreeHost::Create(
    internal::NativeWidgetDelegate* native_widget_delegate,
    DesktopNativeWidgetAura* desktop_native_widget_aura) {
  return new DesktopWindowTreeHostWin(native_widget_delegate,
                                      desktop_native_widget_aura);
}

}  // namespace views
