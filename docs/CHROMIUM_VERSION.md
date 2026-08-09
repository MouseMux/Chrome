# Chromium Base Version and Upgrade Guide

**This patch set is built against Chromium `146.0.7650.0`.**

From `chrome/VERSION`:

```
MAJOR=146
MINOR=0
BUILD=7650
PATCH=0
```

Read this before pulling a newer Chromium. The cost of an upgrade is not
spread evenly across these files — most of them cannot conflict at all, and a
short list can.

## Paths

| what | where |
|---|---|
| Chromium source | `O:\mousemux-builds\Chrome\chrome\src` |
| Build output | `chrome\src\out\Release` |
| MouseMux source | `content\browser\renderer_host\input\mouse_mux\` |
| Build script | `O:\mousemux-builds\Chrome\fix_build_env.py` |
| Launcher source | `O:\mousemux-builds\Chrome\launcher\` |
| Python | `chrome\depot_tools\bootstrap-2@3_11_8_chromium_35_bin\python3\bin\python3.exe` |

## The two classes of file

Files in this folder are flattened. `FILES.txt` maps each back to its place in
the tree. For an upgrade, what matters is which class it belongs to.

### New files — drop in, cannot conflict

These exist only because of MouseMux. Nothing upstream touches them, so on a
Chromium bump they copy across unchanged. They may still need *fixing* if an
API they call changes, but they will never produce a merge conflict.

| file | destination |
|---|---|
| `mouse_mux_config.h` | `content/browser/renderer_host/input/mouse_mux/` |
| `mouse_mux_client.h` / `.cc` | `content/browser/renderer_host/input/mouse_mux/` |
| `mouse_mux_input_controller.h` / `.cc` | `content/browser/renderer_host/input/mouse_mux/` |
| `mouse_mux_control_server.h` / `.cc` | `content/browser/renderer_host/input/mouse_mux/` |
| `mouse_mux_control_dialog.h` / `.cc` | `chrome/browser/ui/views/mouse_mux/` |

`launcher.c` and `build_launcher.py` are not part of the Chromium build at all
and are entirely unaffected by a version bump.

### Modified upstream files — re-apply by hand

**These are the upgrade risk.** Each is an upstream Chromium file carrying a
MouseMux edit. Do not copy these over a newer Chromium wholesale — you would
revert upstream's changes to them. Diff and re-apply the described edit
instead. Every edit is marked with a `MouseMux` comment, so
`grep -n -i mousemux <file>` finds them all.

| file | destination | what the edit does |
|---|---|---|
| `features.h` / `features.cc` | `content/common/` | Declares and defines the `kMouseMuxIntegration` feature flag. |
| `render_widget_host_view_aura.cc` | `content/browser/renderer_host/` | Registers/unregisters the view with the controller when the feature is on; drops native `WM_CHAR` in `InsertChar` while keyboard input is blocked, which is what prevents doubled characters. |
| `render_widget_host_view_event_handler.h` / `.cc` | `content/browser/renderer_host/` | Adds `native_mouse_input_blocked_` / `native_keyboard_input_blocked_` and their setters; drops native mouse events, and (only with `MOUSEMUX_EXPERIMENT_PEN_TOUCH_BLOCK`) `ui::TouchEvent`. |
| `render_widget_host_impl.h` / `.cc` | `content/browser/renderer_host/` | Adds `ResetInputRouterForInjection()`, which clears stuck InputRouter pending-ack state so injected events keep flowing. |
| `desktop_window_tree_host_win.cc` | `ui/views/widget/desktop_aura/` | The largest edit. Re-declares the MouseMux defines (ui/views cannot include content/), defines the `content::g_mousemux_*` globals, and handles the `WM_MOUSEMUX_*` custom messages plus native blocking in `PreHandleMSG`. |
| `dialog_delegate.h` | `ui/views/window/` | Forward-declares `mouse_mux::MouseMuxControlDialog` and friends it, so the dialog can reach protected members. Small but easy to lose. |
| `chrome_browser_main_extra_parts_views.cc` | `chrome/browser/ui/views/` | Calls `MouseMuxControlDialog::Show()` from `PostBrowserStart()` when the feature is enabled. |
| `content_browser_BUILD.gn` | `content/browser/BUILD.gn` | Adds the four `mouse_mux/` sources to the content browser target. |
| `chrome_browser_ui_BUILD.gn` | `chrome/browser/ui/BUILD.gn` | Adds the dialog sources to the chrome UI target. |

The two `BUILD.gn` files change often upstream. Expect to re-add the source
entries by hand rather than copying the file.

## The layering constraint that shapes all of this

`content/` cannot depend on `ui/views` — it is a layering violation the build
enforces. Two consequences survive any Chromium version:

1. The dialog is reached from the content layer by **callback**, never by
   direct call.
2. `desktop_window_tree_host_win.cc` **duplicates** the MouseMux defines and
   the `WM_MOUSEMUX_*` message numbers rather than including
   `mouse_mux_config.h`. Those two lists must be kept in sync by hand. A
   define used there but missing from its list is silently dead — see the
   `MOUSEMUX_EXPERIMENT_NATIVE_BLOCK_HARD` note in `UPDATE.txt` section 4.

## Rebuilding

```
PYTHON fix_build_env.py --build      # normal incremental build
PYTHON fix_build_env.py              # fix environment only
PYTHON fix_build_env.py --check      # verify environment
```

Where `PYTHON` is the depot_tools python above. Build the launcher separately
with `python build_launcher.py` from `launcher\`.

**Close every Chrome from this build first** — `chrome.dll` is locked while
running and the link step fails with a permission error.

### Rules that are not negotiable

These exist for a reason; breaking them costs hours of rebuild time.

- **Never kill ninja.** It corrupts `.ninja_log` and forces a full rebuild of
  40,000+ files. If a build is running, wait.
- **Never delete `.ninja_log` or `.ninja_deps`.** Same effect.
- **Never run two ninja builds against the same `out/Release`.** Check first.
- **Never run ninja directly** — `fix_build_env.py` fixes SDK paths, checks for
  a running ninja, and puts python3 on PATH so gn can regenerate.
- **After a compile error, just fix the code and re-run the build script.**
  Do not clean, do not delete logs. Ninja resumes exactly where it stopped.
- After modifying `toolchain.ninja` or `environment.x64`, touch
  `build.ninja.stamp` and `build.ninja` so ninja does not trigger a gn
  regeneration. The fix script does this automatically.

## Verifying a release build

Diagnostics are compiled out, not merely switched off, so the binary itself is
the proof. All of these must return 0:

```sh
cd out/Release
for s in 'mousemux_diag\.log' 'mousemux_debug\.log' 'chrome-log\.txt' \
         'MouseMux trace build alive'; do
  echo "$s -> $(grep -ac "$s" chrome.dll)"
done
```

And as a sanity check that MouseMux is actually present, these must be non-zero:

```sh
grep -ac 'MouseMuxIntegration' chrome.dll
grep -ac '2\.2\.55' chrome.dll
```
