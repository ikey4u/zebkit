// The native UI pump, with scheduling notifications for the host's loop.
#ifndef ZEBKIT_ENGINE_HOST_LOOP_H_
#define ZEBKIT_ENGINE_HOST_LOOP_H_

#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {
void ConfigureHostWakeup(zebkit_wakeup_func func, void* data);
bool InstallHostLoop();
// Waits for any callback in progress; none can start after this returns.
void StopHostLoop();
}  // namespace zebkit
#endif
