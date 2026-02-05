// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_client.h"

#include <limits>
#include <utility>

#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/process/process_handle.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "net/storage_access_api/status.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/client_security_state.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace content {

namespace {

constexpr char kMouseMuxUrl[] = "ws://localhost:41001";
constexpr size_t kMaxIncomingMessageSize = 64 * 1024;  // 64KB max message

constexpr char kClientVersion[] = "2.2.46";
constexpr char kSdkVersion[] = "2.2.35";
constexpr char kBuildDate[] = "2026-02-05";

// MouseMux message types (M2A = server to app).
constexpr char kTypeMotion[] = "pointer.motion.notify.M2A";
constexpr char kTypeButton[] = "pointer.button.notify.M2A";
constexpr char kTypeWheel[] = "pointer.wheel.notify.M2A";
constexpr char kTypeUserList[] = "user.list.notify.M2A";
constexpr char kTypeUserCreate[] = "user.create.notify.M2A";
constexpr char kTypeUserDispose[] = "user.dispose.notify.M2A";
constexpr char kTypeUserChanged[] = "user.changed.notify.M2A";
constexpr char kTypeKeyboardKey[] = "keyboard.key.notify.M2A";
constexpr char kTypePing[] = "server.ping.notify.M2A";
constexpr char kTypeServerShutdown[] = "server.shutdown.notify.M2A";
constexpr char kTypeTimeoutWarning[] = "server.timeout.warning.notify.M2A";
constexpr char kTypeTimeoutStopped[] = "server.timeout.stopped.notify.M2A";

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("mouse_mux_client", R"(
        semantics {
          sender: "MouseMux Input Client"
          description:
            "Chrome connects to a local MouseMux server to receive mouse "
            "input events from external sources. This is used for advanced "
            "input multiplexing scenarios."
          trigger:
            "User enables MouseMux integration via the control dialog at "
            "Chrome startup when the kMouseMuxIntegration feature is enabled."
          user_data {
            type: NONE
          }
          data: "Mouse motion and button events (coordinates, button states)."
          internal {
            contacts {
                email: "nickelson@google.com"
            }
          }
          destination: LOCAL
          last_reviewed: "2024-01-01"
        }
        policy {
          cookies_allowed: NO
          setting: "This feature is controlled by the kMouseMuxIntegration "
            "feature flag and requires explicit user opt-in via the "
            "startup dialog."
        })");

}  // namespace

MouseMuxClient::MouseMuxClient()
    : service_url_(kMouseMuxUrl),
      readable_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL) {
  LogDebug("MouseMuxClient created");
}

MouseMuxClient::~MouseMuxClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LogDebug("MouseMuxClient destroyed");
}

void MouseMuxClient::SetDebugLogCallback(DebugLogCallback callback) {
  debug_log_callback_ = std::move(callback);
}

void MouseMuxClient::LogDebug(const std::string& message) {
  if (debug_log_callback_) {
    debug_log_callback_.Run(
        base::StringPrintf("[Client|PID:%d] %s",
                           static_cast<int>(base::GetCurrentProcId()),
                           message.c_str()));
  }
}

void MouseMuxClient::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void MouseMuxClient::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void MouseMuxClient::Connect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LogDebug("Connect() called, current state=" +
           std::to_string(static_cast<int>(state_)));

  if (state_ == State::kConnecting || state_ == State::kOpen) {
    LogDebug("Already connecting/connected, returning");
    return;
  }

  state_ = State::kConnecting;
  LogDebug("State set to kConnecting");

  auto handshake_remote = handshake_receiver_.BindNewPipeAndPassRemote();
  handshake_receiver_.set_disconnect_handler(base::BindOnce(
      &MouseMuxClient::OnMojoPipeDisconnect, base::Unretained(this)));
  LogDebug("Handshake receiver bound");

  auto* network_context = GetContentClient()->browser()->GetSystemNetworkContext();
  if (!network_context) {
    LogDebug("ERROR: No system network context available!");
    LOG(ERROR) << "MouseMux: No system network context available";
    ClosePipe();
    return;
  }
  LogDebug("Got network context, creating WebSocket to " + service_url_.spec());

  network_context->CreateWebSocket(
      service_url_,
      /*protocols=*/{},
      net::SiteForCookies(),
      net::StorageAccessApiStatus::kNone,
      net::IsolationInfo::CreateForInternalRequest(
          url::Origin::Create(service_url_)),
      /*additional_headers=*/{},
      network::mojom::kBrowserProcessId,
      url::Origin::Create(service_url_),
      network::mojom::ClientSecurityState::New(),
      network::mojom::kWebSocketOptionBlockAllCookies,
      net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation),
      std::move(handshake_remote),
      /*url_loader_network_observer=*/mojo::NullRemote(),
      /*auth_handler=*/mojo::NullRemote(),
      /*header_client=*/mojo::NullRemote(),
      /*throttling_profile_id=*/std::nullopt);
}

