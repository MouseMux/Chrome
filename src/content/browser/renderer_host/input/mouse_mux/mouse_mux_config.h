// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONFIG_H_
#define CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONFIG_H_

// MouseMux compile-time configuration.
// This header is intentionally minimal — it contains ONLY defines so that
// files can include it without pulling in the full controller header and
// triggering cascade rebuilds.
//
// NAMING CONVENTION — the prefix states maturity, so the default state is
// readable from the name alone:
//
//   MOUSEMUX_*             shipped and verified on hardware.  ON.
//   MOUSEMUX_EXPERIMENT_*  unverified or known-broken.  OFF, by definition.
//                          An experiment that is on is a contradiction: either
//                          promote it out of the prefix or turn it off.
//   MOUSEMUX_DEBUG*        diagnostics that write input to disk.  OFF in any
//                          build that leaves this machine.
//
// MIRRORING — ui/views cannot include content/, so
// desktop_window_tree_host_win.cc re-declares the defines it needs at the top
// of the file.  That list is kept line-for-line identical to the one here,
// commented-out entries included, so the two diff cleanly.  A define used in
// that file but missing from its list is silently dead: the #ifdef simply
// never fires and enabling it here does nothing.  Flip both together.

// ---------------------------------------------------------------------------
// VERSION STAMP — the one place.  Release 2.2.N is build #N; bump all three
// together for a release.  The client reports the version to the server, the
// dialog shows build and date in its footer.  They used to live in those two
// files separately and had already drifted apart.
// ---------------------------------------------------------------------------
#define MOUSEMUX_VERSION "2.2.62"
#define MOUSEMUX_BUILD_NUMBER 62
#define MOUSEMUX_BUILD_DATE "2026-09-05"

// ---------------------------------------------------------------------------
// SHIPPED FEATURES — verified, on by default.
// ---------------------------------------------------------------------------

// Dispatch SDK clicks that miss web content through the aura event system so
// Chrome UI (tabs, address bar, popups, etc.) can receive them.
// Verified since v10.  Content layer only — not mirrored in the views file.
#define MOUSEMUX_AURA_UI_CLICK_THROUGH

// Native mouse blocking: when enabled, PreHandleMSG in
// DesktopWindowTreeHostWin drops native WM_LBUTTONDOWN/UP etc. while
// native input is blocked.  Without this, only the content/renderer layer
// blocks native events — Chrome UI (tabs, toolbar) still receives them.
// Mirrored in the views file.
#define MOUSEMUX_NATIVE_BLOCK

// SDK pen/touch injection.  The client parses "pointer.pen.notify.M2A" and the
// controller injects events carrying blink pointer properties (type, pressure,
// tilt, twist).  Device subtype comes from the user list ("mouse",
// "pen_external", "pen_internal", "touch", "touchpad"); touchpad maps to mouse
// since it drives a cursor rather than making direct contact.
//
// Not yet verified on pen/touch hardware, but kept ON deliberately: the server
// sends this message type for touchpad devices too, so disabling it would stop
// touchpads moving — a regression against 2.2.53, which shipped with it on.
// This half can only ADD events; it never suppresses native input.  The half
// that does is MOUSEMUX_EXPERIMENT_PEN_TOUCH_BLOCK below.
#define MOUSEMUX_PEN_TOUCH_INJECT

// Multiple simultaneous owners: several device pairs driving one browser, each
// in its own window, none able to disturb another.  Everything that used to be
// a single member — keyboard target, drag target, button state, held keys,
// pending motion — is per device, and each user's keystrokes follow their own
// mouse.
//
// Two things this required, both non-obvious.  The OS-level focus calls in
// InjectKeyboardEvent (SetForegroundWindow, view->Focus()) are gone: with one
// owner they were harmless, with four they were four keystroke streams
// fighting over one foreground window.  Only host->Focus() remains, a renderer
// IPC rather than an OS call, applied to every window — separate windows are
// separate WebContents, so nothing arbitrates focus between them and each
// renderer can be told independently that its page is focused.  And while
// captured, a window losing OS focus no longer blurs its view, because
// otherwise the operator clicking this dialog extinguishes every user's caret.
//
// Capture is a hard requirement, not a nicety: native input is what forces
// Windows to have one active window, and one active window is what makes one
// user's click kill another user's caret.  All owners captured, or none of
// this holds.
//
// Verified 2026-09-01 on hardware: two users, two mice, two keyboards, two
// windows, typing simultaneously with two blinking carets and no crossover.
// No longer a switch (2026-09-04): the single-owner code it replaced is gone.

