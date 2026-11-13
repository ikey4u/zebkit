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
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/common/content_switches.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/tcp_server_socket.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

// What the server bound, written by the DevTools thread once it has: waited
// for by zebkit_cdp_listen(), told to the engine's thread for
// zebkit_cdp_listen_async().
struct Binding {
  base::WaitableEvent bound;
  std::atomic<unsigned> port{0};
  scoped_refptr<base::SequencedTaskRunner> reply_runner;
  base::RepeatingCallback<void(unsigned)> reply;
};

class SocketFactory : public content::DevToolsSocketFactory {
 public:
  SocketFactory(std::string address, uint16_t port, std::shared_ptr<Binding> binding)
      : address_(std::move(address)), port_(port), binding_(std::move(binding)) {}

 private:
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    net::IPEndPoint endpoint;
    const bool listening = socket->ListenWithAddressAndPort(address_, port_, 10) == net::OK &&
                           socket->GetLocalAddress(&endpoint) == net::OK;
    binding_->port.store(listening ? endpoint.port() : 0);
    binding_->bound.Signal();
    if (binding_->reply)
      binding_->reply_runner->PostTask(FROM_HERE, base::BindOnce(binding_->reply, binding_->port.load()));
    return listening ? std::move(socket) : nullptr;
  }

  std::unique_ptr<net::ServerSocket> CreateForTethering(
      std::string* out_name) override {
    return nullptr;
  }

  std::string address_;
  uint16_t port_;
  std::shared_ptr<Binding> binding_;
};

struct Waiter {
  zebkit_cdp_listen_func func = nullptr;
  void* data = nullptr;
};

struct Endpoint {
  unsigned port = 0;
  std::string address;
  // What was asked for: 0 when any port.
  unsigned asked_port = 0;
  // DevToolsActivePort is written here, with the browser target's path.
  base::ScopedTempDir directory;
  std::string url;
  // zebkit_cdp_listen_async() under way: its callers, told together.
  bool starting = false;
  uint64_t generation = 0;
  std::vector<Waiter> waiting;
};

Endpoint& Current() {
  static base::NoDestructor<Endpoint> endpoint;
  return *endpoint;
}

// The checks both forms make. A second call is answered with what runs, unless
// it asks for another address or port.
bool CheckRequest(const std::string& address, unsigned port, std::string* error) {
  net::IPAddress parsed;
  if (!parsed.AssignFromIPLiteral(address)) {
    *error = "not a numeric IP address: " + address;
    return false;
  }
  if (port > 65535) {
    *error = "not a port: " + base::NumberToString(port);
    return false;
  }
  Endpoint& endpoint = Current();
  if ((endpoint.port || endpoint.starting) &&
      (address != endpoint.address || (port && port != (endpoint.port ? endpoint.port : endpoint.asked_port)))) {
    *error = "CDP is served on " + endpoint.address + ":" +
             base::NumberToString(endpoint.port ? endpoint.port : endpoint.asked_port) +
             " already; zebkit_cdp_stop() first";
    return false;
  }
  return true;
}

// The browser target's address, from the DevToolsActivePort the server writes
// once it serves: a line with the port, and one with /devtools/browser/<id>.
std::string ReadUrl(const base::FilePath& directory, const std::string& address, unsigned port) {
  std::string text;
  if (!base::ReadFileToString(directory.AppendASCII("DevToolsActivePort"), &text))
    return std::string();
  std::vector<std::string> lines =
      base::SplitString(text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (lines.size() < 2)
    return std::string();
  const std::string host = address.find(':') != std::string::npos ? "[" + address + "]" : address;
  return "ws://" + host + ":" + base::NumberToString(port) + lines[1];
}

void TellWaiters(unsigned port, const std::string& url, const std::string& error) {
  Endpoint& endpoint = Current();
  std::vector<Waiter> waiting = std::move(endpoint.waiting);
  endpoint.waiting.clear();
  for (const Waiter& waiter : waiting)
    waiter.func(port, url.empty() ? nullptr : url.c_str(), error.empty() ? nullptr : error.c_str(), waiter.data);
}

void DeleteSoon(base::ScopedTempDir directory) {
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
                             base::BindOnce([](base::ScopedTempDir directory) { std::ignore = directory.Delete(); },
                                            std::move(directory)));
}