void MouseMuxClient::Disconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ClosePipe();
}

void MouseMuxClient::SendMessage(const std::string& json_message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen || !websocket_.is_bound()) {
    LogDebug("SendMessage: not connected, ignoring");
    return;
  }

  LogDebug("SendMessage: " + json_message);

  // Write to the data pipe.
  size_t actually_written = 0;
  std::vector<uint8_t> data(json_message.begin(), json_message.end());
  MojoResult result = writable_->WriteData(base::as_byte_span(data),
                                           MOJO_WRITE_DATA_FLAG_NONE,
                                           actually_written);
  if (result != MOJO_RESULT_OK) {
    LogDebug("SendMessage: WriteData failed: " + std::to_string(result));
    return;
  }

  // Tell the WebSocket to send the frame.
  websocket_->SendMessage(network::mojom::WebSocketMessageType::TEXT,
                          data.size());
}

void MouseMuxClient::RequestUserList() {
  SendMessage(R"({"type":"user.list.request.A2M"})");
}

void MouseMuxClient::SendCaptureRequest(int hwid) {
  SendMessage(base::StringPrintf(
      R"({"type":"pointer.capture.request.A2M","hwid":%d})", hwid));
  LogDebug(base::StringPrintf("Sent capture request for hwid=0x%x", hwid));
}

void MouseMuxClient::SendCaptureRelease(int hwid) {
  SendMessage(base::StringPrintf(
      R"({"type":"pointer.capture.release.request.A2M","hwid":%d})", hwid));
  LogDebug(base::StringPrintf("Sent capture release for hwid=0x%x", hwid));
}

void MouseMuxClient::OnOpeningHandshakeStarted(
    network::mojom::WebSocketHandshakeRequestPtr request) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LogDebug("OnOpeningHandshakeStarted - handshake beginning");
}

void MouseMuxClient::OnFailure(const std::string& message,
                               int net_error,
                               int response_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::string err_msg = base::StringPrintf(
      "OnFailure: %s (net_error=%d, response_code=%d)",
      message.c_str(), net_error, response_code);
  LogDebug(err_msg);
  LOG(ERROR) << "MouseMux connection failed: " << message << " (net_error="
             << net_error << ", response_code=" << response_code << ")";
  ClosePipe();
}

