// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_control_server.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "content/browser/renderer_host/input/mouse_mux/mouse_mux_input_controller.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/desktop_streams_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "url/gurl.h"
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
    if (!server_) {
      return;
    }
    // A file from <exe dir>/mousemux-web, by bare name: "/" is host.html,
    // "/view.html" is view.html.  Nothing with a slash or a dot-dot inside
    // gets near the disk.
    std::string name = info.path;
    if (const size_t q = name.find('?'); q != std::string::npos) {
      name.erase(q);
    }
    if (name == "/" || name.empty()) {
      name = "/host.html";
    }
    name.erase(0, 1);
    bool clean = !name.empty();
    for (const char ch : name) {
      clean = clean && (base::IsAsciiAlphaNumeric(ch) || ch == '.' ||
                        ch == '_' || ch == '-');
    }
    if (!clean || name.find("..") != std::string::npos) {
      server_->Send404(connection_id, kTrafficAnnotation);
      return;
    }
    base::FilePath dir;
    base::PathService::Get(base::DIR_EXE, &dir);
    const base::FilePath file =
        dir.Append(FILE_PATH_LITERAL("mousemux-web")).AppendASCII(name);
    std::string mime = "text/plain; charset=utf-8";
    if (base::EndsWith(name, ".html")) {
      mime = "text/html; charset=utf-8";
    } else if (base::EndsWith(name, ".js")) {
      mime = "application/javascript; charset=utf-8";
    } else if (base::EndsWith(name, ".css")) {
      mime = "text/css; charset=utf-8";
    } else if (base::EndsWith(name, ".png")) {
      mime = "image/png";
    }
    // Disk is not the IO thread's business either.
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](const base::FilePath& file) -> std::optional<std::string> {
              std::string body;
              if (!base::ReadFileToString(file, &body)) {
                return std::nullopt;
              }
              return body;
            },
            file),
        base::BindOnce(&ServerDelegate::SendFile, weak_factory_.GetWeakPtr(),
                       connection_id, mime));
  }

  void SendFile(int connection_id,
                const std::string& mime,
                std::optional<std::string> body) {
    if (!server_) {
      return;
    }
    if (!body) {
      server_->Send404(connection_id, kTrafficAnnotation);
      return;
    }
    server_->Send200(connection_id, *body, mime, kTrafficAnnotation);
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
  base::WeakPtrFactory<ServerDelegate> weak_factory_{this};
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
  controller_->LogDebug("CONTROL IN  #" + base::NumberToString(connection_id) +
                        ": " + data.substr(0, 300));

  if (*type == "status") {
    HandleStatus(connection_id);
  } else if (*type == "set") {
    HandleSet(connection_id, data);
  } else if (*type == "capture") {
    HandleCapture(connection_id, data);
  } else if (*type == "sources") {
    HandleSources(connection_id);
  } else if (*type == "rect") {
    HandleRect(connection_id, data);
  } else if (*type == "assign") {
    HandleAssign(connection_id, data);
  } else {
    SendResponse(connection_id, R"({"type":"error","msg":"unknown type"})");
  }
}

