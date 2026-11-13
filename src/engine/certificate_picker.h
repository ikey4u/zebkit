// The engine's picker of a client certificate (docs/SYSTEM_INTEGRATION.md, 9):
// when no function of the application's decides, the page has a window and
// the system's stores offer identities, the person chooses one, as in Chrome.

#ifndef ZEBKIT_ENGINE_CERTIFICATE_PICKER_H_
#define ZEBKIT_ENGINE_CERTIFICATE_PICKER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"

namespace content {
class WebContents;
}

namespace zebkit {

// An index into @names, or nothing for cancelled; answered after this
// returns. Returns what closes it unanswered when its request ends first
// (@answer is then never run); a null closure, and @answer the caller's, with
// no window.
using CertificateAnswer = base::OnceCallback<void(std::optional<size_t>)>;
base::OnceClosure ShowCertificatePicker(content::WebContents* contents, const std::u16string& host,
                                        const std::vector<std::u16string>& names, CertificateAnswer& answer);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_CERTIFICATE_PICKER_H_
