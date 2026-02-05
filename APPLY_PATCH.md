# How to Apply MouseMux Patch to Chromium

## Prerequisites

1. Fresh Chromium checkout at version 146.0.7650.0 (or compatible)
2. Working Chromium build environment (VS 2022, Windows SDK, etc.)
3. Python 3.x in your PATH (or MSYS2 with mingw64)

## Step 1: Copy New MouseMux Files

Copy the entire `mouse_mux` directories to your Chromium source:

```bash
# From patch directory:
cp -r src/content/browser/renderer_host/input/mouse_mux \
      <chromium>/src/content/browser/renderer_host/input/

cp -r src/chrome/browser/ui/views/mouse_mux \
      <chromium>/src/chrome/browser/ui/views/
```

## Step 2: Merge Modified Files

These files need changes merged (not replaced entirely):

### src/content/common/features.h
Add near other feature declarations:
```cpp
CONTENT_EXPORT BASE_DECLARE_FEATURE(kMouseMuxIntegration);
```

### src/content/common/features.cc
Add near other feature definitions:
```cpp
BASE_FEATURE(kMouseMuxIntegration,
             "MouseMuxIntegration",
             base::FEATURE_DISABLED_BY_DEFAULT);
```

### src/content/browser/BUILD.gn
Add to the `browser` source_set's `sources`:
```gn
"renderer_host/input/mouse_mux/mouse_mux_client.cc",
"renderer_host/input/mouse_mux/mouse_mux_client.h",
"renderer_host/input/mouse_mux/mouse_mux_input_controller.cc",
"renderer_host/input/mouse_mux/mouse_mux_input_controller.h",
```

### src/chrome/browser/ui/BUILD.gn
Add to the `ui` source_set's `sources`:
```gn
"views/mouse_mux/mouse_mux_control_dialog.cc",
"views/mouse_mux/mouse_mux_control_dialog.h",
```

### src/content/browser/renderer_host/render_widget_host_view_aura.cc
This file has the integration hooks. Key changes:
1. Include the MouseMux headers at the top
2. In constructor: Initialize MouseMux controller if feature enabled
3. In destructor: Clean up MouseMux registration
4. Feature check: `base::FeatureList::IsEnabled(features::kMouseMuxIntegration)`

See the provided copy for exact changes needed.

## Step 3: Apply Windows Build Fixes (Windows Only)

If building on Windows, apply these fixes to avoid proto plugin errors:

### src/tools/protoc_wrapper/protoc_wrapper.py (line ~211)
Change `os.path.relpath` to `os.path.abspath`:
```python
"--plugin", "protoc-gen-plugin=" + os.path.abspath(options.plugin),
```

### src/tools/protoc_wrapper/protoc-gen-ts_proto.bat
Replace `python3` with full path:
```batch
O:\devtools\shell\msys64\mingw64\bin\python3.exe "%~dp0protoc-gen-ts_proto.py" %*
```
(Adjust path to your Python installation)

### src/third_party/dom_distiller_js/protoc_plugins/json_values_converter.bat
Same fix - replace `python3` with full path.

## Step 4: Build

```bash
# Generate build files (first time only)
gn gen out/Release --args='is_component_build=false is_debug=false symbol_level=0'

# Build Chrome
ninja -C out/Release chrome -j 30
```

## Step 5: Run

```bash
chrome.exe --enable-features=MouseMuxIntegration
```

## Troubleshooting

### "python3 not recognized"
Apply the Windows build fixes in Step 3.

### Proto plugin path errors (../../ not recognized)
Apply the protoc_wrapper.py fix in Step 3.

### Ninja rebuilds everything
Never interrupt ninja builds. If state is corrupted, let the full rebuild complete.

### Clicks only work on first tab
Make sure you have the IsShowing() fix in mouse_mux_input_controller.cc's FindViewAtPoint().