void MouseMuxControlServer::SendResponse(int connection_id,
                                         const std::string& json) {
  if (!io_thread_ || !delegate_) {
    return;
  }
  controller_->LogDebug("CONTROL OUT #" + base::NumberToString(connection_id) +
                        ": " + json.substr(0, 300));
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

namespace {

gfx::AcceleratedWidget HwndFromDict(const base::DictValue& dict) {
  if (std::optional<double> raw = dict.FindDouble("hwnd")) {
    return reinterpret_cast<gfx::AcceleratedWidget>(
        static_cast<uintptr_t>(*raw));
  }
  return gfx::kNullAcceleratedWidget;
}

void SetRect(base::DictValue& into, gfx::AcceleratedWidget hwnd) {
  RECT rect = {};
  ::GetWindowRect(hwnd, &rect);
  into.Set("hwnd", static_cast<double>(reinterpret_cast<uintptr_t>(hwnd)));
  into.Set("x", static_cast<int>(rect.left));
  into.Set("y", static_cast<int>(rect.top));
  into.Set("width", static_cast<int>(rect.right - rect.left));
  into.Set("height", static_cast<int>(rect.bottom - rect.top));
}

std::string CaptionOf(gfx::AcceleratedWidget hwnd) {
  const int length = ::GetWindowTextLengthW(hwnd);
  if (length <= 0) {
    return std::string();
  }
  std::wstring caption(static_cast<size_t>(length) + 1, L'\0');
  const int copied =
      ::GetWindowTextW(hwnd, caption.data(), static_cast<int>(caption.size()));
  if (copied <= 0) {
    return std::string();
  }
  caption.resize(static_cast<size_t>(copied));
  return base::WideToUTF8(caption);
}

}  // namespace

void MouseMuxControlServer::HandleSources(int connection_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  base::ListValue windows;
  for (gfx::AcceleratedWidget hwnd : controller_->KnownWindows()) {
    base::DictValue entry;
    SetRect(entry, hwnd);
    entry.Set("title", CaptionOf(hwnd));
    entry.Set("owner", controller_->OwnerNameOfWindow(hwnd));
    windows.Append(std::move(entry));
  }
  base::DictValue reply;
  reply.Set("type", "sources");
  reply.Set("windows", std::move(windows));
  std::string json;
  base::JSONWriter::Write(reply, &json);
  SendResponse(connection_id, json);
}

void MouseMuxControlServer::HandleRect(int connection_id,
                                       const std::string& data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto parsed = base::JSONReader::Read(data, base::JSON_ALLOW_TRAILING_COMMAS);
  const gfx::AcceleratedWidget hwnd =
      parsed && parsed->is_dict() ? HwndFromDict(parsed->GetDict())
                                  : gfx::kNullAcceleratedWidget;
  if (!hwnd || !::IsWindow(hwnd)) {
    SendResponse(connection_id,
                 R"({"type":"error","msg":"rect: no such window"})");
    return;
  }
  base::DictValue reply;
  reply.Set("type", "rect");
  SetRect(reply, hwnd);
  std::string json;
  base::JSONWriter::Write(reply, &json);
  SendResponse(connection_id, json);
}

void MouseMuxControlServer::HandleAssign(int connection_id,
                                         const std::string& data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto parsed = base::JSONReader::Read(data, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    SendResponse(connection_id, R"({"type":"error","msg":"invalid json"})");
    return;
  }
  const base::DictValue& dict = parsed->GetDict();
  const gfx::AcceleratedWidget hwnd = HwndFromDict(dict);
  const std::optional<int> hwid = dict.FindInt("hwid");
  if (!hwnd || !hwid) {
    SendResponse(connection_id,
                 R"({"type":"error","msg":"assign: hwnd and hwid required"})");
    return;
  }
  // Not captured: a virtual user has no native input to stop.  Blocked, so
  // the host's real mouse cannot disturb a window that is somebody's.
  const bool ok = controller_->AssignWindowToHwid(
      *hwid, hwnd, /*captured=*/false, /*block_native=*/true);
  SendResponse(connection_id,
               ok ? R"({"type":"ok"})"
                  : R"({"type":"error","msg":"assign: refused, unknown device or another user's window"})");
}

void MouseMuxControlServer::HandleCapture(int connection_id,
                                          const std::string& data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto parsed = base::JSONReader::Read(data, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    SendResponse(connection_id, R"({"type":"error","msg":"invalid json"})");
    return;
  }
  const base::DictValue& dict = parsed->GetDict();

  // 1. Which window: by owner name, or by handle.
  gfx::AcceleratedWidget hwnd = gfx::kNullAcceleratedWidget;
  if (const std::string* owner = dict.FindString("owner")) {
    for (const auto& info : controller_->GetOwners()) {
      if (info.name == *owner && info.has_window) {
        hwnd = info.window;
        break;
      }
    }
  } else if (std::optional<double> raw = dict.FindDouble("hwnd")) {
    hwnd = reinterpret_cast<gfx::AcceleratedWidget>(
        static_cast<uintptr_t>(*raw));
  }
  if (!hwnd || !::IsWindow(hwnd)) {
    SendResponse(connection_id,
                 R"({"type":"error","msg":"capture: no such window"})");
    return;
  }
  aura::WindowTreeHost* host = aura::WindowTreeHost::GetForAcceleratedWidget(hwnd);
  if (!host || !host->window()) {
    SendResponse(connection_id,
                 R"({"type":"error","msg":"capture: window has no aura root"})");
    return;
  }

  // 2. Which page asked: the tab showing the URL it sent.  The registry
  //    ties the stream to that frame and origin, so no other page can use
  //    the id, and getUserMedia in that page is what Chromium already
  //    accepts for chromeMediaSource "desktop".
  const std::string* page = dict.FindString("page");
  if (!page) {
    SendResponse(connection_id,
                 R"({"type":"error","msg":"capture: page url missing"})");
    return;
  }
  const GURL page_url(*page);
  RenderFrameHost* frame = nullptr;
  for (WebContentsImpl* contents : WebContentsImpl::GetAllWebContents()) {
    if (contents && contents->GetLastCommittedURL() == page_url) {
      frame = contents->GetPrimaryMainFrame();
      break;
    }
  }
  if (!frame || !frame->GetProcess()) {
    SendResponse(connection_id,
                 R"({"type":"error","msg":"capture: no tab shows that page"})");
    return;
  }

  // 3. Register: the window's root as a source, then a stream on it for
  //    that frame.  The source id names an aura window, which on Windows
  //    selects the compositor-based capturer (AuraWindowVideoCaptureDevice):
  //    frames come from what the window would draw, whether or not any
  //    screen shows it, and the capture keeps the compositor alive while the
  //    window is occluded (WindowTreeHost::VideoCaptureLock).
  const DesktopMediaID source = DesktopMediaID::RegisterNativeWindow(
      DesktopMediaID::TYPE_WINDOW, host->window());
  //    Restricted to the page's process and origin, not to one frame: a
  //    second tab showing the same URL, or a reload, gives a different frame
  //    in the same process, and the lookup by URL above can land on the
  //    other one.  Then getUserMedia failed with "Error starting tab
  //    capture" (STREAM_NOT_FOUND_IN_REGISTRY) on the second attempt
  //    (2026-09-05 20:2x).  The spike page also makes its URL unique with a
  //    fragment, so the lookup itself is right.
  const std::string stream_id =
      DesktopStreamsRegistry::GetInstance()->RegisterStream(
          frame->GetProcess()->GetDeprecatedID(),
          /*restrict_to_render_frame_id=*/std::nullopt,
          frame->GetLastCommittedOrigin(), source,
          kRegistryStreamTypeDesktop);

  RECT rect = {};
  ::GetWindowRect(hwnd, &rect);
  base::DictValue reply;
  reply.Set("type", "capture");
  reply.Set("id", stream_id);
  reply.Set("hwnd", static_cast<double>(reinterpret_cast<uintptr_t>(hwnd)));
  reply.Set("x", static_cast<int>(rect.left));
  reply.Set("y", static_cast<int>(rect.top));
  reply.Set("width", static_cast<int>(rect.right - rect.left));
  reply.Set("height", static_cast<int>(rect.bottom - rect.top));
  std::string json;
  base::JSONWriter::Write(reply, &json);
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
