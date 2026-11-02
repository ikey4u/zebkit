// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZEBKIT_ENGINE_PERMISSION_MANAGER_H_
#define ZEBKIT_ENGINE_PERMISSION_MANAGER_H_

#include "zebkit/engine/content_names.h"
#include <optional>

#include "base/functional/callback.h"
#include "content/public/browser/permission_controller_delegate.h"
#include "content/public/browser/permission_result.h"

namespace blink {
enum class PermissionType;
}

namespace zebkit {

class PermissionManager : public PermissionControllerDelegate {
 public:
  PermissionManager();

  PermissionManager(const PermissionManager&) = delete;
  PermissionManager& operator=(const PermissionManager&) = delete;

  ~PermissionManager() override;

  // zebkit: the embedder's answer for @permission at @requesting_origin,
  // before the shell's own: granted, denied, or nullopt to leave it to the
  // shell (Playwright's grantPermissions).
  using EmbedderDecision =
      base::RepeatingCallback<std::optional<blink::mojom::PermissionStatus>(
          blink::PermissionType permission,
          const GURL& requesting_origin)>;
  static void SetEmbedderDecision(EmbedderDecision decision);

  using DocumentStatus = base::RepeatingCallback<
      std::optional<blink::mojom::PermissionStatus>(RenderFrameHost*,
          blink::PermissionType, const GURL&, const GURL&)>;
  using RequestReply = base::OnceCallback<void(const std::vector<PermissionResult>&)>;
  // Return true only after taking ownership of *reply. false preserves the
  // normal shell decision. Called after fenced-frame rejection.
  using DocumentRequest = base::RepeatingCallback<bool(
      RenderFrameHost*, const PermissionRequestDescription&, RequestReply*)>;
  static void SetDocumentPermissionHooks(DocumentStatus status, DocumentRequest request);


  // PermissionManager implementation.
  void ResetPermission(blink::PermissionType permission,
                       const GURL& requesting_origin,
                       const GURL& embedding_origin) override;
  void RequestPermissionsFromCurrentDocument(
      RenderFrameHost* render_frame_host,
      const PermissionRequestDescription& request_description,
      base::OnceCallback<void(const std::vector<PermissionResult>&)> callback)
      override;
  blink::mojom::PermissionStatus GetPermissionStatus(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      const GURL& requesting_origin,
      const GURL& embedding_origin) override;
  PermissionResult GetPermissionResultForOriginWithoutContext(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      const url::Origin& requesting_origin,
      const url::Origin& embedding_origin) override;
  PermissionResult GetPermissionResultForCurrentDocument(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderFrameHost* render_frame_host,
      bool should_include_device_status) override;
  PermissionResult GetPermissionResultForWorker(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderProcessHost* render_process_host,
      const GURL& worker_origin) override;
  PermissionResult GetPermissionResultForEmbeddedRequester(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderFrameHost* render_frame_host,
      const url::Origin& overridden_origin) override;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PERMISSION_MANAGER_H_
