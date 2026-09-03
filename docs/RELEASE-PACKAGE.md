# Building the release package

The thing we ship is **`native-chrome-<version>-<date>.zip`**, and it is not
just a zip of the build directory. It is a MouseMux *app pack*: MouseMux's app browser reads
`app.json` out of it, shows `logo.jpg` and `icon.png`, and launches
`chrome.exe` with the command line the manifest gives. Get its shape wrong and
it does not appear in MouseMux at all.

Every release before 2.2.58 was assembled by hand, and no two were the same.
Don't assemble one by hand.

```
python github-repo/package/make-release.py --version 2.2.58
```

That produces `chrome-release-<version>-<date>/native-chrome/` and zips it as
`native-chrome-<version>-<date>.zip` beside it.

The FOLDER inside the zip must stay `native-chrome` — `app.json`'s
`"base": "native-chrome"` names it and the app browser reads the manifest from
there, so renaming the folder makes the pack invisible. The FILE name is free,
and carries the version because the reference package did not: two builds a
month apart were both `native-chrome.zip` and indistinguishable once emailed. The reference package it was derived from,
which is the last hand-built one, is
`C:\Users\dev\Desktop\MouseWithoutBorders\native-chrome.zip`.

**`package/` is deliberately not in git** (see `.gitignore`). It holds the
shipped artwork and the MouseMux manifest — product assets rather than source,
and several megabytes of them. It lives beside this checkout and is backed up
with the releases, not with the code. If you clone this repo somewhere fresh,
copy `package/` across or the packager will stop and tell you what is missing.

---

## Before running it

1. **Confirm the debug switches are OFF.** In
   `content/browser/renderer_host/input/mouse_mux/mouse_mux_config.h`, both
   `MOUSEMUX_DEBUG` and `MOUSEMUX_DEBUG_TRACE` must be commented out — and
   `MOUSEMUX_DEBUG_TRACE` again in
   `ui/views/widget/desktop_aura/desktop_window_tree_host_win.cc`, which keeps
   its own mirrored copy. With either on, **every keystroke is written to
   disk**. They were both switched on deliberately on 2026-09-03 to chase a
   bug, which is exactly how a release ships with them still on.

   ```
   grep -n "define MOUSEMUX_DEBUG" mouse_mux_config.h
   ```

   Then delete any logs left behind: `O:\tmp\mousemux_debug.log`,
   `O:\tmp\mousemux_diag.log`, `O:\chrome-log.txt`.

2. **Build, and let the link finish.** The package copies `chrome.dll`
   straight from `out/Release`, so a stale DLL ships silently. Close Chrome
   first or the link fails and you package the previous build.
3. **Check the build number.** `kBuildNumber` and `kBuildDate` in
   `mouse_mux_control_dialog.cc`, and `kClientVersion` in
   `mouse_mux_client.cc`, must all match the version you are packaging. The
   script warns when the dialog disagrees. This has been wrong before: 2.2.55
   and 2.2.56 both shipped reporting build #54.
4. **Retake the screenshot** if the dialog has changed —
   `github-repo/package/screenshot.png`. It is the only picture of the product
   a customer sees before running it, and a screenshot of the previous UI is
   worse than none.
5. **Update the README.** `github-repo/docs/RELEASE-README.md` is the source;
   the package gets two copies of it (see below).

---

## What goes in, and why

| | |
|---|---|
| Chrome runtime | Listed explicitly in `RUNTIME_FILES` in the script, not globbed. `out/Release` holds tens of thousands of build artefacts; a package built by sweeping a directory is one nobody can review. |
| `locales/en-US.pak` | **One** locale. The build produces every language Chromium has, plus a `.pak.info` beside each — 400+ files, 13 MB. The reference ships exactly one. Adding a language is a deliberate edit to `RUNTIME_LOCALES`. |
| `MEIPreload/` | Copied whole. |
| `app.json` | The MouseMux manifest. Source of truth is `github-repo/package/app.json`; the script updates `date`, `time`, `info.make` and `info.text` per release. |
| `icon.ico`, `icon.png`, `logo.jpg` | In `github-repo/package/`. **Nothing in the build produces these** — a packager that only looks at build output ships without them, and MouseMux then shows the app with no artwork. |
| `docs/screenshot.png` | Same: lives in `github-repo/package/`, copied into `docs/`. |
| `README.md` and `docs/README.md` | The same document twice, from `docs/RELEASE-README.md`. The root copy is what a customer finds on unzipping; the `docs/` copy sits beside the screenshot. They differ only in the logo's relative path — `logo.jpg` from the root, `../logo.jpg` from `docs/`. |
| `CONTROL_SERVER.md` | From `github-repo/docs/`. |
| `start-mousemux-chrome.bat` | From the repo root. A convenience shortcut only; it takes no part in seat tracking. |

## What is deliberately left out

- **`user-data/`** — a profile from *our* testing. Shipping it hands the
  customer our history and cookies. The browser makes its own on first run.
- **`First Run`** — same reason; `--no-first-run` in the `.bat` covers it.
- **`.pdb` files** — hundreds of megabytes of debug symbols, no use to anyone
  receiving the zip.

Around 176 MB and 42 files when it is right.

---

## `app.json`: two things to remember

Neither is something to fix by guessing, and both are worth putting to
whoever owns the MouseMux side.

**`mode.hint` says `switched`, and that is correct.** Verified on hardware on
2026-09-03: two users typing simultaneously in two windows, in Switched mode
with multi-keyboard on. The name is misleading if you reason from it rather
than testing it - "switched" does not mean one user at a time here - and this
document previously claimed the hint was wrong on exactly that reasoning.

What is NOT optional is **multi keyboard**. Without it there is no low-level
keyboard hook, so keystrokes carry no per-device identity and arrive as
ordinary focus-driven Windows input. Nothing downstream can separate them.

**`make` tracks the MouseMux version**, and is set by hand. It sat at `3.0.11`
against a 3.0.17 server until 2.2.58, when it was corrected to `3.0.21`.
Nothing detects this, so bump it when MouseMux is released - otherwise the
pack claims to have been built for a version nobody is running.

Separately, our login advertises `sdkVersion` `2.2.35` while the server reports
protocol `2.2.45`. Whether that matters is a question for the MouseMux side.
