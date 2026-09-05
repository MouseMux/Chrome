// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONTROL_SERVER_H_
#define CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONTROL_SERVER_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/threading/thread.h"
#include "content/common/content_export.h"
#include "net/server/http_server.h"

namespace content {

class MouseMuxInputController;

// Simple WebSocket server for external automation of MouseMux state.
// Listens on a configurable port (--mousemux-control-port) and accepts
// JSON messages to query/set owner, connection, capture, and blocking state.
//
// Plain HTTP GETs serve files from <directory of chrome.exe>/mousemux-web
// ("/" is host.html): the pages of the hosted-browser feature, host.html and
// view.html, so both ends need nothing but this Chrome.
//
// Protocol:
//   → {"type":"status"}
//   ← {"type":"status","owner":"user1:mouse1","connected":true,
//      "blocked":true,"captured":true}
//
//   → {"type":"set","owner":"user1:mouse1","connected":true,
//      "blocked":true,"captured":true}
//   ← {"type":"ok"}
//
//   → {"type":"set","owner":null,"connected":false,"blocked":false,
//      "captured":false}
//   ← {"type":"ok"}
class CONTENT_EXPORT MouseMuxControlServer {
 public:
  explicit MouseMuxControlServer(MouseMuxInputController* controller);
  ~MouseMuxControlServer();

  MouseMuxControlServer(const MouseMuxControlServer&) = delete;
  MouseMuxControlServer& operator=(const MouseMuxControlServer&) = delete;

  // Starts the server on the given port. Returns true if started.
  bool Start(uint16_t port);

  // Stops the server and shuts down the IO thread.
  void Stop();

 private:
  // Delegate that runs on the IO thread and forwards to UI thread.
  class ServerDelegate;

  // Called on the UI thread when a WebSocket message arrives.
  void OnMessage(int connection_id, std::string data);

  // Sends a JSON string back to a client (posts to IO thread).
  void SendResponse(int connection_id, const std::string& json);

  // Handles "status" request.
  void HandleStatus(int connection_id);

  // Handles "set" request.
  void HandleSet(int connection_id, const std::string& data);

  // {"type":"capture","owner":"Green"|"hwnd":N,"page":"<url>"}: registers
  // the window's root as a desktop-capture source for the tab showing
  // <page>, and replies {"type":"capture","id":"...",x,y,width,height}.
  // The page then calls getUserMedia with chromeMediaSource "desktop" and
  // that id; Chromium serves it from the compositor, so the window need not
  // be visible on any screen.  See HandleCapture for what is checked.
  void HandleCapture(int connection_id, const std::string& data);

  // {"type":"sources"} -> {"type":"sources","windows":[{hwnd,title,owner,
  // x,y,width,height}...]}: every window with a page of ours in it.
  void HandleSources(int connection_id);
  // {"type":"rect","hwnd":N} -> {"type":"rect","hwnd",x,y,width,height}:
  // where the window is now, for mapping a viewer's normalized coordinates.
  void HandleRect(int connection_id, const std::string& data);
  // {"type":"assign","hwnd":N,"hwid":M} -> {"type":"ok"}: the window becomes
  // that device's, as if it had clicked in it (AssignWindowToHwid).
  void HandleAssign(int connection_id, const std::string& data);

  raw_ptr<MouseMuxInputController> controller_;  // Not owned, singleton.
  std::unique_ptr<base::Thread> io_thread_;
  std::unique_ptr<ServerDelegate> delegate_;

  base::WeakPtrFactory<MouseMuxControlServer> weak_factory_{this};
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONTROL_SERVER_H_
