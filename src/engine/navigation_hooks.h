#ifndef ZEBKIT_ENGINE_NAVIGATION_HOOKS_H_
#define ZEBKIT_ENGINE_NAVIGATION_HOOKS_H_
#include <memory>
struct zebkit_view;
namespace content {
class NavigationThrottleRegistry;
class NavigationHandle;
class CommitDeferringCondition;
}
namespace zebkit {
void InstallNavigationHooks();
void StopNavigationHooks();
void AddNavigationPolicyThrottle(content::NavigationThrottleRegistry& registry);
std::unique_ptr<content::CommitDeferringCondition> CreateNavigationActivationPolicy(
    content::NavigationHandle* handle, bool prerender);
void CancelNavigationRequests(zebkit_view* view, const char* reason);
}
#endif
