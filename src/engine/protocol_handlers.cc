// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/protocol_handlers.h"

#include <memory>

#include "base/no_destructor.h"
#include "build/build_config.h"
#include "components/custom_handlers/protocol_handler_registry.h"
#include "zebkit/engine/protocol_handler_delegate.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/browser/browser_context.h"

namespace zebkit {

// static
ProtocolHandlers*
ProtocolHandlers::GetInstance() {
  static base::NoDestructor<ProtocolHandlers> factory;
  return factory.get();
}

// static
std::unique_ptr<KeyedService> BuildProtocolHandlerRegistryService(
    content::BrowserContext* context) {
  // We can't ensure the UserPref has been set, so we pass a nullptr
  // PrefService.
  return custom_handlers::ProtocolHandlerRegistry::Create(
      nullptr, std::make_unique<ProtocolHandlerDelegate>(),
      context->IsOffTheRecord());
}

// static
BrowserContextKeyedServiceFactory::TestingFactory
ProtocolHandlers::GetDefaultFactory() {
  return base::BindRepeating(&BuildProtocolHandlerRegistryService);
}

// static
custom_handlers::ProtocolHandlerRegistry*
ProtocolHandlers::GetForBrowserContext(
    content::BrowserContext* context,
    bool create) {
  return static_cast<custom_handlers::ProtocolHandlerRegistry*>(
      GetInstance()->GetServiceForBrowserContext(context, create));
}

ProtocolHandlers::ProtocolHandlers()
    : BrowserContextKeyedServiceFactory(
          "ProtocolHandlerRegistry",
          BrowserContextDependencyManager::GetInstance()) {}

// Will be created when initializing profile_io_data, so we might
// as well have the framework create this along with other
// PKSs to preserve orderly civic conduct :)
bool ProtocolHandlers::ServiceIsCreatedWithBrowserContext()
    const {
  return true;
}

// Do not create this service for tests. MANY tests will fail
// due to the threading requirements of this service. ALSO,
// not creating this increases test isolation (which is GOOD!)
bool ProtocolHandlers::ServiceIsNULLWhileTesting() const {
  return true;
}

std::unique_ptr<KeyedService>
ProtocolHandlers::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return BuildProtocolHandlerRegistryService(context);
}

}  // namespace zebkit
