// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_ORIGIN_TRIAL_POLICY_H_
#define ZEBKIT_ENGINE_ORIGIN_TRIAL_POLICY_H_

#include "zebkit/engine/content_names.h"
#include <vector>

#include "third_party/blink/public/common/origin_trials/origin_trial_policy.h"

namespace zebkit {

class OriginTrialPolicyImpl : public blink::OriginTrialPolicy {
 public:
  OriginTrialPolicyImpl();

  OriginTrialPolicyImpl(const OriginTrialPolicyImpl&) = delete;
  OriginTrialPolicyImpl& operator=(const OriginTrialPolicyImpl&) = delete;

  ~OriginTrialPolicyImpl() override;

  // blink::OriginTrialPolicy interface
  bool IsOriginTrialsSupported() const override;
  const std::vector<blink::OriginTrialPublicKey>& GetPublicKeys()
      const override;
  bool IsOriginSecure(const GURL& url) const override;

 private:
  std::vector<blink::OriginTrialPublicKey> public_keys_;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_ORIGIN_TRIAL_POLICY_H_
