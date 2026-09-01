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
// Content layer only — not mirrored in the views file.
#define MOUSEMUX_MULTI_OWNER

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

// Keyboard layout translation.  InjectKeyboardEvent currently derives the
// character through ui::UsLayoutKeyboardCodeToDomCode and
// ui::DomCodeToUsLayoutDomKey — hardcoded US.  Latin letters and digits land
// correctly because the layouts agree there; punctuation does not, and neither
// c-cedilla nor any dead-key accent (acute + a -> a-acute) can be produced.
//
// With this on the character comes from ToUnicodeEx against a real HKL, using
// the vkey AND scan code the SDK already sends (both arrive in OnKeyboardKey,
// and scan is currently only logged).  Dead-key composition state must be held
// per device rather than left to the Win32 per-thread buffer, or two users
// mid-accent corrupt each other.
//
// OFF: unproven, and untestable without a non-US keyboard.  Content layer only.
// #define MOUSEMUX_EXPERIMENT_MULTI_LANGUAGE

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
// #define MOUSEMUX_DEBUG

// Log file paths.  Deliberately declared INSIDE the debug guard: any logging
// that escapes the #ifdef then fails to compile rather than quietly shipping.
// Do not move these out of the guard.
#ifdef MOUSEMUX_DEBUG
#define MOUSEMUX_DIAG_LOG_PATH  "O:/tmp/mousemux_diag.log"
#define MOUSEMUX_DEBUG_LOG_PATH "O:/tmp/mousemux_debug.log"
#endif

// Traces the whole input pipeline to a file, stage by stage, including every
// point where an event is DROPPED and why.  Independent of MOUSEMUX_DEBUG so
// the dialog keeps its normal size and the log panel stays off.
//
// COMMENT OUT FOR RELEASE.  It writes input coordinates and key codes to disk
// and reopens the file on every line, so it is slow as well as sensitive.
// Mirrored in the views file.
// #define MOUSEMUX_DEBUG_TRACE

#ifdef MOUSEMUX_DEBUG_TRACE
#define MOUSEMUX_DEBUG_TRACE_PATH "O:\\chrome-log.txt"

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

// Opens, writes and closes per line.  Deliberately: a buffered handle loses
// the last writes when the browser crashes or is killed, which is exactly the
// moment the tail of the log matters most.  Falls back to %TEMP% when the
// configured drive does not exist on the target machine.
static inline void MouseMuxTrace(const char* stage, const char* fmt, ...) {
  FILE* f = fopen(MOUSEMUX_DEBUG_TRACE_PATH, "a");
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

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONFIG_H_