void MouseMuxClient::OnConnectionEstablished(
    mojo::PendingRemote<network::mojom::WebSocket> socket,
    mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
    network::mojom::WebSocketHandshakeResponsePtr response,
    mojo::ScopedDataPipeConsumerHandle readable,
    mojo::ScopedDataPipeProducerHandle writable) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LogDebug("OnConnectionEstablished - WebSocket connected!");
  CHECK(!websocket_.is_bound());
  CHECK(state_ == State::kConnecting);

  websocket_.Bind(std::move(socket));
  readable_ = std::move(readable);
  writable_ = std::move(writable);
  CHECK_EQ(readable_watcher_.Watch(
               readable_.get(), MOJO_HANDLE_SIGNAL_READABLE,
               MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
               base::BindRepeating(&MouseMuxClient::ReadFromDataPipe,
                                   base::Unretained(this))),
           MOJO_RESULT_OK);

  client_receiver_.Bind(std::move(client_receiver));

  handshake_receiver_.set_disconnect_handler(base::DoNothing());
  client_receiver_.set_disconnect_handler(base::BindOnce(
      &MouseMuxClient::OnMojoPipeDisconnect, base::Unretained(this)));

  websocket_->StartReceiving();

  state_ = State::kOpen;
  LogDebug("State set to kOpen, sending login");

  // Send login message as required by MouseMux protocol.
  SendMessage(base::StringPrintf(
      R"({"type":"client.login.request.A2M",)"
      R"("appName":"Chrome MouseMux",)"
      R"("appVersion":"%s",)"
      R"("appBuildDate":"%s",)"
      R"("sdkVersion":"%s",)"
      R"("sdkBuildDate":"%s"})",
      kClientVersion, kBuildDate, kSdkVersion, kBuildDate));

  for (Observer& observer : observers_) {
    observer.OnConnectionStateChanged(true);
  }
}

void MouseMuxClient::OnDataFrame(bool finish,
                                 network::mojom::WebSocketMessageType type,
                                 uint64_t data_len) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK_EQ(state_, State::kOpen);
  CHECK_EQ(pending_read_data_index_, pending_read_data_.size());
  CHECK(!pending_read_finished_);

  if (data_len == 0) {
    if (finish) {
      ProcessCompletedMessage();
    }
    return;
  }

  const size_t old_size = pending_read_data_index_;
  const size_t new_size = old_size + data_len;
  if ((type != network::mojom::WebSocketMessageType::TEXT &&
       type != network::mojom::WebSocketMessageType::CONTINUATION) ||
      data_len > std::numeric_limits<uint32_t>::max() || new_size < old_size ||
      new_size > kMaxIncomingMessageSize) {
    LOG(ERROR) << "Invalid MouseMux frame (type: " << static_cast<int>(type)
               << ", len: " << data_len << ")";
    ClosePipe();
    return;
  }

  pending_read_data_.resize(new_size);
  pending_read_finished_ = finish;
  client_receiver_.Pause();
  ReadFromDataPipe(MOJO_RESULT_OK, mojo::HandleSignalsState());
}

void MouseMuxClient::OnDropChannel(bool was_clean,
                                   uint16_t code,
                                   const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LogDebug(base::StringPrintf("OnDropChannel: was_clean=%d, code=%d, reason=%s",
                               was_clean, code, reason.c_str()));
  CHECK(state_ == State::kOpen || state_ == State::kConnecting);
  ClosePipe();
}

void MouseMuxClient::OnClosingHandshake() {
  LogDebug("OnClosingHandshake");
}

void MouseMuxClient::ReadFromDataPipe(MojoResult,
                                      const mojo::HandleSignalsState&) {
  CHECK_LT(pending_read_data_index_, pending_read_data_.size());

  size_t actually_read_bytes = 0;
  const MojoResult result = readable_->ReadData(
      MOJO_READ_DATA_FLAG_NONE,
      base::span(pending_read_data_).subspan(pending_read_data_index_),
      actually_read_bytes);

  if (result == MOJO_RESULT_OK) {
    pending_read_data_index_ += actually_read_bytes;
    DCHECK_LE(pending_read_data_index_, pending_read_data_.size());

    if (pending_read_data_index_ < pending_read_data_.size()) {
      readable_watcher_.ArmOrNotify();
    } else {
      client_receiver_.Resume();
      if (pending_read_finished_) {
        ProcessCompletedMessage();
      }
    }
  } else if (result == MOJO_RESULT_SHOULD_WAIT) {
    readable_watcher_.ArmOrNotify();
  } else {
    LOG(ERROR) << "Reading MouseMux WebSocket frame failed: "
               << static_cast<int>(result);
    ClosePipe();
  }
}

void MouseMuxClient::ProcessCompletedMessage() {
  std::vector<uint8_t> pending_read_data;
  pending_read_data.swap(pending_read_data_);
  pending_read_data_index_ = 0;
  pending_read_finished_ = false;

  ParseAndDispatchMessage(pending_read_data);
}