// Off the engine's thread, as the file is there: up to two seconds, as the
// blocking form waits.
void ReadUrlSoon(uint64_t generation, int attempt) {
  Endpoint& endpoint = Current();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ReadUrl, endpoint.directory.GetPath(), endpoint.address, endpoint.port),
      base::BindOnce(
          [](uint64_t generation, int attempt, std::string url) {
            Endpoint& endpoint = Current();
            if (endpoint.generation != generation || !endpoint.port)
              return;
            if (url.empty() && attempt < 200) {
              content::GetUIThreadTaskRunner({})->PostDelayedTask(
                  FROM_HERE, base::BindOnce(&ReadUrlSoon, generation, attempt + 1), base::Milliseconds(10));
              return;
            }
            endpoint.url = url;
            TellWaiters(endpoint.port, url, url.empty() ? "the browser target's address did not come" : "");
          },
          generation, attempt));
}

void Bound(uint64_t generation, unsigned port) {
  Endpoint& endpoint = Current();
  if (endpoint.generation != generation || !endpoint.starting)
    return;
  endpoint.starting = false;
  if (!port) {
    content::DevToolsAgentHost::StopRemoteDebuggingServer();
    DeleteSoon(std::move(endpoint.directory));
    TellWaiters(0, std::string(),
                "cannot listen on " + endpoint.address + ":" + base::NumberToString(endpoint.asked_port));
    return;
  }
  endpoint.port = port;
  endpoint.url.clear();
  ReadUrlSoon(generation, 0);
}

}  // namespace

unsigned CdpListen(const std::string& address_in,
                   unsigned port,
                   std::string* error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Endpoint& endpoint = Current();
  const std::string address = address_in.empty() ? "127.0.0.1" : address_in;
  if (!CheckRequest(address, port, error))
    return 0;
  if (endpoint.port)
    return endpoint.port;
  if (endpoint.starting) {
    *error = "CDP is being started (zebkit_cdp_listen_async)";
    return 0;
  }
  // A short wait on the UI thread for the DevTools thread to bind, so that
  // the port can be returned: zebkit_cdp_listen_async() does not wait.
  // Chromium lists who may block here; what zebkit links is test-only until
  // it has its own embedder, and these checks are compiled out of release
  // builds.
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
  if (!endpoint.directory.CreateUniqueTempDir()) {
    *error = "cannot create a temporary directory";
    return 0;
  }
  // The binding outlives the server's thread's use of it: it is waited for
  // here, and the factory touches it only when it creates the socket.
  auto binding = std::make_shared<Binding>();
  content::DevToolsAgentHost::StartRemoteDebuggingServer(
      std::make_unique<SocketFactory>(address, static_cast<uint16_t>(port), binding),
      endpoint.directory.GetPath(), base::FilePath());
  if (!binding->bound.TimedWait(base::Seconds(10)) || !binding->port.load()) {
    content::DevToolsAgentHost::StopRemoteDebuggingServer();
    std::ignore = endpoint.directory.Delete();
    *error = "cannot listen on " + address + ":" + base::NumberToString(port);
    return 0;
  }
  ++endpoint.generation;
  endpoint.port = binding->port.load();
  endpoint.address = address;
  endpoint.asked_port = port;
  endpoint.url.clear();
  return endpoint.port;
}

