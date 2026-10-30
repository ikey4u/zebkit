#ifndef ZEBKIT_ENGINE_PERMISSION_TYPES_H_
#define ZEBKIT_ENGINE_PERMISSION_TYPES_H_
#include <optional>
#include <string>
#include <string_view>
#include "third_party/blink/public/common/permissions/permission_utils.h"
namespace zebkit {
std::optional<blink::PermissionType> PermissionTypeNamed(std::string_view name);
std::string PermissionName(blink::PermissionType type);
}
#endif
