# MouseMux Chrome Integration - Code Changes

This document details all modifications made to Chromium source files. For new files (the `mouse_mux/` directories), simply copy them into your Chromium checkout. For modified files, apply the changes described below.

**Base Chromium Version**: 151.0.7922.77
**Patch Version**: 2.2.57

> **The authoritative file list is [`docs/CHROMIUM_VERSION.md`](docs/CHROMIUM_VERSION.md).**
> It splits the patch into files that are new (safe to copy wholesale) and
> upstream files carrying an edit (must be re-applied by hand on a Chromium
> bump), and says what each edit does. Sections further down this document
> date from 2.2.46 and no longer list everything.
>
> Changelog for this version: [`docs/UPDATE-v18.txt`](docs/UPDATE-v18.txt).
> Previous version (2.2.56, same Chromium): [`docs/UPDATE-v17.txt`](docs/UPDATE-v17.txt).

---

## New Files (Copy Directly)

These directories contain entirely new files - copy them into your Chromium source tree:

```
src/content/browser/renderer_host/input/mouse_mux/
  mouse_mux_client.cc
  mouse_mux_client.h
  mouse_mux_config.h            <- compile-time switches
  mouse_mux_control_server.cc
  mouse_mux_control_server.h
  mouse_mux_input_controller.cc
  mouse_mux_input_controller.h

src/chrome/browser/ui/views/mouse_mux/
  mouse_mux_control_dialog.cc
  mouse_mux_control_dialog.h
```

`mouse_mux_config.h` holds every compile-time switch. Read the convention at
the top of it before flipping anything: `MOUSEMUX_EXPERIMENT_*` is off by
definition, and `MOUSEMUX_DEBUG*` must stay off in any build that leaves the
build machine.

`launcher/launcher.c` is **no longer built or shipped** as of 2.2.57. The
browser starts its own seats: it claims a seat mutex at startup, keyed off
`--mousemux-control-port` (52000+N is seat N), and holds it until the process
exits. The launcher existed only to hold that mutex on behalf of a browser it
could not modify. It is kept in this repo as reference — its comments record
why several earlier seat-tracking schemes were abandoned — but nothing uses it.

Also copy `icon.ico` to the directory where chrome.exe will be located.

---

## Modified Chromium Files

### 1. `src/content/common/features.h`

**Location**: Near other Windows-specific feature declarations (around line 160)

**Add**:
```cpp
#if BUILDFLAG(IS_WIN)
CONTENT_EXPORT BASE_DECLARE_FEATURE(kMouseMuxIntegration);
#endif
```

---

### 2. `src/content/common/features.cc`

**Location**: End of file, before the closing namespace

**Add**:
```cpp
#if BUILDFLAG(IS_WIN)
// Enable MouseMux integration for WebSocket-based mouse input.
BASE_FEATURE(kMouseMuxIntegration,
             "MouseMuxIntegration",
             base::FEATURE_DISABLED_BY_DEFAULT);
#endif
```

---

### 3. `src/content/browser/renderer_host/render_widget_host_view_event_handler.h`

**Location 1**: In the public section, after other setter methods (around line 145)

**Add**:
```cpp
  // MouseMux integration: block native mouse input when enabled.
  void SetNativeMouseInputBlocked(bool blocked) {
    native_mouse_input_blocked_ = blocked;
  }
  bool IsNativeMouseInputBlocked() const { return native_mouse_input_blocked_; }

  // MouseMux integration: block native keyboard input when enabled.
  void SetNativeKeyboardInputBlocked(bool blocked) {
    native_keyboard_input_blocked_ = blocked;
  }
  bool IsNativeKeyboardInputBlocked() const { return native_keyboard_input_blocked_; }
```

**Location 2**: In the private section, with other member variables (end of class)

**Add**:
```cpp
  // MouseMux integration: when true, native mouse input is blocked.
  bool native_mouse_input_blocked_ = false;

  // MouseMux integration: when true, native keyboard input is blocked.
  bool native_keyboard_input_blocked_ = false;
```

---

### 4. `src/content/browser/renderer_host/render_widget_host_view_event_handler.cc`

**Location 1**: In `OnKeyEvent()`, at the start of the function (around line 240)

**Add**:
```cpp
  // MouseMux integration: block native keyboard input when enabled.
  if (native_keyboard_input_blocked_) {
    event->SetHandled();
    return;
  }
```

**Location 2**: In `OnMouseEvent()`, after the cursor hide check (around line 330)

