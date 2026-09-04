# TODO

Open items, newest first. Each says why it is open and what would close it.

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
- **Remove the "Keep each user in their own window" checkbox.** Since Build
  #62 every user is always kept in their own window(s); the checkbox does
  nothing.
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