void CdpListenAsync(const std::string& address_in, unsigned port, zebkit_cdp_listen_func func, void* data) {
  Endpoint& endpoint = Current();
  const std::string address = address_in.empty() ? "127.0.0.1" : address_in;
  std::string error;
  // Always after the call returns, as every answer of the engine's.
  auto tell_later = [&](unsigned port, std::string url, std::string error) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(
                       [](zebkit_cdp_listen_func func, void* data, unsigned port, std::string url, std::string error) {
                         func(port, url.empty() ? nullptr : url.c_str(), error.empty() ? nullptr : error.c_str(), data);
                       },
                       func, data, port, std::move(url), std::move(error)));
  };
  if (!CheckRequest(address, port, &error)) {
    tell_later(0, std::string(), error);
    return;
  }
  if (endpoint.port && !endpoint.url.empty()) {
    tell_later(endpoint.port, endpoint.url, std::string());
    return;
  }
  endpoint.waiting.push_back({func, data});
  if (endpoint.port) {
    // Served by the blocking form, its address not read yet.
    if (endpoint.waiting.size() == 1)
      ReadUrlSoon(endpoint.generation, 0);
    return;
  }
  if (endpoint.starting)
    return;
  endpoint.starting = true;
  endpoint.address = address;
  endpoint.asked_port = port;
  const uint64_t generation = ++endpoint.generation;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce([] {
        base::ScopedTempDir directory;
        std::ignore = directory.CreateUniqueTempDir();
        return directory;
      }),
      base::BindOnce(
          [](uint64_t generation, base::ScopedTempDir directory) {
            Endpoint& endpoint = Current();
            if (endpoint.generation != generation || !endpoint.starting) {
              DeleteSoon(std::move(directory));
              return;
            }
            if (!directory.IsValid()) {
              endpoint.starting = false;
              TellWaiters(0, std::string(), "cannot create a temporary directory");
              return;
            }
            endpoint.directory = std::move(directory);
            auto binding = std::make_shared<Binding>();
            binding->reply_runner = content::GetUIThreadTaskRunner({});
            binding->reply = base::BindRepeating(&Bound, generation);
            content::DevToolsAgentHost::StartRemoteDebuggingServer(
                std::make_unique<SocketFactory>(endpoint.address, static_cast<uint16_t>(endpoint.asked_port),
                                                binding),
                endpoint.directory.GetPath(), base::FilePath());
            // As the blocking form: ten seconds for the server's thread to bind.
            content::GetUIThreadTaskRunner({})->PostDelayedTask(FROM_HERE, base::BindOnce(&Bound, generation, 0u),
                                                                base::Seconds(10));
          },
          generation));
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
  // Waits for the server to write it: zebkit_cdp_listen_async() gives it
  // without waiting.
  base::ScopedAllowBlockingForTesting allow_blocking;
  for (int attempt = 0; attempt < 200; ++attempt) {
    endpoint.url = ReadUrl(endpoint.directory.GetPath(), endpoint.address, endpoint.port);
    if (!endpoint.url.empty())
      return endpoint.url;
    base::PlatformThread::Sleep(base::Milliseconds(10));
  }
  return std::string();
}

void CdpStop() {
  Endpoint& endpoint = Current();
  if (!endpoint.port && !endpoint.starting)
    return;
  // The server's own connections close with it; the views' sessions and
  // automation's are not the endpoint's to end.
  content::DevToolsAgentHost::StopRemoteDebuggingServer();
  ++endpoint.generation;
  const bool starting = endpoint.starting;
  endpoint.starting = false;
  if (endpoint.directory.IsValid())
    DeleteSoon(std::move(endpoint.directory));
  endpoint.port = 0;
  endpoint.url.clear();
  if (starting || !endpoint.waiting.empty())
    TellWaiters(0, std::string(), "CDP was stopped");
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
                         "remote-debugging-address"),
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
  if (!zebkit::CurrentlyOnEngineThread()) {
    if (error)
      *error = strdup("zebkit_cdp_listen is for the engine's thread");
    return 0;
  }
  std::string message;
  const unsigned bound =
      zebkit::CdpListen(address ? address : "", port, &message);
  if (error)
    *error = bound ? nullptr : strdup(message.c_str());
  return bound;
}

unsigned zebkit_cdp_get_port(void) {
  if (!zebkit::CurrentlyOnEngineThread())
    return 0;
  return zebkit::CdpPort();
}

void zebkit_cdp_listen_async(const char* address, unsigned port, zebkit_cdp_listen_func func, void* data) {
  if (!func)
    return;
  if (!zebkit::CurrentlyOnEngineThread()) {
    func(0, nullptr, "zebkit_cdp_listen_async is for the engine's thread", data);
    return;
  }
  zebkit::CdpListenAsync(address ? address : "", port, func, data);
}

char* zebkit_cdp_get_url(void) {
  if (!zebkit::CurrentlyOnEngineThread())
    return nullptr;
  const std::string url = zebkit::CdpUrl();
  return url.empty() ? nullptr : strdup(url.c_str());
}

void zebkit_cdp_stop(void) {
  if (zebkit::CurrentlyOnEngineThread())
    zebkit::CdpStop();
}

}  // extern "C"