// Keyboard layout translation.  Injected characters come from ToUnicodeEx
// against the layout actually in use — the same call Windows makes to turn
// WM_KEYDOWN into WM_CHAR — rather than from Chromium's US-layout tables.
//
// Without this, letters and digits land correctly because layouts broadly
// agree on them, but punctuation does not, and a dedicated key like ABNT2's
// c-cedilla or any dead-key accent cannot be produced AT ALL.  That affected
// every release up to 2.2.56 and every non-US customer.
//
// DomCode is deliberately still derived from the US tables: it describes a
// physical key POSITION, which does not vary by layout, and that is what
// Chromium itself does for synthetic events.  Only the character is
// layout-dependent.
//
// Dead-key composition is held per device.  ToUnicodeEx keeps its state per
// THREAD and the browser has one thread, so without that, one user's
// half-finished accent composes into another user's next keystroke.  AltGr
// (right Alt) presents as Ctrl+Alt, which is how ABNT2 and most European
// layouts reach their third level.
//
// Verified 2026-09-01 on a Brazilian ABNT2 layout: c-cedilla, and acute,
// tilde and circumflex accents all composing correctly, with ordinary Latin
// typing and Ctrl shortcuts unaffected.  Content layer only.
#define MOUSEMUX_KEYBOARD_LAYOUT

// Anonymous keys in the logs.  With MOUSEMUX_DEBUG on, every key event is
// logged, and key codes are letters and digits in all but name: a log sent
// in from the field could be read back as what was typed.  With this on,
// keys that produce text - letters, digits, space, numpad, punctuation - are
// logged as 'X' (vkey 0x58, scan 0, character U+0058).  Which device typed,
// when, into which window, accepted or dropped: all still there.  Modifier,
// navigation and function keys are logged as they are; they carry no text.
// Raw MouseMux keyboard messages are not logged at all.  ON for builds that
// leave this machine; OFF (comment out) for rich logging on the bench.
#define MOUSEMUX_ANONYMOUS_KEYS

static inline bool MouseMuxIsTextKey(int vkey) {
  return vkey == 0x20 || (vkey >= 0x30 && vkey <= 0x39) ||
         (vkey >= 0x41 && vkey <= 0x5A) || (vkey >= 0x60 && vkey <= 0x6F) ||
         (vkey >= 0xBA && vkey <= 0xE2);
}
#ifdef MOUSEMUX_ANONYMOUS_KEYS
static inline int MouseMuxLogVkey(int vkey) {
  return MouseMuxIsTextKey(vkey) ? 0x58 : vkey;
}
static inline int MouseMuxLogScan(int vkey, int scan) {
  return MouseMuxIsTextKey(vkey) ? 0 : scan;
}
static inline unsigned MouseMuxLogChar(unsigned) {
  return 0x58;
}
static inline int MouseMuxLogDomKey(int vkey, int dom_key) {
  return MouseMuxIsTextKey(vkey) ? 0 : dom_key;
}
static inline bool MouseMuxLogRawKeys() {
  return false;
}
#else
static inline int MouseMuxLogVkey(int vkey) {
  return vkey;
}
static inline int MouseMuxLogScan(int, int scan) {
  return scan;
}
static inline unsigned MouseMuxLogChar(unsigned ch) {
  return ch;
}
static inline int MouseMuxLogDomKey(int, int dom_key) {
  return dom_key;
}
static inline bool MouseMuxLogRawKeys() {
  return true;
}
#endif

// Keyboard through the window, not straight into the renderer.
//
// Off, an SDK keystroke is handed to the active tab's RenderWidgetHost and
// the character to RenderWidgetHostViewAura::InsertChar.  That reaches the
// page and nothing else: the omnibox, the find bar and every browser shortcut
// live in the views focus manager, which that path never visits.  On, the
// controller posts WM_MOUSEMUX_KEYDOWN/KEYUP/CHAR to the user's window and
// DesktopWindowTreeHostWin::PreHandleMSG feeds them in where a real
// WM_KEYDOWN enters - through the focus manager to whichever view holds
// focus - so keys land wherever the user last clicked, page or chrome.
// No longer a switch (2026-09-04): the InsertChar path it replaced is gone.

// ---------------------------------------------------------------------------
// EXPERIMENTS — unverified or known-broken.  All OFF.
// ---------------------------------------------------------------------------

// Native pen/touch blocking.  When native input is blocked, also drop
// WM_POINTER*/WM_TOUCH in PreHandleMSG and ui::TouchEvent in the content
// layer, matching what already happens for mouse.
//
// OFF because it is the only untested path that can SUBTRACT input: if
// injection does not deliver, a touchscreen goes completely dead with no way
// back except restarting the browser.  Turning it on without a touchscreen to
// test on is how you ship a build nobody can click out of.
// Mirrored in the views file.
// #define MOUSEMUX_EXPERIMENT_PEN_TOUCH_BLOCK

// Hard mode: on top of MOUSEMUX_NATIVE_BLOCK, also block mouse movement,
// non-client clicks and keyboard.  WM_SYSKEY* is left through so Alt+F4 still
// closes the window — without that escape hatch there is no way out.
// Near-total lockout of native input; only useful when the SDK must be the
// sole input source for a test.
// Mirrored in the views file.
// #define MOUSEMUX_EXPERIMENT_NATIVE_BLOCK_HARD

