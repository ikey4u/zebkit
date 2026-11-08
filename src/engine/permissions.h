#ifndef ZEBKIT_ENGINE_PERMISSIONS_H_
#define ZEBKIT_ENGINE_PERMISSIONS_H_
#include <optional>
#include <string>
#include <string_view>
#include "base/functional/callback_forward.h"
#include "base/values.h"
struct zebkit_view;
namespace content { class RenderFrameHost; }
namespace zebkit {
// zebkit's own permissions, decisions with no blink::PermissionType:
// "file-system-read", "file-system-write" (File System Access handles beyond
// what the person picked) and "protocol-handler" (registerProtocolHandler).
// They are asked of the view's permission hook as the others are, with the
// same requests, rules (zebkit_view_set_permission), deadline and result
// events, and denied without a hook (docs/SYSTEM_INTEGRATION.md, 9 and 10).
bool IsNamedPermission(std::string_view name);
// What a named permission's request came to: the application's answer (or a
// rule of its), or none, when there is no hook, or it did not answer in time,
// or the page went away. None is refused, and leaves the request to be asked
// again: it is not a person's "no".
enum class NamedAnswer { kGranted, kDenied, kUndecided };
// @details is added to the request's metadata. @done runs once, on the
// engine's thread: at once for a rule or no hook, else when it is decided.
void RequestNamedPermission(content::RenderFrameHost* frame, std::string_view name,
                            base::DictValue details, bool user_gesture,
                            base::OnceCallback<void(NamedAnswer)> done);

void InstallPermissionHooks();
void StopPermissionHooks();
void CancelPermissionRequests(zebkit_view* view, const char* reason);
void CheckPermissionDocuments(zebkit_view* view);
void PermissionFrameDeleted(zebkit_view* view, content::RenderFrameHost* frame);
}
#endif
