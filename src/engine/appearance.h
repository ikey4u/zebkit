// What the system says of how things should look, and the pages following it
// (docs/SYSTEM_INTEGRATION.md, section 11): its colour scheme, contrast,
// forced colours and reduced motion, read on each system the way Chrome reads
// them, and told to the application (<zebkit/zebkit.h>'s environment).

#ifndef ZEBKIT_ENGINE_APPEARANCE_H_
#define ZEBKIT_ENGINE_APPEARANCE_H_

#include "third_party/blink/public/common/web_preferences/web_preferences.h"

namespace zebkit {

// The startup setting appearance.follow (--zebkit-follow-system-appearance):
// pages take the system's colour scheme and contrast, and their changes.
bool FollowsSystemAppearance();
// Once, as the browser starts, whether or not the pages follow the system: on
// Linux the XDG portal's settings, which macOS and Windows have in their
// NativeTheme already.
void StartSystemAppearance();
void StopSystemAppearance();
// The system's colour scheme, contrast and forced colours into @prefs.
void ApplySystemAppearance(blink::web_pref::WebPreferences* prefs);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_APPEARANCE_H_
