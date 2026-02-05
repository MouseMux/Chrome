// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CLIENT_H_
#define CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/sequence_checker.h"
#include "content/common/content_export.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "url/gurl.h"

namespace content {

// WebSocket client for connecting to a MouseMux server.
// Receives mouse motion and button events and notifies observers.
class CONTENT_EXPORT MouseMuxClient
    : public network::mojom::WebSocketHandshakeClient,
      public network::mojom::WebSocketClient {
 public:
  // Debug logging callback type.
  using DebugLogCallback = base::RepeatingCallback<void(const std::string&)>;

  // User info from MouseMux server.
  struct UserInfo {
    int user_id = 0;
    int hwid_mouse = 0;
    int hwid_keyboard = 0;
    std::string name;
  };

  // Observer interface for receiving MouseMux events.
  class CONTENT_EXPORT Observer : public base::CheckedObserver {
   public:
    // Called when a motion event is received.
    // |hwid| is the hardware device ID.
    // |x|, |y| are physical screen coordinates in pixels.
    virtual void OnMouseMotion(int hwid, float x, float y) = 0;

    // Called when a button event is received.
    // |hwid| is the hardware device ID.
    // |x|, |y| are physical screen coordinates in pixels.
    // |data| is the button bitmask.
    virtual void OnMouseButton(int hwid, float x, float y, int data) = 0;

    // Called when a wheel event is received.
    // |hwid| is the hardware device ID.
    // |x|, |y| are physical screen coordinates in pixels.
    // |delta| is the wheel delta (positive = up/forward, negative = down/back).
    // |horizontal| is true for horizontal scroll, false for vertical.
    virtual void OnMouseWheel(int hwid, float x, float y, int delta, bool horizontal) = 0;

    // Called when connection state changes.
    virtual void OnConnectionStateChanged(bool connected) = 0;

    // Called when user list is received.
    virtual void OnUserList(const std::vector<UserInfo>& users) = 0;

    // Called when a new user joins.
    virtual void OnUserCreated(const UserInfo& user) = 0;

    // Called when a user leaves.
    virtual void OnUserDisposed(int hwid_mouse, int hwid_keyboard) = 0;

    // Called when a keyboard event is received.
    // |hwid| is the keyboard hardware device ID.
    // |vkey| is the Windows virtual key code.
    // |message| is the Windows message (0x100=WM_KEYDOWN, 0x101=WM_KEYUP).
    // |scan| is the scan code.
    // |flags| are additional flags.
    virtual void OnKeyboardKey(int hwid, int vkey, int message, int scan, int flags) = 0;

    // Called when server sends a timeout warning.
    // |minutes| is the number of minutes until timeout.
    virtual void OnTimeoutWarning(int minutes) = 0;

    // Called when server session has stopped due to timeout.
    // |reason| describes why the session ended.
    virtual void OnTimeoutStopped(const std::string& reason) = 0;

   protected:
    ~Observer() override = default;
  };

  MouseMuxClient();
  ~MouseMuxClient() override;

  MouseMuxClient(const MouseMuxClient&) = delete;
  MouseMuxClient& operator=(const MouseMuxClient&) = delete;

  // Observer management.
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Connection management.
  void Connect();
  void Disconnect();
  bool IsConnected() const { return state_ == State::kOpen; }

  // Send a message to the server.
  void SendMessage(const std::string& json_message);

  // Request user list from server.
  void RequestUserList();

  // Capture a pointer device (prevents it from sending to Windows).
  void SendCaptureRequest(int hwid);

  // Release capture of a pointer device.
  void SendCaptureRelease(int hwid);

  // Set a callback for debug logging.
  void SetDebugLogCallback(DebugLogCallback callback);

  // network::mojom::WebSocketHandshakeClient:
  void OnOpeningHandshakeStarted(
      network::mojom::WebSocketHandshakeRequestPtr request) override;
  void OnFailure(const std::string& message,
                 int net_error,
                 int response_code) override;
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::WebSocket> socket,
      mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
      network::mojom::WebSocketHandshakeResponsePtr response,
      mojo::ScopedDataPipeConsumerHandle readable,
      mojo::ScopedDataPipeProducerHandle writable) override;

  // network::mojom::WebSocketClient:
  void OnDataFrame(bool finish,
                   network::mojom::WebSocketMessageType type,
                   uint64_t data_len) override;
  void OnDropChannel(bool was_clean,
                     uint16_t code,
                     const std::string& reason) override;
  void OnClosingHandshake() override;

 private:
  enum class State {
    kInitialized,
    kConnecting,
    kOpen,
    kDisconnected,
  };

  void ReadFromDataPipe(MojoResult, const mojo::HandleSignalsState&);
  void ProcessCompletedMessage();
  void OnMojoPipeDisconnect();
  void ClosePipe();
  void ParseAndDispatchMessage(const std::vector<uint8_t>& data);
  void LogDebug(const std::string& message);

  State state_ = State::kInitialized;
  const GURL service_url_;
  base::ObserverList<Observer> observers_;

  // Buffer for incoming message data.
  std::vector<uint8_t> pending_read_data_;
  size_t pending_read_data_index_ = 0;
  bool pending_read_finished_ = false;

  mojo::Receiver<network::mojom::WebSocketHandshakeClient> handshake_receiver_{
      this};
  mojo::Receiver<network::mojom::WebSocketClient> client_receiver_{this};
  mojo::Remote<network::mojom::WebSocket> websocket_;
  mojo::ScopedDataPipeConsumerHandle readable_;
  mojo::ScopedDataPipeProducerHandle writable_;
  mojo::SimpleWatcher readable_watcher_;

  // Debug logging callback.
  DebugLogCallback debug_log_callback_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_MOUSE_MUX_MOUSE_MUX_CLIENT_H_