void MouseMuxClient::ParseAndDispatchMessage(const std::vector<uint8_t>& data) {
  std::string json_str(data.begin(), data.end());

  std::optional<base::Value> parsed =
      base::JSONReader::Read(json_str, base::JSON_PARSE_RFC);

  if (!parsed || !parsed->is_dict()) {
    LogDebug("Failed to parse JSON: " + json_str.substr(0, 100));
    return;
  }

  const base::Value::Dict& dict = parsed->GetDict();
  const std::string* type = dict.FindString("type");
  if (!type) {
    LogDebug("No type field in message: " + json_str.substr(0, 100));
    return;
  }

  // Handle server ACK/response messages (echoed A2M type with ok field).
  if (type->ends_with(".A2M")) {
    auto ok = dict.FindBool("ok");
    if (ok.has_value()) {
      if (!*ok) {
        LogDebug("Server rejected request: " + *type);
        // If login was rejected, disconnect.
        if (*type == "client.login.request.A2M") {
          LogDebug("Login rejected by server - disconnecting");
          ClosePipe();
        }
      }
      return;
    }
  }

  // Only log user-related and keyboard messages for minimal logging.
  if (*type == kTypeUserList || *type == kTypeUserCreate ||
      *type == kTypeUserDispose || *type == kTypeUserChanged ||
      *type == kTypeKeyboardKey) {
    LogDebug("MSG: " + *type);
  }

  // Handle pointer motion/button/wheel events.
  if (*type == kTypeMotion || *type == kTypeButton ||
      *type == kTypeWheel) {
    std::optional<int> hwid = dict.FindInt("hwid");

    // Handle x/y as either int or double (JSON might send either).
    std::optional<double> x;
    std::optional<double> y;
    if (auto x_double = dict.FindDouble("x")) {
      x = x_double;
    } else if (auto x_int = dict.FindInt("x")) {
      x = static_cast<double>(*x_int);
    }
    if (auto y_double = dict.FindDouble("y")) {
      y = y_double;
    } else if (auto y_int = dict.FindInt("y")) {
      y = static_cast<double>(*y_int);
    }

    if (!hwid || !x || !y) {
      LogDebug(base::StringPrintf("Missing fields in %s: hwid=%d x=%d y=%d",
                                   type->c_str(), hwid.has_value(),
                                   x.has_value(), y.has_value()));
      return;
    }

    if (*type == kTypeMotion) {
      for (Observer& observer : observers_) {
        observer.OnMouseMotion(*hwid, static_cast<float>(*x),
                               static_cast<float>(*y));
      }
    } else if (*type == kTypeButton) {
      std::optional<int> button_data = dict.FindInt("button");
      if (!button_data) {
        LogDebug(base::StringPrintf(
            "BTN missing 'button' field! hwid=0x%x pos=(%.0f,%.0f) json=%s",
            *hwid, *x, *y, json_str.substr(0, 200).c_str()));
        return;
      }
      // Log every button event received from server.
      LogDebug(base::StringPrintf(
          "BTN RECV: hwid=0x%x button=0x%x pos=(%.0f,%.0f)",
          *hwid, *button_data, *x, *y));
      for (Observer& observer : observers_) {
        observer.OnMouseButton(*hwid, static_cast<float>(*x),
                               static_cast<float>(*y), *button_data);
      }
    } else {
      // Wheel event
      std::optional<int> wheel_delta = dict.FindInt("delta");
      if (!wheel_delta) {
        LogDebug("WHEEL missing 'delta' field!");
        return;
      }
      bool horizontal = dict.FindBool("horizontal").value_or(false);
      for (Observer& observer : observers_) {
        observer.OnMouseWheel(*hwid, static_cast<float>(*x),
                              static_cast<float>(*y), *wheel_delta,
                              horizontal);
      }
    }
    return;
  }

  // Handle keyboard events.
  if (*type == kTypeKeyboardKey) {
    std::optional<int> hwid = dict.FindInt("hwid");
    std::optional<int> vkey = dict.FindInt("vkey");
    std::optional<int> message = dict.FindInt("message");
    std::optional<int> scan = dict.FindInt("scan");
    std::optional<int> flags = dict.FindInt("flags");

    if (!hwid || !vkey || !message || !scan || !flags) {
      LogDebug(base::StringPrintf(
          "Missing fields in %s: hwid=%d vkey=%d msg=%d scan=%d flags=%d",
          type->c_str(), hwid.has_value(), vkey.has_value(),
          message.has_value(), scan.has_value(), flags.has_value()));
      return;
    }

    LogDebug(base::StringPrintf(
        "KEY RECV: hwid=0x%x vkey=0x%x msg=0x%x scan=%d flags=%d",
        *hwid, *vkey, *message, *scan, *flags));

    for (Observer& observer : observers_) {
      observer.OnKeyboardKey(*hwid, *vkey, *message, *scan, *flags);
    }
    return;
  }

  // Handle user list response.
  if (*type == kTypeUserList) {
    LogDebug("Received user list");
    const base::Value::List* users = dict.FindList("users");
    if (!users) {
      return;
    }

    std::vector<UserInfo> user_list;
    for (const auto& user_value : *users) {
      if (!user_value.is_dict()) {
        continue;
      }
      const base::Value::Dict& user_dict = user_value.GetDict();

      UserInfo info;
      std::optional<int> id = user_dict.FindInt("id");
      if (id) {
        info.user_id = *id;
      }

      const std::string* name = user_dict.FindString("name");
      if (name) {
        info.name = *name;
      }

      // Find mouse and keyboard hwids from devices array.
      const base::Value::List* devices = user_dict.FindList("devices");
      if (devices) {
        for (const auto& device_value : *devices) {
          if (!device_value.is_dict()) {
            continue;
          }
          const base::Value::Dict& device_dict = device_value.GetDict();
          std::optional<int> dev_hwid = device_dict.FindInt("hwid");
          const std::string* dev_type = device_dict.FindString("type");
          if (dev_hwid && dev_type) {
            if (*dev_type == "pointer") {
              info.hwid_mouse = *dev_hwid;
            } else if (*dev_type == "keyboard") {
              info.hwid_keyboard = *dev_hwid;
            }
          }
        }
      }

      LogDebug(base::StringPrintf("  User: id=%d name=%s mouse=0x%x kb=0x%x",
                                   info.user_id, info.name.c_str(),
                                   info.hwid_mouse, info.hwid_keyboard));
      user_list.push_back(std::move(info));
    }

    for (Observer& observer : observers_) {
      observer.OnUserList(user_list);
    }
    return;
  }

  // Handle user create notification.
  if (*type == kTypeUserCreate || *type == kTypeUserChanged) {
    // user.changed.notify.M2A has action field.
    const std::string* action = dict.FindString("action");
    if (*type == kTypeUserChanged && action) {
      if (*action == "dispose") {
        // Handle dispose via user.changed.
        std::optional<int> hwid_ms = dict.FindInt("hwid_ms");
        std::optional<int> hwid_kb = dict.FindInt("hwid_kb");
        LogDebug(base::StringPrintf("User disposed via changed: mouse=%d kb=%d",
                                     hwid_ms.value_or(-1), hwid_kb.value_or(-1)));
        for (Observer& observer : observers_) {
          observer.OnUserDisposed(hwid_ms.value_or(-1), hwid_kb.value_or(-1));
        }
        return;
      }
      if (*action == "map") {
        // Keyboard was mapped/unmapped for a user - refresh user list
        // to update keyboard-to-mouse mapping.
        LogDebug(base::StringPrintf("User map event received - requesting user list refresh"));
        SendMessage("{\"type\":\"user.list.request.A2M\"}");
        return;
      }
      if (*action != "create") {
        LogDebug(base::StringPrintf("User changed: unhandled action=%s", action->c_str()));
        return;
      }
    } else if (*type == kTypeUserChanged && !action) {
      return;
    }

    UserInfo info;
    std::optional<int> hwid_ms = dict.FindInt("hwid_ms");
    std::optional<int> hwid_kb = dict.FindInt("hwid_kb");
    const std::string* name = dict.FindString("name");
    std::optional<int> user_id = dict.FindInt("userId");

    if (hwid_ms) {
      info.hwid_mouse = *hwid_ms;
    }
    if (hwid_kb) {
      info.hwid_keyboard = *hwid_kb;
    }
    if (name) {
      info.name = *name;
    }
    if (user_id) {
      info.user_id = *user_id;
    }

    LogDebug(base::StringPrintf("User created: id=%d name=%s mouse=%d kb=%d",
                                 info.user_id, info.name.c_str(),
                                 info.hwid_mouse, info.hwid_keyboard));

    for (Observer& observer : observers_) {
      observer.OnUserCreated(info);
    }
    return;
  }

  // Handle ping from server - respond with pong.
  if (*type == kTypePing) {
    SendMessage(R"({"type":"client.pong.request.A2M"})");
    return;
  }

  // Handle server shutdown notification.
  if (*type == kTypeServerShutdown) {
    const std::string* reason = dict.FindString("reason");
    LogDebug(base::StringPrintf("Server shutdown: %s",
                                 reason ? reason->c_str() : "unknown"));
    ClosePipe();
    return;
  }

  // Handle timeout warning.
  if (*type == kTypeTimeoutWarning) {
    int minutes = dict.FindInt("minutes").value_or(0);
    LogDebug(base::StringPrintf("Timeout warning: %d minutes remaining", minutes));
    for (Observer& observer : observers_) {
      observer.OnTimeoutWarning(minutes);
    }
    return;
  }

  // Handle timeout stopped.
  if (*type == kTypeTimeoutStopped) {
    const std::string* reason = dict.FindString("reason");
    std::string reason_str = reason ? *reason : "timeout";
    LogDebug("Session stopped: " + reason_str);
    for (Observer& observer : observers_) {
      observer.OnTimeoutStopped(reason_str);
    }
    ClosePipe();
    return;
  }

  // Handle user dispose notification.
  if (*type == kTypeUserDispose) {
    std::optional<int> hwid_ms = dict.FindInt("hwid_ms");
    std::optional<int> hwid_kb = dict.FindInt("hwid_kb");
    LogDebug(base::StringPrintf("User disposed: mouse=%d kb=%d",
                                 hwid_ms.value_or(-1), hwid_kb.value_or(-1)));
    for (Observer& observer : observers_) {
      observer.OnUserDisposed(hwid_ms.value_or(-1), hwid_kb.value_or(-1));
    }
    return;
  }
}

