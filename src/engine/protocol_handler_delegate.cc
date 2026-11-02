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

void ProtocolHandlerDelegate::RegisterWithOSAsDefaultClient(
    const std::string& protocol,
    DefaultClientCallback callback) {
  // Do as-if the registration has to run on another sequence and post back
  // the result with a task to the current thread.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), !force_os_failure_));

  if (!force_os_failure_)
    os_registered_protocols_.insert(protocol);
}

void ProtocolHandlerDelegate::CheckDefaultClientWithOS(
    const std::string& protocol,
    DefaultClientCallback callback) {
  // Respond asynchronously to mimic the real behavior.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), true));
}

bool ProtocolHandlerDelegate::ShouldRemoveHandlersNotInOS() {
  return true;
}

bool ProtocolHandlerDelegate::IsFakeRegisteredWithOS(
    const std::string& protocol) {
  return os_registered_protocols_.find(protocol) !=
         os_registered_protocols_.end();
}

void ProtocolHandlerDelegate::Reset() {
  registered_protocols_.clear();
  os_registered_protocols_.clear();
  force_os_failure_ = false;
}

}  // namespace zebkit
