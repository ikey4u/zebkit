#ifndef ZEBKIT_ENGINE_HTTP_AUTH_H_
#define ZEBKIT_ENGINE_HTTP_AUTH_H_

#include <memory>
#include "content/public/browser/login_delegate.h"

class GURL;
struct zebkit_view;
namespace content { class WebContents; }

namespace zebkit {
void InstallHttpAuthHooks();
void StopHttpAuthHooks();
void CancelHttpAuthRequests(zebkit_view* view, const char* reason);
// Leaves callback untouched when no native handler owns this challenge.
std::unique_ptr<content::LoginDelegate> CreateHttpAuthDelegate(
    const net::AuthChallengeInfo& challenge, content::WebContents* contents,
    const GURL& url, bool primary_main_frame, bool navigation, bool first_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback& callback);
}
#endif
