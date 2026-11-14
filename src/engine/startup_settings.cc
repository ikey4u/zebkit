// Startup settings without constructing URLs or initializing ICU before content.
#include "zebkit/engine/c_api_util.h"
#include "zebkit/engine/startup_settings.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string_view>

#include "net/http/http_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/values.h"

namespace zebkit {
namespace {

bool Fields(const base::DictValue& value,
            std::initializer_list<std::string_view> fields, std::string* error) {
  for (auto [key, ignored] : value) {
    if (std::find(fields.begin(), fields.end(), key) == fields.end())
      return SetError(error, "Unknown startup setting: " + key);
  }
  return true;
}

bool Text(const base::Value& value, std::string_view name, std::string* error) {
  return (value.is_string() && value.GetString().find('\0') == std::string::npos) ||
         SetError(error, std::string(name) + ": expected a string without NUL");
}

void Switch(std::vector<std::string>* args, std::string_view name,
            std::optional<std::string> value) {
  const std::string flag = "--" + std::string(name);
  std::erase_if(*args, [&](const std::string& arg) {
    return arg == flag || arg.starts_with(flag + "=");
  });
  if (value)
    args->push_back(flag + (value->empty() ? "" : "=" + *value));
}

bool StringSwitch(const base::DictValue& settings, const char* key,
                  const char* flag, bool path, std::vector<std::string>* args,
                  std::string* error) {
  const base::Value* value = settings.Find(key);
  if (!value)
    return true;
  if (!Text(*value, key, error))
    return false;
  const std::string& text = value->GetString();
  // Not through UTF-8: that depends on the locale, which is the application's.
  if (text.empty() || (path && !base::FilePath(base::FilePath::StringType(text.begin(), text.end())).IsAbsolute()))
    return SetError(error, std::string(key) + (path ? ": expected an absolute path" : ": must not be empty"));
  if (!strcmp(key, "userAgent") && !net::HttpUtil::IsValidHeaderValue(text))
    return SetError(error, "userAgent: not a valid header value");
  if (!strcmp(key, "proxy")) {
    net::ProxyConfig::ProxyRules rules;
    rules.ParseFromString(text);
    if (rules.empty() || rules.type == net::ProxyConfig::ProxyRules::Type::EMPTY)
      return SetError(error, "proxy: not a proxy list");
  }
  Switch(args, flag, text);
  return true;
}

bool BooleanSwitch(const base::DictValue& settings, const char* key,
                   const char* flag, bool inverted, std::vector<std::string>* args,
                   std::string* error) {
  const base::Value* value = settings.Find(key);
  if (!value)
    return true;
  if (!value->is_bool())
    return SetError(error, std::string(key) + ": expected a boolean");
  Switch(args, flag, value->GetBool() != inverted ? std::make_optional(std::string()) : std::nullopt);
  return true;
}

}  // namespace

bool ApplyStartupSettings(const char* json, std::vector<std::string>* args,
                          bool* application_integration, std::string* error) {
  *application_integration = false;
  if (!json)
    return true;
  auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return SetError(error, "Startup settings must be a JSON object");
  const auto& settings = parsed->GetDict();
  if (!Fields(settings, {"profileDirectory", "proxy", "userAgent", "headless",
                         "sandbox", "gpu", "logging", "cookies", "applicationIntegration", "appearance"}, error))
    return false;
  std::vector<std::string> result = *args;
  if (!StringSwitch(settings, "profileDirectory", "user-data-dir", true, &result, error) ||
      !StringSwitch(settings, "proxy", "proxy-server", false, &result, error) ||
      !StringSwitch(settings, "userAgent", "user-agent", false, &result, error) ||
      !BooleanSwitch(settings, "headless", "headless", false, &result, error) ||
      !BooleanSwitch(settings, "sandbox", "no-sandbox", true, &result, error))
    return false;
  if (settings.contains("proxy")) {
    for (const char* flag : {"no-proxy-server", "proxy-auto-detect", "proxy-pac-url"})
      Switch(&result, flag, std::nullopt);
  }
  if (const base::Value* gpu = settings.Find("gpu")) {
    if (!Text(*gpu, "gpu", error))
      return false;
    const auto& mode = gpu->GetString();
    if (mode != "auto" && mode != "disabled" && mode != "software")
      return SetError(error, "gpu: expected auto, disabled or software");
    for (const char* flag : {"disable-gpu", "use-gl", "use-angle", "enable-unsafe-swiftshader"})
      Switch(&result, flag, std::nullopt);
    if (mode == "disabled")
      Switch(&result, "disable-gpu", std::string());
    if (mode == "software") {
      Switch(&result, "use-gl", "angle");
      Switch(&result, "use-angle", "swiftshader-webgl");
      Switch(&result, "enable-unsafe-swiftshader", std::string());
    }
  }
  if (const base::Value* logging = settings.Find("logging")) {
    if (!logging->is_dict())
      return SetError(error, "logging: expected an object");
    const auto& log = logging->GetDict();
    if (!Fields(log, {"destination", "path", "level"}, error))
      return false;
    const base::Value* destination = log.Find("destination");
    if (!destination || !Text(*destination, "logging.destination", error))
      return SetError(error, "logging.destination: expected stderr, file or none");
    const auto& target = destination->GetString();
    if (target != "stderr" && target != "file" && target != "none")
      return SetError(error, "logging.destination: expected stderr, file or none");
    for (const char* flag : {"enable-logging", "disable-logging", "log-file"})
      Switch(&result, flag, std::nullopt);
    Switch(&result, target == "none" ? "disable-logging" : "enable-logging",
           target == "stderr" ? std::string("stderr") : std::string());
    if (target == "file") {
      if (!log.contains("path"))
        return SetError(error, "logging.path is required for file logging");
      if (!StringSwitch(log, "path", "log-file", true, &result, error))
        return false;
    } else if (log.contains("path")) {
      return SetError(error, "logging.path is only valid for file logging");
    }
    if (const base::Value* level = log.Find("level")) {
      if (!level->is_int() || level->GetInt() < 0 || level->GetInt() > 3)
        return SetError(error, "logging.level: expected an integer from 0 to 3");
      Switch(&result, "log-level", std::to_string(level->GetInt()));
    }
  }
  if (const base::Value* cookies = settings.Find("cookies")) {
    if (!cookies->is_dict())
      return SetError(error, "cookies: expected an object");
    const auto& cookie = cookies->GetDict();
    if (!Fields(cookie, {"storage", "keyDirectory", "thirdParty"}, error))
      return false;
    if (const base::Value* storage = cookie.Find("storage")) {
      if (!Text(*storage, "cookies.storage", error))
        return false;
      if (storage->GetString() != "os" && storage->GetString() != "file")
        return SetError(error, "cookies.storage: expected os or file");
      Switch(&result, "zebkit-cookie-storage", storage->GetString());
      if (storage->GetString() == "os")
        Switch(&result, "zebkit-cookie-key-directory", std::nullopt);
    }
    // Over what the settings and argv come to together.
    if (cookie.contains("keyDirectory")) {
      const std::string prefix = "--zebkit-cookie-storage=";
      bool file = false;
      for (const auto& arg : result)
        if (arg.starts_with(prefix)) file = arg.substr(prefix.size()) == "file";
      if (!file)
        return SetError(error, "cookies.keyDirectory requires file storage");
    }
    if (!StringSwitch(cookie, "keyDirectory", "zebkit-cookie-key-directory", true, &result, error) ||
        !BooleanSwitch(cookie, "thirdParty", "zebkit-allow-third-party-cookies", false, &result, error))
      return false;
  }
  bool integration = false;
  if (const base::Value* app = settings.Find("applicationIntegration")) {
    if (!app->is_bool())
      return SetError(error, "applicationIntegration: expected a boolean");
    integration = app->GetBool();
  }
  if (const base::Value* appearance = settings.Find("appearance")) {
    const base::DictValue* dict = appearance->GetIfDict();
    if (!dict)
      return SetError(error, "appearance: expected an object");
    for (auto [key, value] : *dict)
      if (key != "follow")
        return SetError(error, "appearance: unknown key " + key);
    if (const base::Value* follow = dict->Find("follow")) {
      if (!follow->is_bool())
        return SetError(error, "appearance.follow: expected a boolean");
      Switch(&result, "zebkit-follow-system-appearance",
             follow->GetBool() ? std::make_optional(std::string()) : std::nullopt);
    }
  }
  *application_integration = integration;
  *args = std::move(result);
  return true;
}
}  // namespace zebkit
