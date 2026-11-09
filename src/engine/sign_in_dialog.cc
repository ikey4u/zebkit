#include "zebkit/engine/sign_in_dialog.h"

#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/engine.h"

namespace zebkit {

namespace {

class SignInDelegate : public content::LoginDelegate {
 public:
  SignInDelegate() = default;
  void set_callback(LoginAuthRequiredCallback callback) { callback_ = std::move(callback); }
  base::WeakPtr<SignInDelegate> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  void Answer(std::optional<std::pair<std::u16string, std::u16string>> credentials) {
    auto callback = std::move(callback_);
    if (!callback)
      return;
    if (credentials)
      std::move(callback).Run(net::AuthCredentials(credentials->first, credentials->second));
    else
      std::move(callback).Run(std::nullopt);
  }

 private:
  LoginAuthRequiredCallback callback_;
  base::WeakPtrFactory<SignInDelegate> weak_factory_{this};
};

}  // namespace

std::unique_ptr<content::LoginDelegate> CreateSignInDialogDelegate(
    const net::AuthChallengeInfo& challenge, content::WebContents* contents, const GURL& url,
    content::LoginDelegate::LoginAuthRequiredCallback& callback) {
  if (!contents || HasNoDisplay(contents))
    return nullptr;
  // The callback only once the dialog is shown: the caller keeps it otherwise.
  // The dialogs answer later, never from inside ShowSignInDialog().
  auto delegate = std::make_unique<SignInDelegate>();
  SignInAnswer answer = base::BindOnce(
      [](base::WeakPtr<SignInDelegate> delegate,
         std::optional<std::pair<std::u16string, std::u16string>> credentials) {
        if (delegate)
          delegate->Answer(std::move(credentials));
      },
      delegate->GetWeakPtr());
  const std::u16string host = base::UTF8ToUTF16(challenge.challenger.Serialize());
  std::u16string message = (challenge.is_proxy ? u"The proxy " : u"") + host + u" asks you to sign in.";
  if (!challenge.realm.empty())
    message += u"\n" + base::UTF8ToUTF16(challenge.realm);
  if (!ShowSignInDialog(contents, u"Sign in", message, answer))
    return nullptr;
  delegate->set_callback(std::move(callback));
  return delegate;
}

}  // namespace zebkit
