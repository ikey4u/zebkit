// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/content_index_provider.h"

namespace zebkit {

ContentIndexProviderImpl::ContentIndexProviderImpl()
    : icon_sizes_({{96, 96}}) {}

ContentIndexProviderImpl::~ContentIndexProviderImpl() = default;

std::vector<gfx::Size> ContentIndexProviderImpl::GetIconSizes(
    blink::mojom::ContentCategory category) {
  return icon_sizes_;
}

void ContentIndexProviderImpl::OnContentAdded(ContentIndexEntry entry) {
  entries_[entry.description->id] = {
      entry.service_worker_registration_id,
      url::Origin::Create(entry.launch_url.DeprecatedGetOriginAsURL())};
}

void ContentIndexProviderImpl::OnContentDeleted(
    int64_t service_worker_registration_id,
    const url::Origin& origin,
    const std::string& description_id) {
  entries_.erase(description_id);
}

std::pair<int64_t, url::Origin>
ContentIndexProviderImpl::GetRegistrationDataFromId(const std::string& id) {
  if (!entries_.count(id))
    return {-1, url::Origin()};
  return entries_[id];
}

}  // namespace zebkit