**Add**:
```cpp
  // MouseMux integration: block native mouse input when enabled.
  // Preserve non-client events (window move/resize) by checking EF_IS_NON_CLIENT.
  if (native_mouse_input_blocked_ &&
      !(event->flags() & ui::EF_IS_NON_CLIENT)) {
    event->SetHandled();
    return;
  }
```

---

### 5. `src/content/browser/renderer_host/render_widget_host_view_aura.cc`

**Location 1**: In the Windows includes section (around line 115)

**Add**:
```cpp
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "content/common/features.h"
```

**Location 2**: In the **constructor**, inside the existing `#if BUILDFLAG(IS_WIN)`
block after `UpdateArabicIndicDigitInputStateIfNecessary()` (around line 400)

**Add**:
```cpp
  // Register with MouseMux controller for event injection.
  if (base::FeatureList::IsEnabled(features::kMouseMuxIntegration)) {
    MouseMuxInputController::GetInstance()->RegisterView(this);
  }
```

> Earlier revisions of this document said `InitAsChild()`. That was wrong, and
> the difference matters: `InitAsChild()` runs only for ordinary web content
> views, whereas the constructor runs for **every** `RenderWidgetHostViewAura`
> — including the popup views created by `InitAsPopup()` for `<select>`
> dropdowns. Registering in the constructor is what puts those popups in
> `registered_views_`, so `FindViewAtPoint()` can hit-test them and injected
> clicks reach dropdown items. Moving this to `InitAsChild()` would silently
> break clicking on `<select>` dropdowns.

**Location 3**: In destructor, before `text_input_manager_` cleanup (around line 2937)

**Add**:
```cpp
  // Unregister from MouseMux controller.
  if (base::FeatureList::IsEnabled(features::kMouseMuxIntegration)) {
    MouseMuxInputController::GetInstance()->UnregisterView(this);
  }
```

---

### 6. `src/chrome/browser/chrome_browser_main.cc`

**Location 1**: In Windows includes section (around line 240)

**Add**:
```cpp
#include "chrome/browser/ui/views/mouse_mux/mouse_mux_control_dialog.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "content/common/features.h"
```

**Location 2**: In `PreMainMessageLoopRunImpl()`, after `OfflinePageInfoHandler::Register()` (around line 1955)

**Add**:
```cpp
#if BUILDFLAG(IS_WIN)
  // MouseMux integration: show control dialog when enabled.
  // Dialog stays open for runtime control - doesn't block startup.
  if (base::FeatureList::IsEnabled(features::kMouseMuxIntegration)) {
    mouse_mux::MouseMuxControlDialog::Show();
  }
#endif
```

---

### 7. `src/content/browser/BUILD.gn`

**Location**: In the `if (is_win)` block (around line 3570)

**Add**:
```gn
    # MouseMux integration for WebSocket-based mouse input.
    sources += [
      "renderer_host/input/mouse_mux/mouse_mux_client.cc",
      "renderer_host/input/mouse_mux/mouse_mux_client.h",
      "renderer_host/input/mouse_mux/mouse_mux_input_controller.cc",
      "renderer_host/input/mouse_mux/mouse_mux_input_controller.h",
    ]
```

---

### 8. `src/chrome/browser/ui/BUILD.gn`

**Location**: In the Windows sources section (around line 4995)

**Add**:
```gn
        "views/mouse_mux/mouse_mux_control_dialog.cc",
        "views/mouse_mux/mouse_mux_control_dialog.h",
```

---

## Windows Build Fixes (Optional)

These are fixes for building on Windows outside of depot_tools environment:

### `src/tools/protoc_wrapper/protoc_wrapper.py`

**Line 211**: Change `os.path.relpath` to `os.path.abspath`:

```python
# Before:
"--plugin", "protoc-gen-plugin=" + os.path.relpath(options.plugin),

# After:
"--plugin", "protoc-gen-plugin=" + os.path.abspath(options.plugin),
```

### `src/tools/protoc_wrapper/protoc-gen-ts_proto.bat`

Replace `python3` with absolute path to your Python installation.

### `src/third_party/dom_distiller_js/protoc_plugins/json_values_converter.bat`

Replace `python3` with absolute path to your Python installation.

---

## Build Configuration

Create `out/Release/args.gn`:

```gn
is_component_build = false
is_debug = false
is_official_build = false
symbol_level = 0
dcheck_always_on = false
use_siso = false
```

## Build Command

```bash
ninja -C out/Release chrome
```

## Run Command

```bash
chrome.exe --enable-features=MouseMuxIntegration
```
