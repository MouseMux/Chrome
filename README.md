# MouseMux Chrome Integration - Patch 2.2.46

This patch adds MouseMux integration to Chromium for multi-seat browser setups.
Each Chrome instance can be locked to a specific user's mouse and keyboard,
allowing multiple users to browse independently on the same Windows machine.

## Version Info
- **Patch Version**: 2.2.46
- **Date**: 2026-02-05
- **Base Chromium**: 146.0.7650.0

## Features
- WebSocket connection to MouseMux
- Mouse motion and button event injection
- Keyboard event injection
- Per-user device ownership and locking
- Native input blocking for web content
- Multi-tab support (events route to visible/active tab only)
- Mouse capture with configurable release hotkey

## Patch Contents

### New Files (MouseMux Implementation)
```
src/content/browser/renderer_host/input/mouse_mux/
├── mouse_mux_client.cc          # WebSocket client
├── mouse_mux_client.h
├── mouse_mux_input_controller.cc # Input event injection
└── mouse_mux_input_controller.h

src/chrome/browser/ui/views/mouse_mux/
├── mouse_mux_control_dialog.cc   # Control UI dialog
└── mouse_mux_control_dialog.h
```

### Modified Files (Chromium Integration)
```
src/content/common/features.cc    # Feature flag definition
src/content/common/features.h     # Feature flag declaration
src/content/browser/renderer_host/render_widget_host_view_aura.cc  # View registration
src/content/browser/renderer_host/render_widget_host_view_event_handler.cc  # Input blocking
src/content/browser/renderer_host/render_widget_host_view_event_handler.h
src/chrome/browser/chrome_browser_main.cc  # Dialog launch
src/content/browser/BUILD.gn      # Build rules
src/chrome/browser/ui/BUILD.gn    # Build rules
```

### Windows Build Fixes
```
src/tools/protoc_wrapper/protoc_wrapper.py           # os.path.abspath fix
src/tools/protoc_wrapper/protoc-gen-ts_proto.bat     # python3 path fix
src/third_party/dom_distiller_js/protoc_plugins/json_values_converter.bat  # python3 path fix
```

## Quick Start

See [APPLY_PATCH.md](APPLY_PATCH.md) for detailed instructions.

## Documentation

- [CHANGES.md](CHANGES.md) - Detailed code changes for each modified Chromium file
- [APPLY_PATCH.md](APPLY_PATCH.md) - Step-by-step patch application guide
- [docs/BUILD_NOTES.md](docs/BUILD_NOTES.md) - Build notes and current status
