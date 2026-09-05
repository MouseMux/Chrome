# TODO

Open items, newest first. Each says why it is open and what would close it.

- **Update the Help dialog** (2026-09-04). It still describes the dialog as it
  was that morning: no per-row "Block native", no window sets, no "[Green]"
  caption/chip, no column header, no Save/Load layout. The "Why
  Multi-keyboard is required" section is in (2026-09-05). Rewrite the
  Options and row sections and the flow steps to match.
- **Save / load, the rest** (2026-09-04; first cut shipped 2026-09-05 as
  entries 60-65: Save layout and Load layout buttons, one file next to the
  profile, owners assigned by MouseMux name). Still open: several tabs per
  window (today the active tab's URL only), named layouts, load on start,
  and **reconnect memory**: when the connection drops every user is
  released and has to click again; on the first user list after a reconnect
  the last owner of each window could be re-assigned by name with
  `AssignWindow`. One list, filled at disconnect, used once.
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
  (`o:\GitHub\Gibster\mousemux-apps\release\apps\webapp-screen-matrix`):
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
- **Mouse capture knows about users** (2026-09-05). Another user's click on
  Chrome UI (a button, a tab; not a page) takes mouse capture, Windows has
  one capture per thread, and the open menu of the first user closes.
  Documented limitation (UPDATE-v21 entry 57). Fix means an injected UI
  press not taking Win32 capture while another window's menu holds it.
- **Tab × glyph blink** (2026-09-05). Seen once: the × on an inactive tab
  blinked under an injected pointer, then stopped. The glyph is drawn while
  the tab believes it is hovered, so it was the tab's hover state toggling.
  If it recurs: trace Tab::OnMouseEntered/Exited and whether the hover card
  was up.

- **Route wheel and mouse through Chrome's input router** instead of straight
  into the main frame's host, so cross-process frames receive them (the
  frame theory for the customer's wheel). Decide after the customer's next
  log with ACK lines: "no consumer" on a page with a scroller under the
  pointer would confirm it.
- **`"debug": true` in app.json for `--tag debug` packages**, if MouseMux
  reads that flag. The packager writes `false` for every build.
- **New screenshot** of the current dialog (rows read "Window N · title",
  footer has Save layout / Load layout). The packaged one is from 2.2.59.

Done since this list started, removed from it: tab-strip clicks from an
injected mouse (work since the WindowFromPoint routing, verified
2026-09-05); `MOUSEMUX_MENU_TRACE` (removed, entry 49); menus closing on
activation change (fixed 2026-09-04; what remains is the capture item
above).
