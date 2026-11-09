// The engine's sign-in dialog for HTTP authentication (docs/SYSTEM_INTEGRATION.md,
// 9): when neither the application (zebkit_view_on_http_auth) nor automation
// answers a challenge, and the page has a window, a username and a password
// asked of the person, as Chrome asks; without a window, cancelled as before.

#ifndef ZEBKIT_ENGINE_SIGN_IN_DIALOG_H_
#define ZEBKIT_ENGINE_SIGN_IN_DIALOG_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "content/public/browser/login_delegate.h"
#include "net/base/auth.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}

namespace zebkit {

std::unique_ptr<content::LoginDelegate> CreateSignInDialogDelegate(
    const net::AuthChallengeInfo& challenge, content::WebContents* contents, const GURL& url,
    content::LoginDelegate::LoginAuthRequiredCallback& callback);

// The platform's dialog (sign_in_dialog_views.cc, sign_in_dialog_mac.mm): the
// username and password, or nothing for cancelled. False with no window.
using SignInAnswer = base::OnceCallback<void(std::optional<std::pair<std::u16string, std::u16string>>)>;
bool ShowSignInDialog(content::WebContents* contents, const std::u16string& title,
                      const std::u16string& message, SignInAnswer& answer);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_SIGN_IN_DIALOG_H_
