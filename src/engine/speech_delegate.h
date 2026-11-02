// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_SPEECH_DELEGATE_H_
#define ZEBKIT_ENGINE_SPEECH_DELEGATE_H_

#include "zebkit/engine/content_names.h"
#include "base/functional/bind.h"
#include "content/public/browser/speech_recognition_event_listener.h"
#include "content/public/browser/speech_recognition_manager_delegate.h"

namespace zebkit {


// This is content_shell's delegate used by the speech recognition manager to
// check for permission to record audio. For content_shell, we always authorize
// speech recognition (see crbug.com/237119).
class SpeechDelegate
    : public SpeechRecognitionManagerDelegate {
 public:
  SpeechDelegate() {}

  SpeechDelegate(
      const SpeechDelegate&) = delete;
  SpeechDelegate& operator=(
      const SpeechDelegate&) = delete;

  ~SpeechDelegate() override {}

  // SpeechRecognitionManagerDelegate methods.
  void CheckRecognitionIsAllowed(
      int session_id,
      base::OnceCallback<void(bool ask_user, bool is_allowed)> callback)
      override;
  SpeechRecognitionEventListener* GetEventListener() override;
#if !BUILDFLAG(IS_FUCHSIA) && !BUILDFLAG(IS_ANDROID)
  // It is empty in this delegate.
  void BindSpeechRecognitionContext(
      mojo::PendingReceiver<media::mojom::SpeechRecognitionContext> receiver,
      const std::string& language,
      const GlobalRenderFrameHostId& render_frame_host_id) override;
#endif  //! BUILDFLAG(IS_FUCHSIA) && !BUILDFLAG(IS_ANDROID)
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_SPEECH_DELEGATE_H_
