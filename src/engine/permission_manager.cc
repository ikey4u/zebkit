// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zebkit/engine/permission_manager.h"

#include "base/no_destructor.h"

#include "base/command_line.h"
#include "base/functional/callback.h"
#include "components/content_settings/core/common/features.h"
#include "components/permissions/permission_util.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/permission_result.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/content_switches.h"
#include "media/base/media_switches.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/origin.h"

using blink::PermissionType;

namespace zebkit {

namespace {

bool IsAllowlistedPermissionType(PermissionType permission) {
  switch (permission) {
    // zebkit: only what tells nothing about the person or their machine is
    // granted without asking. A browser would ask; an engine has no prompt,
    // so the rest is denied unless the application or an agent grants it
    // (CDP's Browser.setPermission, which overrides this).
    case PermissionType::WAKE_LOCK_SCREEN:
      return true;

    // zebkit: denied by default. Location (the device's Wi-Fi networks are
    // looked up for it), motion sensors, whether the person is idle, NFC,
    // payment handlers, work after the page is closed, MIDI devices.
    case PermissionType::GEOLOCATION:
    case PermissionType::GEOLOCATION_APPROXIMATE:
    case PermissionType::SENSORS:
    case PermissionType::PAYMENT_HANDLER:
    case PermissionType::BACKGROUND_SYNC:
    case PermissionType::BACKGROUND_FETCH:
    case PermissionType::PERIODIC_BACKGROUND_SYNC:
    case PermissionType::IDLE_DETECTION:
    case PermissionType::NFC:
    case PermissionType::MIDI:
    case PermissionType::MIDI_SYSEX:
    case PermissionType::NOTIFICATIONS:
    case PermissionType::PROTECTED_MEDIA_IDENTIFIER:
    case PermissionType::PERSISTENT_STORAGE:
    case PermissionType::AUDIO_CAPTURE:
    case PermissionType::VIDEO_CAPTURE:
    case PermissionType::CLIPBOARD_READ_WRITE:
    case PermissionType::CLIPBOARD_SANITIZED_WRITE:
    case PermissionType::NUM:
    case PermissionType::WAKE_LOCK_SYSTEM:
    case PermissionType::HAND_TRACKING:
    case PermissionType::VR:
    case PermissionType::AR:
    case PermissionType::STORAGE_ACCESS_GRANT:
    case PermissionType::CAMERA_PAN_TILT_ZOOM:
    case PermissionType::WINDOW_MANAGEMENT:
    case PermissionType::LOCAL_FONTS:
    case PermissionType::DISPLAY_CAPTURE:
    case PermissionType::TOP_LEVEL_STORAGE_ACCESS:
    case PermissionType::CAPTURED_SURFACE_CONTROL:
    case PermissionType::SMART_CARD:
    case PermissionType::WEB_PRINTING:
    case PermissionType::SPEAKER_SELECTION:
    case PermissionType::KEYBOARD_LOCK:
    case PermissionType::POINTER_LOCK:
    case PermissionType::AUTOMATIC_FULLSCREEN:
    case PermissionType::WEB_APP_INSTALLATION:
    case PermissionType::LOCAL_NETWORK_ACCESS:
    case PermissionType::LOCAL_NETWORK:
    case PermissionType::LOOPBACK_NETWORK:
      return false;
  }

  NOTREACHED();
}

}  // namespace

PermissionManager::PermissionManager() = default;

PermissionManager::~PermissionManager() = default;


void PermissionManager::ResetPermission(
    PermissionType permission,
    const GURL& requesting_origin,
    const GURL& embedding_origin) {
}

namespace {
struct DocumentPermissionHooks {
  PermissionManager::DocumentStatus status;
  PermissionManager::DocumentRequest request;
};
DocumentPermissionHooks& DocumentHooks() {
  static base::NoDestructor<DocumentPermissionHooks> hooks;
  return *hooks;
}
PermissionManager::EmbedderDecision& EmbedderDecisionForShell() {
  static base::NoDestructor<PermissionManager::EmbedderDecision> decision;
  return *decision;
}

std::optional<blink::mojom::PermissionStatus> EmbedderStatus(
    PermissionType permission,
    const GURL& requesting_origin) {
  const PermissionManager::EmbedderDecision& decision =
      EmbedderDecisionForShell();
  return decision ? decision.Run(permission, requesting_origin) : std::nullopt;
}
}  // namespace

void PermissionManager::SetEmbedderDecision(EmbedderDecision decision) {
  EmbedderDecisionForShell() = std::move(decision);
}

void PermissionManager::SetDocumentPermissionHooks(DocumentStatus status, DocumentRequest request) {
  DocumentHooks().status = std::move(status);
  DocumentHooks().request = std::move(request);
}

void PermissionManager::RequestPermissionsFromCurrentDocument(
    content::RenderFrameHost* render_frame_host,
    const PermissionRequestDescription& request_description,
    base::OnceCallback<void(const std::vector<PermissionResult>&)> callback) {
  if (render_frame_host->IsNestedWithinFencedFrame()) {
    std::move(callback).Run(std::vector<PermissionResult>(
        request_description.permissions.size(),
        PermissionResult(blink::mojom::PermissionStatus::DENIED)));
    return;
  }
  if (DocumentHooks().request &&
      DocumentHooks().request.Run(render_frame_host, request_description, &callback))
    return;
  std::vector<PermissionResult> result;
  blink::PermissionType permission_type;
  for (const auto& permission : request_description.permissions) {
    permission_type = blink::PermissionDescriptorToPermissionType(permission);
    if (std::optional<blink::mojom::PermissionStatus> status = EmbedderStatus(
            permission_type,
            render_frame_host->GetLastCommittedOrigin().GetURL())) {
      result.emplace_back(*status);
      continue;
    }
    // When the `ApproximateGeolocationPermission` feature is enabled, granting
    // geolocation requires more granular control via `GeolocationSetting`.
    if (base::FeatureList::IsEnabled(
            content_settings::features::kApproximateGeolocationPermission) &&
        permission_type == blink::PermissionType::GEOLOCATION &&
        IsAllowlistedPermissionType(permission_type)) {
      GeolocationSetting setting = {PermissionOption::kAllowed,
                                    PermissionOption::kAllowed};
      result.emplace_back(blink::mojom::PermissionStatus::GRANTED,
                          PermissionStatusSource::UNSPECIFIED, setting);
    } else {
      result.emplace_back(IsAllowlistedPermissionType(permission_type)
                              ? blink::mojom::PermissionStatus::GRANTED
                              : blink::mojom::PermissionStatus::DENIED);
    }
  }
  std::move(callback).Run(result);
}

blink::mojom::PermissionStatus PermissionManager::GetPermissionStatus(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    const GURL& requesting_origin,
    const GURL& embedding_origin) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  const auto permission_type =
      blink::PermissionDescriptorToPermissionType(permission_descriptor);

