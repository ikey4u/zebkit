// What a profile's network keeps between runs (http_cache.h), and
// zebkit_clear_http_cache() (<zebkit/services.h>).

#include "zebkit/engine/http_cache.h"

#include <memory>
#include <string>

#include "base/barrier_closure.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "services/network/public/cpp/transferable_directory.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/window.h"

namespace zebkit {

void ConfigureNetworkPersistence(content::BrowserContext* context,
                                 bool in_memory,
                                 const base::FilePath& partition,
                                 network::mojom::NetworkContextParams* params) {
  const base::CommandLine& command = *base::CommandLine::ForCurrentProcess();
  // startup_settings.cc has checked both.
  const std::string storage = command.GetSwitchValueASCII("zebkit-http-cache");
  int size = 0;
  if (base::StringToInt(command.GetSwitchValueASCII("zebkit-http-cache-size"), &size) && size >= 0)
    params->http_cache_max_size = size;
  if (storage == "off")
    params->http_cache_enabled = false;
  // A context, or a profile in memory: nothing on disk, as an incognito
  // window keeps nothing.
  if (in_memory || context->IsOffTheRecord() || context->GetPath().empty())
    return;
  const base::FilePath directory = context->GetPath().Append(partition);
  const base::FilePath network = directory.AppendASCII("Network");
  // The files' writers do not make their directory; the cookie database's
  // does, but cookies may stay in memory. Long before the first write (the
  // stores write seconds after a change).
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
                             base::BindOnce(base::IgnoreResult(&base::CreateDirectory), network));
  if (!params->file_paths)
    params->file_paths = network::mojom::NetworkContextFilePaths::New();
  params->file_paths->data_directory = network::TransferableDirectory(network);
  // Chrome's names (chrome/common/chrome_constants.cc).
  params->file_paths->http_server_properties_file_name =
      base::FilePath(FILE_PATH_LITERAL("Network Persistent State"));
  params->file_paths->transport_security_persister_file_name =
      base::FilePath(FILE_PATH_LITERAL("TransportSecurity"));
  // Content makes it, and Cache_Data in it for the entries.
  if (storage.empty() || storage == "disk")
    params->file_paths->http_cache_directory =
        network::TransferableDirectory(directory.AppendASCII("Cache"));
}

}  // namespace zebkit

extern "C" bool zebkit_clear_http_cache(zebkit_context* context,
                                        zebkit_task_func done,
                                        void* data) {
  if (!zebkit::CurrentlyOnEngineThread() || zebkit::Window::IsShutDown())
    return false;
  if (context && !context->profile)
    return false;
  content::BrowserContext* profile = zebkit::ProfileOf(context);
  if (!profile)
    return false;
  std::vector<content::StoragePartition*> partitions;
  profile->ForEachLoadedStoragePartition(
      [&](content::StoragePartition* partition) { partitions.push_back(partition); });
  if (partitions.empty())
    partitions.push_back(profile->GetDefaultStoragePartition());
  // Called once both are done in every partition, also when a network
  // context goes away first (its callbacks then run as it closes).
  base::RepeatingClosure barrier = base::BarrierClosure(
      partitions.size() * 2, base::BindOnce(
                                 [](zebkit_task_func done, void* data) {
                                   if (done)
                                     done(data);
                                 },
                                 done, data));
  for (content::StoragePartition* partition : partitions) {
    network::mojom::NetworkContext* network = partition->GetNetworkContext();
    // The cache, and what the network keeps of the servers it met: their
    // HTTP/2 and alternative services, and HSTS (ClearNetworkingHistoryBetween).
    network->ClearHttpCache(base::Time(), base::Time::Max(), nullptr,
                            mojo::WrapCallbackWithDefaultInvokeIfNotRun(base::OnceClosure(barrier)));
    network->ClearNetworkingHistoryBetween(
        base::Time(), base::Time::Max(),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(base::OnceClosure(barrier)));
  }
  return true;
}
