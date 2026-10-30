#ifndef ZEBKIT_ENGINE_CERTIFICATE_HOOKS_H_
#define ZEBKIT_ENGINE_CERTIFICATE_HOOKS_H_
#include "base/functional/callback.h"
#include "content/public/browser/certificate_request_result_type.h"
class GURL;
namespace content { class WebContents; }
namespace net { class SSLInfo; }
struct zebkit_view;
namespace zebkit {
void InstallCertificateHooks();
void StopCertificateHooks();
void CancelCertificateRequests(zebkit_view* view, const char* reason);
void FinishCertificateNavigation(zebkit_view* view, const GURL& url);
void HandleCertificateError(content::WebContents* contents, int error,
    const net::SSLInfo& info, const GURL& url, bool main_frame, bool strict,
    base::OnceCallback<void(content::CertificateRequestResultType)> reply);
}
#endif
