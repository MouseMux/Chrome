// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_control_server.h"

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace content {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("mousemux_control_server", R"(
      semantics {
        sender: "MouseMux Control Server"
        description:
          "Local WebSocket server for external automation of MouseMux "
          "multi-mouse state (owner, capture, connection)."
        trigger: "Launched via --mousemux-control-port flag."
        data: "JSON messages with MouseMux state."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "Controlled by --mousemux-control-port command-line flag."
      })");

}  // namespace

// Delegate runs on the IO thread, forwards messages to UI thread.
class MouseMuxControlServer::ServerDelegate
    : public net::HttpServer::Delegate {
 public:
  ServerDelegate(base::WeakPtr<MouseMuxControlServer> owner,
                 scoped_refptr<base::SingleThreadTaskRunner> ui_runner)
      : owner_(owner), ui_runner_(ui_runner) {}

  net::HttpServer* server() { return server_.get(); }

  void CreateServer(uint16_t port) {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    int rv = socket->ListenWithAddressAndPort("127.0.0.1", port, 5);
    if (rv != net::OK) {
      LOG(ERROR) << "MouseMuxControlServer: failed to listen on port " << port
                 << ": " << net::ErrorToString(rv);
      return;
    }
    server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
    net::IPEndPoint address;
    server_->GetLocalAddress(&address);
    LOG(INFO) << "MouseMuxControlServer: listening on ws://127.0.0.1:"
              << address.port();
  }

  void SendOnIO(int connection_id, const std::string& data) {
    if (server_) {
      server_->SendOverWebSocket(connection_id, data, kTrafficAnnotation);
    }
  }

  // net::HttpServer::Delegate:
  void OnConnect(int connection_id) override {}

  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override {
    // Not a WebSocket request — return a simple status page.
    if (server_) {
      server_->Send200(connection_id,
                       "MouseMux Control Server\n",
                       "text/plain", kTrafficAnnotation);
    }
  }

  void OnWebSocketRequest(
      int connection_id,
      const net::HttpServerRequestInfo& request) override {
    if (server_) {
      server_->AcceptWebSocket(connection_id, request, kTrafficAnnotation);
    }
  }

  void OnWebSocketMessage(int connection_id, std::string data) override {
    ui_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MouseMuxControlServer::OnMessage, owner_,
                       connection_id, std::move(data)));
  }

  void OnClose(int connection_id) override {}

 private:
  base::WeakPtr<MouseMuxControlServer> owner_;
  scoped_refptr<base::SingleThreadTaskRunner> ui_runner_;
  std::unique_ptr<net::HttpServer> server_;
};

MouseMuxControlServer::MouseMuxControlServer(
    MouseMuxInputController* controller)
    : controller_(controller) {}

MouseMuxControlServer::~MouseMuxControlServer() {
  Stop();
}

bool MouseMuxControlServer::Start(uint16_t port) {
  if (io_thread_) {
    return false;  // Already running.
  }

  io_thread_ = std::make_unique<base::Thread>("MouseMuxControlServer");
  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  if (!io_thread_->StartWithOptions(std::move(options))) {
    io_thread_.reset();
    return false;
  }

  delegate_ = std::make_unique<ServerDelegate>(
      weak_factory_.GetWeakPtr(),
      base::SingleThreadTaskRunner::GetCurrentDefault());

  io_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&ServerDelegate::CreateServer,
                     base::Unretained(delegate_.get()), port));
  return true;
}

void MouseMuxControlServer::Stop() {
  if (!io_thread_) {
    return;
  }

  // The delegate owns a net::HttpServer created on, and bound to, the IO
  // thread — its socket has an IO watcher registered there.  Destroying it
  // from the UI thread would tear that down on the wrong sequence, so hand
  // the delete back to the IO thread and let the join below wait for it.
  if (delegate_) {
    io_thread_->task_runner()->DeleteSoon(FROM_HERE, delegate_.release());
  }

  // Joins the thread, which also runs the DeleteSoon queued above.
  io_thread_.reset();
}