void MouseMuxClient::ClosePipe() {
  LogDebug("ClosePipe called, state=" + std::to_string(static_cast<int>(state_)));
  if (state_ == State::kDisconnected) {
    LogDebug("Already disconnected, returning");
    return;
  }

  bool was_connected = (state_ == State::kOpen);

  // Send logout before closing if we were connected.
  if (was_connected && websocket_.is_bound()) {
    LogDebug("Sending logout before disconnect");
    SendMessage(base::StringPrintf(
        R"({"type":"client.logout.request.A2M",)"
        R"("appName":"Chrome MouseMux",)"
        R"("appVersion":"%s",)"
        R"("sdkVersion":"%s",)"
        R"("reason":"shutdown"})",
        kClientVersion, kSdkVersion));
  }
  state_ = State::kDisconnected;
  LogDebug("State set to kDisconnected, was_connected=" +
           std::to_string(was_connected));

  handshake_receiver_.reset();
  client_receiver_.reset();
  websocket_.reset();
  readable_.reset();
  writable_.reset();
  readable_watcher_.Cancel();

  pending_read_data_index_ = 0;
  pending_read_finished_ = false;
  pending_read_data_.clear();

  if (was_connected) {
    for (Observer& observer : observers_) {
      observer.OnConnectionStateChanged(false);
    }
  }
}

void MouseMuxClient::OnMojoPipeDisconnect() {
  ClosePipe();
}

}  // namespace content
