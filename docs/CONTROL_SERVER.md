# MouseMux Control Server

Local WebSocket server for external automation of MouseMux state.

## Launch

```
chrome.exe --enable-features=MouseMuxIntegration --mousemux-control-port=52000
```

The server binds to `127.0.0.1` only. Connect with any WebSocket client:

```
ws://127.0.0.1:52000
```

## Protocol

All messages are JSON text frames.

### Query Status

```json
â†’ {"type":"status"}
â† {"type":"status","owner":null,"connected":false,"blocked":false,"captured":false}
```

Response fields:

| Field       | Type          | Description |
|-------------|---------------|-------------|
| owner       | string/int/null | Current owner name, hwid, or null if unclaimed |
| connected   | bool          | Connected to MouseMux server (ws://localhost:41001) |
| blocked     | bool          | Native mouse/keyboard input blocked |
| captured    | bool          | Owner's mouse captured (exclusive input) |
| visible     | bool          | Control dialog window is shown |

### Set State

```json
â†’ {"type":"set","owner":"user1:mouse1","connected":true,"blocked":true,"captured":true}
â† {"type":"ok"}
```

All fields are optional â€” include only what you want to change.

| Field       | Type          | Effect |
|-------------|---------------|--------|
| connected   | bool          | true: connect to MouseMux server. false: disconnect |
| owner       | string/int/null | string: set owner by name. int > 0: set by hwid. null/0/"": release owner (also releases capture) |
| blocked     | bool          | true: block native input. false: unblock |
| captured    | bool          | true: capture owner's mouse. false: release capture |
| visible     | bool          | true: show the control dialog. false: hide it entirely |

### Hiding vs collapsing

`visible:false` removes the dialog window completely and can only be undone
from here — there is nothing left on screen to click. It is meant for kiosk
and automation setups where no control UI should appear.

The **Collapse** button in the dialog is a different thing: it shrinks the
dialog to a small icon-plus-Expand strip that stays on screen, so a person can
always restore it without any tooling. Nothing a user can click leads to the
fully hidden state.

Fields are applied in order: connected â†’ owner â†’ blocked â†’ captured.

### Error

```json
â† {"type":"error","msg":"invalid json"}
```

## Typical Flows

### Claim a browser for a user

```json
â†’ {"type":"status"}
â† {"type":"status","owner":null,...}

â†’ {"type":"set","connected":true,"owner":"user1:mouse1","blocked":true}
â† {"type":"ok"}
```

### Release a browser

```json
â†’ {"type":"set","owner":null,"connected":false,"blocked":false,"captured":false}
â† {"type":"ok"}
```

### Full capture mode

```json
â†’ {"type":"set","connected":true,"owner":"user1:mouse1","blocked":true,"captured":true}
â† {"type":"ok"}
```

## Notes

- The owner name must match a user known to the MouseMux server (appears in the user list after connection).
- Setting owner before connecting will set the hwid but the server won't know about it until connected.
- Recommended order: connect first, wait briefly for user list, then set owner.
- Multiple WebSocket clients can connect simultaneously.
- The dialog UI toggles sync automatically when state is changed via the control server.
