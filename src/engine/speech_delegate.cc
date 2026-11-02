// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/speech_delegate.h"

#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

using base::OnceCallback;

namespace zebkit {

void SpeechDelegate::CheckRecognitionIsAllowed(
    int session_id,
    OnceCallback<void(bool ask_user, bool is_allowed)> callback) {
  // The microphone's permission, as for getUserMedia: ask_user sends it through
  // WebContentsDelegate::RequestMediaAccessPermission, the application's hooks.
  GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), /*ask_user=*/true,
                                /*is_allowed=*/false));
}

SpeechRecognitionEventListener*
    SpeechDelegate::GetEventListener() {
  return nullptr;
}

#if !BUILDFLAG(IS_FUCHSIA) && !BUILDFLAG(IS_ANDROID)
void SpeechDelegate::BindSpeechRecognitionContext(
    mojo::PendingReceiver<media::mojom::SpeechRecognitionContext> receiver,
    const std::string& language,
    const GlobalRenderFrameHostId& render_frame_host_id) {}
#endif  // !BUILDFLAG(IS_FUCHSIA) && !BUILDFLAG(IS_ANDROID)

}  // namespace zebkit
