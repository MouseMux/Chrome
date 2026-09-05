# TODO

Open items, newest first. Each says why it is open and what would close it.

- **Update the Help dialog** (2026-09-04). It still describes the dialog as it
  was this morning: no per-row "Block native", no window sets, no "[Green]"
  caption/chip, no column header. Rewrite the Options and row sections and
  the flow steps to match.
- **Save / load** (2026-09-04). A named layout: windows, positions, sizes,
  tabs/URLs, and which user owns which; load it on start or on demand. Same
  item as the disconnect note below; this is the user-facing form.
- **Advanced options** (2026-09-04, plan 2026-09-05). Four commits, in order:
  1. logging becomes a runtime switch: the `MOUSEMUX_DEBUG` code stays
     compiled in, the four log sinks (controller log, diag log, controller
     trace, views trace) check one flag; default off, on via
     `--mousemux-log` or the option; anonymised keys stay compile-time;
  2. `mousemux-settings.json` next to the layout file, read at startup:
     hotkey, wheel step, logging, global block;
  3. an Advanced dialog from a footer button: the hotkey dropdown moves
     there, plus wheel step, logging, global block, port read-only;
  4. the wheel step (today a constant, 40 px per notch) read from the
     setting.
- **Screen sharing / hosted browsers** (2026-09-04, plan 2026-09-05).
  Goal: a host runs many of our Chrome windows that nobody sees locally; each
  remote person drives one from their own machine, as a normal user of the
  window.  Pixels first: structure streaming (Blimp-style) was considered
  and rejected for the same reasons Google dropped Blimp (pages are programs,
  fonts/subresources/session leak, pixel fallback needed anyway).
  What exists already - MouseMux Screen Matrix
  (`o:\GitHub\Gibster\mousemux-appseleasepps\webapp-screen-matrix`):
  peer-to-peer WebRTC (`js/p2p-room.js`, LiveKit-shaped API, signalling
  `wss://signal.mousemux.com/ws` with join-session, STUN at
  turn.mousemux.com, TURN credentials from the welcome message), session
  codes per shared source, window-level coordinate mapping, viewers as
  MouseMux virtual users with their own cursors.  LiveKit not needed.
  What only our Chrome can add, in order:
  1. a capture source per top-level surface (window, menu popup, bubble)
     straight from the compositor, visible or not, plus its screen rect,
     exposed to the page as a media track the Web SDK can publish - the OS
     window capture Screen Matrix uses gives black for hidden windows;
  2. windows that keep rendering while hidden/off-screen (Chrome throttles
     occluded windows; flag or placement, measure first);
  3. the host page: for each window a session code, the virtual user, and
     AssignWindow(name) so the code is the window is the user;
  4. the client side is Screen Matrix as it is; later our dialog can open
     the viewer with a code.
  Spike: one hidden host window, one capture track over P2P to a Screen
  Matrix viewer, driven through a virtual user.  Proves 1-3 end to end.

- **Tab-strip clicks from an injected mouse do nothing** (2026-09-04). Presses
  at the top of the window (tab titles, a tab's X) reach the window via the
  Chrome-UI path but no view handles them (`handled=0` in chrome-log.txt),
  while the "..." button below them and the page work. Measure where the
  event stops (the frame's non-client hit test is the suspect: that area is
  HTCAPTION/HTCLIENT-tab for a real mouse and goes through
  HWNDMessageHandler, which the custom-message path skips).
- **Route wheel and mouse through Chrome's input router** instead of straight
  into the main frame's host, so cross-process frames receive them (the
  frame theory for the customer's wheel). Decide after the customer's next
  log with ACK lines: "no consumer" on a page with a scroller under the
  pointer would confirm it.

- **Save/load a layout** (2026-09-04). Save the windows, their positions, and
  which user owns which, and restore that on start or after a MouseMux
  disconnect. Today a lost connection releases every user and everyone has
  to click to claim again. The disconnect itself is left as it is until this
  exists.
- **`"debug": true` in app.json for `--tag debug` packages**, if MouseMux
  reads that flag. The packager writes `false` for every build.
- **New screenshot** of the current dialog (rows read "Window N · title").
  The packaged one is from 2.2.59.
- **Remove `MOUSEMUX_MENU_TRACE`** from `ui/views/controls/menu/menu_controller.cc`
  before a release build; it was a one-off diagnostic (selection-change stack
  traces).
- **Menus closing on activation change.** A menu cancels itself when any
  window's activation changes, so user B activating a window closes user A's
  menu. Could be suppressed while MouseMux drives. Deliberately left as is
  for now (2026-09-04).
