#ifndef ZEBKIT_ENGINE_PERMISSIONS_H_
#define ZEBKIT_ENGINE_PERMISSIONS_H_
struct zebkit_view;
namespace content { class RenderFrameHost; }
namespace zebkit {
void InstallPermissionHooks();
void StopPermissionHooks();
void CancelPermissionRequests(zebkit_view* view, const char* reason);
void CheckPermissionDocuments(zebkit_view* view);
void PermissionFrameDeleted(zebkit_view* view, content::RenderFrameHost* frame);
}
#endif