// Non-client area handling: block native WM_NCLBUTTONDOWN etc. and use
// WM_NCHITTEST in PreHandleMSG to route title bar / caption button clicks
// through Win32 APIs (SetForegroundWindow, WM_CLOSE, WM_SYSCOMMAND).
// Also uses GetWindowRect instead of GetBoundsInPixels for hit testing
// so title bar area is included in window bounds.
// Does not work — Chrome uses window hooks for tab dragging.
// Mirrored in the views file.
// #define MOUSEMUX_EXPERIMENT_NC_HANDLING

// ---------------------------------------------------------------------------
// DIAGNOSTICS — write input to disk.  All OFF for release.
// ---------------------------------------------------------------------------

// Enable debug mode: shows the log panel in the dialog, writes to log files,
// and enables verbose per-event logging (every key press, mouse click detail,
// etc.).
//
// MUST STAY COMMENTED OUT FOR RELEASE BUILDS.  When defined, keyboard and
// mouse events are written to disk, which means release binaries would log
// every keystroke the user types.  All logging in the MouseMux code is
// guarded by this define — nothing writes to disk without it.
#define MOUSEMUX_DEBUG  // ON for the 2026-09-03 field debug build

// Log file paths.  Deliberately declared INSIDE the debug guard: any logging
// that escapes the #ifdef then fails to compile rather than quietly shipping.
// Do not move these out of the guard.
#if defined(MOUSEMUX_DEBUG) || defined(MOUSEMUX_DEBUG_TRACE)
#include <string>
#include <windows.h>

// Every debug log goes to %TEMP%.
//
// The paths used to be hardcoded to O:\, which exists on exactly one machine.
// On any other, the file open failed silently and a debug build sent out to
// reproduce a bug came back with no log at all - which is worse than no debug
// build, because it looks like the bug left no trace.
static inline std::string MouseMuxLogPath(const char* name) {
  std::string dir(MAX_PATH, '\0');
  const DWORD n = GetTempPathA(MAX_PATH, dir.data());
  if (n == 0 || n >= MAX_PATH) {
    return std::string(name);  // Current directory: still better than nothing.
  }
  dir.resize(n);
  return dir + name;
}
#endif

#ifdef MOUSEMUX_DEBUG
#define MOUSEMUX_DIAG_LOG_PATH  MouseMuxLogPath("mousemux_diag.log")
#define MOUSEMUX_DEBUG_LOG_PATH MouseMuxLogPath("mousemux_debug.log")
#endif

// Traces the whole input pipeline to a file, stage by stage, including every
// point where an event is DROPPED and why.  Independent of MOUSEMUX_DEBUG so
// the dialog keeps its normal size and the log panel stays off.
//
// COMMENT OUT FOR RELEASE.  It writes input coordinates and key codes to disk
// and reopens the file on every line, so it is slow as well as sensitive.
// Mirrored in the views file.
#define MOUSEMUX_DEBUG_TRACE  // ON for the 2026-09-03 field debug build

#ifdef MOUSEMUX_DEBUG_TRACE
#define MOUSEMUX_DEBUG_TRACE_PATH MouseMuxLogPath("chrome-log.txt")

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

// Opens, writes and closes per line.  Deliberately: a buffered handle loses
// the last writes when the browser crashes or is killed, which is exactly the
// moment the tail of the log matters most.  Falls back to %TEMP% when the
// configured drive does not exist on the target machine.
//
// The suppression below is scoped to this one function on purpose.  Chromium
// 151 rejects fprintf/vfprintf/va_list under -Wunsafe-buffer-usage, and the
// sanctioned alternative - listing our directory in unsafe_buffers_paths.txt -
// would switch those checks off for ALL of our code permanently, to make a
// debug facility compile.  This way the exemption disappears with the trace
// build it exists for.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
static inline void MouseMuxTrace(const char* stage, const char* fmt, ...) {
  FILE* f = fopen(MOUSEMUX_DEBUG_TRACE_PATH.c_str(), "a");
  if (!f) {
    char fallback[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, fallback);
    if (n == 0 || n >= MAX_PATH) {
      return;
    }
    strncat_s(fallback, sizeof(fallback), "chrome-log.txt", _TRUNCATE);
    f = fopen(fallback, "a");
    if (!f) {
      return;
    }
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

#define MMTRACE(stage, ...) MouseMuxTrace(stage, __VA_ARGS__)
#else
#define MMTRACE(stage, ...) ((void)0)
#endif  // MOUSEMUX_DEBUG_TRACE

// ---------------------------------------------------------------------------
// Protocol constants — not switches.
// ---------------------------------------------------------------------------

// Custom Win32 messages for MouseMux SDK click injection.
// Using WM_APP range to avoid clashing with native mouse messages.
// PreHandleMSG in DesktopWindowTreeHostWin converts these back to real
// mouse events via MouseEventFromMSG, keeping SDK and native paths separate.
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

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONFIG_H_
