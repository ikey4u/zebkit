// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/protocol_handler_delegate.h"

#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"

namespace zebkit {

ProtocolHandlerDelegate::ProtocolHandlerDelegate() =
    default;

ProtocolHandlerDelegate::~ProtocolHandlerDelegate() =
    default;

// ProtocolHandlerRegistry::Delegate:
void ProtocolHandlerDelegate::RegisterExternalHandler(
    const std::string& protocol) {
  bool inserted = registered_protocols_.insert(protocol).second;
  DCHECK(inserted);
}

void ProtocolHandlerDelegate::DeregisterExternalHandler(
    const std::string& protocol) {
  size_t removed = registered_protocols_.erase(protocol);
  DCHECK_EQ(removed, 1u);
}

bool ProtocolHandlerDelegate::IsExternalHandlerRegistered(
    const std::string& protocol) {
  return registered_protocols_.find(protocol) != registered_protocols_.end();
}

// zebkit: nothing is registered with the system, and nothing says it is: a
// scheme becomes the application's with the system through its installer
// (docs/SYSTEM_INTEGRATION.md, 15). Handlers stay in the profile's registry,
// where they handle the scheme's links in the engine's pages.
void ProtocolHandlerDelegate::RegisterWithOSAsDefaultClient(
    const std::string& protocol,
    DefaultClientCallback callback) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), false));
}

void ProtocolHandlerDelegate::CheckDefaultClientWithOS(
    const std::string& protocol,
    DefaultClientCallback callback) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), false));
}

bool ProtocolHandlerDelegate::ShouldRemoveHandlersNotInOS() {
  return false;
}

}  // namespace zebkit