void MouseMuxControlServer::OnMessage(int connection_id, std::string data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  auto parsed = base::JSONReader::Read(data, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    SendResponse(connection_id, R"({"type":"error","msg":"invalid json"})");
    return;
  }

  const base::DictValue& dict = parsed->GetDict();
  const std::string* type = dict.FindString("type");
  if (!type) {
    SendResponse(connection_id, R"({"type":"error","msg":"missing type"})");
    return;
  }

  if (*type == "status") {
    HandleStatus(connection_id);
  } else if (*type == "set") {
    HandleSet(connection_id, data);
  } else {
    SendResponse(connection_id, R"({"type":"error","msg":"unknown type"})");
  }
}

void MouseMuxControlServer::SendResponse(int connection_id,
                                         const std::string& json) {
  if (!io_thread_ || !delegate_) {
    return;
  }
  io_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&ServerDelegate::SendOnIO,
                     base::Unretained(delegate_.get()),
                     connection_id, json));
}

void MouseMuxControlServer::HandleStatus(int connection_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  base::DictValue status;
  status.Set("type", "status");

  bool connected = controller_->IsMouseMuxEnabled();
  status.Set("connected", connected);
  status.Set("blocked", controller_->IsNativeInputBlocked());
  status.Set("captured", controller_->IsCaptured());
  const std::string diagnostics = controller_->GetDialogDiagnostics();
  if (!diagnostics.empty()) {
    status.Set("dialog", diagnostics);
  }
  status.Set("visible", controller_->IsDialogVisible());

  int owner_hwid = controller_->GetOwnerHwid();
  if (owner_hwid >= 0) {
    std::string name = controller_->GetOwnerName();
    if (!name.empty()) {
      status.Set("owner", name);
    } else {
      status.Set("owner", owner_hwid);
    }
  } else {
    status.Set("owner", base::Value());  // null
  }

  std::string json;
  base::JSONWriter::Write(status, &json);
  SendResponse(connection_id, json);
}

void MouseMuxControlServer::HandleSet(int connection_id,
                                      const std::string& data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  auto parsed = base::JSONReader::Read(data, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    SendResponse(connection_id, R"({"type":"error","msg":"invalid json"})");
    return;
  }

  const base::DictValue& dict = parsed->GetDict();

  // Handle "connected" — enable/disable MouseMux WebSocket connection.
  std::optional<bool> connected = dict.FindBool("connected");
  if (connected.has_value()) {
    if (*connected != controller_->IsMouseMuxEnabled()) {
      controller_->SetMouseMuxEnabled(*connected);
    }
  }

  // Handle "owner" — set or release ownership.
  // owner: null or 0 or "" → release.  owner: "name" → set by name.
  // owner: int > 0 → set by hwid.
  const base::Value* owner_val = dict.Find("owner");
  if (owner_val) {
    bool should_release = false;
    if (owner_val->is_none()) {
      should_release = true;
    } else if (owner_val->is_string()) {
      const std::string& owner_str = owner_val->GetString();
      if (owner_str.empty() || owner_str == "0" || owner_str == "null") {
        should_release = true;
      } else {
        controller_->SetOwnerByName(owner_str);
      }
    } else if (owner_val->is_int()) {
      int hwid = owner_val->GetInt();
      if (hwid <= 0) {
        should_release = true;
      } else {
        controller_->SetOwner(hwid);
      }
    }
    if (should_release) {
      if (controller_->IsCaptured()) {
        controller_->ReleaseCapture();
      }
      if (controller_->GetOwnerHwid() >= 0) {
        controller_->ReleaseOwnership();
      }
    }
  }

  // Handle "blocked" — enable/disable native input blocking.
  std::optional<bool> blocked = dict.FindBool("blocked");
  if (blocked.has_value()) {
    if (*blocked != controller_->IsNativeInputBlocked()) {
      controller_->SetNativeInputBlocked(*blocked);
    }
  }

  // Handle "visible" — show or hide the control dialog.  Hiding does not
  // change any input behaviour, so this is always reversible from here even
  // when the dialog itself is off screen.
  std::optional<bool> visible = dict.FindBool("visible");
  if (visible.has_value()) {
    controller_->SetDialogVisible(*visible);
  }

  // Handle "captured" — start/stop capture.
  std::optional<bool> captured = dict.FindBool("captured");
  if (captured.has_value()) {
    if (*captured && !controller_->IsCaptured()) {
      controller_->CaptureOwner();
    } else if (!*captured && controller_->IsCaptured()) {
      controller_->ReleaseCapture();
    }
  }

  SendResponse(connection_id, R"({"type":"ok"})");
}

}  // namespace content
