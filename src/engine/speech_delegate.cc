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
  // In content_shell, we expect speech recognition to happen when requested.
  // Therefore we simply authorize it by calling back with is_allowed=true. The
  // first parameter, ask_user, is set to false because we don't want to prompt
  // the user for permission with an infobar.
  GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), false, true));
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