  if ((permission_type == PermissionType::AUDIO_CAPTURE ||
       permission_type == PermissionType::VIDEO_CAPTURE) &&
      command_line->HasSwitch(switches::kUseFakeDeviceForMediaStream) &&
      command_line->HasSwitch(switches::kUseFakeUIForMediaStream) &&
      command_line->GetSwitchValueASCII(
          switches::kUseFakeDeviceForMediaStream) != "deny") {
    return blink::mojom::PermissionStatus::GRANTED;
  }

  if (std::optional<blink::mojom::PermissionStatus> status =
          EmbedderStatus(permission_type, requesting_origin)) {
    return *status;
  }

  // zebkit: what the person has not decided reads as not decided ("prompt"),
  // as in Chrome, not as refused: no browser shows a site every permission
  // refused before it asked. A request is still turned down
  // (RequestPermissionsFromCurrentDocument), as a prompt the person
  // dismissed is, and nothing is kept. Notifications stay refused: there is
  // no notification service to show them, and with none Chromium tells
  // pages Notification.permission "denied", which the status agrees with.
  if (permission_type == PermissionType::NOTIFICATIONS)
    return blink::mojom::PermissionStatus::DENIED;
  return IsAllowlistedPermissionType(permission_type)
             ? blink::mojom::PermissionStatus::GRANTED
             : blink::mojom::PermissionStatus::ASK;
}

PermissionResult
PermissionManager::GetPermissionResultForOriginWithoutContext(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    const url::Origin& requesting_origin,
    const url::Origin& embedding_origin) {
  blink::mojom::PermissionStatus status =
      GetPermissionStatus(permission_descriptor, requesting_origin.GetURL(),
                          embedding_origin.GetURL());

  return PermissionResult(status);
}

PermissionResult PermissionManager::GetPermissionResultForCurrentDocument(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    content::RenderFrameHost* render_frame_host,
    bool should_include_device_status) {
  if (render_frame_host->IsNestedWithinFencedFrame())
    return PermissionResult(blink::mojom::PermissionStatus::DENIED);
  if (DocumentHooks().status) {
    auto status = DocumentHooks().status.Run(render_frame_host,
        blink::PermissionDescriptorToPermissionType(permission_descriptor),
        permissions::PermissionUtil::GetLastCommittedOriginAsURL(render_frame_host),
        permissions::PermissionUtil::GetLastCommittedOriginAsURL(render_frame_host->GetMainFrame()));
    if (status) return PermissionResult(*status);
  }
  return PermissionResult(GetPermissionStatus(
      permission_descriptor,
      permissions::PermissionUtil::GetLastCommittedOriginAsURL(
          render_frame_host),
      permissions::PermissionUtil::GetLastCommittedOriginAsURL(
          render_frame_host->GetMainFrame())));
}

PermissionResult PermissionManager::GetPermissionResultForWorker(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    content::RenderProcessHost* render_process_host,
    const GURL& worker_origin) {
  return PermissionResult(
      GetPermissionStatus(permission_descriptor, worker_origin, worker_origin));
}

PermissionResult
PermissionManager::GetPermissionResultForEmbeddedRequester(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    content::RenderFrameHost* render_frame_host,
    const url::Origin& overridden_origin) {
  if (render_frame_host->IsNestedWithinFencedFrame()) {
    return PermissionResult(blink::mojom::PermissionStatus::DENIED);
  }
  if (DocumentHooks().status) {
    auto status = DocumentHooks().status.Run(render_frame_host,
        blink::PermissionDescriptorToPermissionType(permission_descriptor),
        overridden_origin.GetURL(),
        permissions::PermissionUtil::GetLastCommittedOriginAsURL(render_frame_host->GetMainFrame()));
    if (status) return PermissionResult(*status);
  }
  return PermissionResult(GetPermissionStatus(
      permission_descriptor, overridden_origin.GetURL(),
      permissions::PermissionUtil::GetLastCommittedOriginAsURL(
          render_frame_host->GetMainFrame())));
}

}  // namespace zebkit
