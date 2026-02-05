# MouseMux Chrome Integration

## Current Status (2026-01-28)

### Working
- WebSocket connection to MouseMux server (ws://localhost:41001)
- Mouse motion events received and injected
- Button events with ownership tracking
- Native input blocking for web content
- Keyboard events (basic)
- **Multi-tab support** - clicks now work across multiple tabs (IsShowing() fix)

### Key Files
- `src/content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.cc` - Main controller
- `src/content/browser/renderer_host/input/mouse_mux/mouse_mux_client.cc` - WebSocket client
- `src/chrome/browser/ui/views/mouse_mux/mouse_mux_control_dialog.cc` - UI dialog

---

# Build Notes

## Build Configurations

- **Debug (component)**: `out/Default` - faster incremental builds, multiple DLLs
- **Release (monolithic)**: `out/Release` - single chrome.dll (~287MB), slower builds

## Critical: Never Interrupt Ninja Builds

**Problem**: Killing/interrupting ninja mid-build corrupts `.ninja_log`, causing ninja to lose track of completed steps. This results in massive unnecessary rebuilds (36k+ files instead of just changed files).

**Solution**:
- Always let builds run to completion
- If you must stop, use `Ctrl+C` in the terminal (graceful stop) rather than killing the process
- If ninja state gets corrupted, the only fix is to let the full rebuild complete

## Build Commands

```bash
# Use ninja directly (not autoninja which needs depot_tools initialization)
O:/scratch/chrome/src/third_party/ninja/ninja.exe -C O:/scratch/chrome/src/out/Release chrome -j 30

# For incremental builds after code changes, same command - ninja handles deps
```

## Windows-Specific Issues Fixed

### 1. Proto Plugin Python Path (FIXED)

**Files modified**:
- `tools/protoc_wrapper/protoc-gen-ts_proto.bat`
- `third_party/dom_distiller_js/protoc_plugins/json_values_converter.bat`

**Problem**: These `.bat` files called `python3` which isn't in the Windows cmd PATH when ninja runs outside MSYS2.

**Fix**: Changed to use absolute path:
```batch
O:\devtools\shell\msys64\mingw64\bin\python3.exe "%~dp0script.py" %*
```

### 2. Proto Plugin Relative Path (FIXED)

**File**: `tools/protoc_wrapper/protoc_wrapper.py` line 211

**Problem**: Used `os.path.relpath()` which created `../../` paths that Windows cmd couldn't execute.

**Fix**: Changed to `os.path.abspath()`:
```python
"--plugin", "protoc-gen-plugin=" + os.path.abspath(options.plugin),
```

## Release Build Setup

`out/Release/args.gn`:
```
is_component_build = false
is_debug = false
is_official_build = false
symbol_level = 0
dcheck_always_on = false
use_siso = false
```

## Release Folder

`O:/scratch/chrome-release-v7/` contains the standalone release build with all required DLLs:
- chrome.exe, chrome.dll
- VC++ runtime DLLs (vcruntime140.dll, msvcp140.dll, etc.)
- chrome_elf.dll
- Resource paks, locales, ICU data
- Graphics DLLs (libEGL, libGLESv2, vulkan, etc.)

## Running with MouseMux

```bash
chrome.exe --enable-features=MouseMuxIntegration
```

## Debug vs Release UI

In `mouse_mux_control_dialog.h`:
- Comment out `#define MOUSEMUX_DEBUG` for release (compact 320x200 dialog)
- Uncomment for debug (larger dialog with debug log textarea)
