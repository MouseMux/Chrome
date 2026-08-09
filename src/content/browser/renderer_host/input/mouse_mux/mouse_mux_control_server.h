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

  raw_ptr<MouseMuxInputController> controller_;  // Not owned, singleton.
  std::unique_ptr<base::Thread> io_thread_;
  std::unique_ptr<ServerDelegate> delegate_;

  base::WeakPtrFactory<MouseMuxControlServer> weak_factory_{this};
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CONTROL_SERVER_H_
