#include "zebkit/engine/permission_types.h"
#include "base/strings/string_util.h"
namespace zebkit {
namespace {
using P = blink::PermissionType;
struct Named { const char* name; P type; };
constexpr Named kNames[] = {
    {"midi-sysex", P::MIDI_SYSEX}, {"notifications", P::NOTIFICATIONS},
    {"geolocation", P::GEOLOCATION}, {"geolocation-approximate", P::GEOLOCATION_APPROXIMATE},
    {"protected-media-identifier", P::PROTECTED_MEDIA_IDENTIFIER}, {"midi", P::MIDI},
    {"persistent-storage", P::PERSISTENT_STORAGE}, {"microphone", P::AUDIO_CAPTURE},
    {"camera", P::VIDEO_CAPTURE}, {"background-sync", P::BACKGROUND_SYNC},
    {"sensors", P::SENSORS}, {"clipboard-read", P::CLIPBOARD_READ_WRITE},
    {"clipboard-write", P::CLIPBOARD_SANITIZED_WRITE}, {"payment-handler", P::PAYMENT_HANDLER},
    {"background-fetch", P::BACKGROUND_FETCH}, {"idle-detection", P::IDLE_DETECTION},
    {"periodic-background-sync", P::PERIODIC_BACKGROUND_SYNC}, {"screen-wake-lock", P::WAKE_LOCK_SCREEN},
    {"system-wake-lock", P::WAKE_LOCK_SYSTEM}, {"nfc", P::NFC}, {"vr", P::VR}, {"ar", P::AR},
    {"hand-tracking", P::HAND_TRACKING}, {"smart-card", P::SMART_CARD},
    {"storage-access", P::STORAGE_ACCESS_GRANT}, {"camera-pan-tilt-zoom", P::CAMERA_PAN_TILT_ZOOM},
    {"window-management", P::WINDOW_MANAGEMENT}, {"local-fonts", P::LOCAL_FONTS},
    {"display-capture", P::DISPLAY_CAPTURE}, {"top-level-storage-access", P::TOP_LEVEL_STORAGE_ACCESS},
    {"captured-surface-control", P::CAPTURED_SURFACE_CONTROL}, {"web-printing", P::WEB_PRINTING},
    {"speaker-selection", P::SPEAKER_SELECTION}, {"keyboard-lock", P::KEYBOARD_LOCK},
    {"pointer-lock", P::POINTER_LOCK}, {"automatic-fullscreen", P::AUTOMATIC_FULLSCREEN},
    {"web-app-installation", P::WEB_APP_INSTALLATION}, {"local-network-access", P::LOCAL_NETWORK_ACCESS},
    {"local-network", P::LOCAL_NETWORK}, {"loopback-network", P::LOOPBACK_NETWORK},
};
}
std::optional<P> PermissionTypeNamed(std::string_view name) {
  for (auto entry : kNames) if (name == entry.name) return entry.type;
  if (name == "ambient-light-sensor" || name == "accelerometer" || name == "gyroscope" || name == "magnetometer")
    return P::SENSORS;
  for (auto type : blink::GetAllPermissionTypes())
    if (base::EqualsCaseInsensitiveASCII(name, blink::GetPermissionString(type))) return type;
  return std::nullopt;
}
std::string PermissionName(P type) {
  for (auto entry : kNames) if (entry.type == type) return entry.name;
  return base::ToLowerASCII(blink::GetPermissionString(type));
}
}
