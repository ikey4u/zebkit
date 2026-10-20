// The Chrome DevTools Protocol endpoint: content's DevTools server, which
// is what Chrome serves with --remote-debugging-port, started on request.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <string>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/common/content_switches.h"
#include "content/shell/common/shell_switches.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/tcp_server_socket.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

namespace {

// What the server bound, written by the DevTools thread once it has.
struct Binding {
  base::WaitableEvent bound;
  std::atomic<unsigned> port{0};
};

class SocketFactory : public content::DevToolsSocketFactory {
 public:
  SocketFactory(std::string address, uint16_t port, Binding* binding)
      : address_(std::move(address)), port_(port), binding_(binding) {}

 private:
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    net::IPEndPoint endpoint;
    if (socket->ListenWithAddressAndPort(address_, port_, 10) != net::OK ||
        socket->GetLocalAddress(&endpoint) != net::OK) {
      binding_->bound.Signal();
      return nullptr;
    }
    binding_->port.store(endpoint.port());
    binding_->bound.Signal();
    return socket;
  }

  std::unique_ptr<net::ServerSocket> CreateForTethering(
      std::string* out_name) override {
    return nullptr;
  }

  std::string address_;
  uint16_t port_;
  raw_ptr<Binding> binding_;
};

struct Endpoint {
  unsigned port = 0;
  std::string address;
  // DevToolsActivePort is written here, with the browser target's path.
  base::ScopedTempDir directory;
  std::string url;
};

Endpoint& Current() {
  static base::NoDestructor<Endpoint> endpoint;
  return *endpoint;
}

}  // namespace

unsigned CdpListen(const std::string& address_in,
                   unsigned port,
                   std::string* error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Endpoint& endpoint = Current();
  if (endpoint.port)
    return endpoint.port;
  const std::string address = address_in.empty() ? "127.0.0.1" : address_in;
  net::IPAddress parsed;
  if (!parsed.AssignFromIPLiteral(address)) {
    *error = "not a numeric IP address: " + address;
    return 0;
  }
  if (port > 65535) {
    *error = "not a port: " + base::NumberToString(port);
    return 0;
  }
  // A short wait on the UI thread for the DevTools thread to bind, so that
  // the port can be returned. Chromium lists who may block here; what zebkit
  // links is test-only until it has its own embedder, and these checks are
  // compiled out of release builds.
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
  if (!endpoint.directory.CreateUniqueTempDir()) {
    *error = "cannot create a temporary directory";
    return 0;
  }
  // The binding outlives the server's thread's use of it: it is waited for
  // here, and the factory touches it only when it creates the socket.
  static base::NoDestructor<Binding> binding;
  binding->bound.Reset();
  binding->port.store(0);
  content::DevToolsAgentHost::StartRemoteDebuggingServer(
      std::make_unique<SocketFactory>(address, static_cast<uint16_t>(port),
                                      binding.get()),
      endpoint.directory.GetPath(), base::FilePath());
  if (!binding->bound.TimedWait(base::Seconds(10)) || !binding->port.load()) {
    content::DevToolsAgentHost::StopRemoteDebuggingServer();
    std::ignore = endpoint.directory.Delete();
    *error = "cannot listen on " + address + ":" + base::NumberToString(port);
    return 0;
  }
  endpoint.port = binding->port.load();
  endpoint.address = address;
  endpoint.url.clear();
  return endpoint.port;
}

unsigned CdpPort() {
  return Current().port;
}

std::string CdpUrl() {
  Endpoint& endpoint = Current();
  if (!endpoint.port)
    return std::string();
  if (!endpoint.url.empty())
    return endpoint.url;
  // The server writes the browser target's path once it serves: a line
  // with the port, and one with /devtools/browser/<id>.
  base::ScopedAllowBlockingForTesting allow_blocking;
  const base::FilePath file =
      endpoint.directory.GetPath().Append("DevToolsActivePort");
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::string text;
    if (base::ReadFileToString(file, &text)) {
      std::vector<std::string> lines = base::SplitString(
          text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
      if (lines.size() >= 2) {
        const std::string host = endpoint.address.find(':') != std::string::npos
                                     ? "[" + endpoint.address + "]"
                                     : endpoint.address;
        endpoint.url = "ws://" + host + ":" +
                       base::NumberToString(endpoint.port) + lines[1];
        return endpoint.url;
      }
    }
    base::PlatformThread::Sleep(base::Milliseconds(10));
  }
  return std::string();
}

void CdpStop() {
  Endpoint& endpoint = Current();
  if (!endpoint.port)
    return;
  content::DevToolsAgentHost::DetachAllClients();
  content::DevToolsAgentHost::StopRemoteDebuggingServer();
  base::ScopedAllowBlockingForTesting allow_blocking;
  std::ignore = endpoint.directory.Delete();
  endpoint.port = 0;
  endpoint.url.clear();
}

void StartCdpFromCommandLine() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(switches::kRemoteDebuggingPort)) {
    unsigned port = 0;
    if (!base::StringToUint(
            command_line.GetSwitchValueASCII(switches::kRemoteDebuggingPort),
            &port)) {
      LOG(ERROR) << "--" << switches::kRemoteDebuggingPort << " needs a port";
    } else {
      std::string error;
      // The server prints "DevTools listening on ws://...", as Chrome's
      // does, for clients that read it from stderr.
      if (!CdpListen(command_line.GetSwitchValueASCII(
                         switches::kRemoteDebuggingAddress),
                     port, &error)) {
        LOG(ERROR) << "CDP: " << error;
      }
    }
  }
  if (command_line.HasSwitch(switches::kRemoteDebuggingPipe))
    content::DevToolsAgentHost::StartRemoteDebuggingPipeHandler(
        base::OnceClosure());
}

}  // namespace zebkit

extern "C" {

unsigned zebkit_cdp_listen(const char* address, unsigned port, char** error) {
  std::string message;
  const unsigned bound =
      zebkit::CdpListen(address ? address : "", port, &message);
  if (error)
    *error = bound ? nullptr : strdup(message.c_str());
  return bound;
}

unsigned zebkit_cdp_get_port(void) {
  return zebkit::CdpPort();
}

char* zebkit_cdp_get_url(void) {
  const std::string url = zebkit::CdpUrl();
  return url.empty() ? nullptr : strdup(url.c_str());
}

void zebkit_cdp_stop(void) {
  zebkit::CdpStop();
}

}  // extern "C"
