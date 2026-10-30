#ifndef ZEBKIT_ENGINE_STARTUP_SETTINGS_H_
#define ZEBKIT_ENGINE_STARTUP_SETTINGS_H_
#include <string>
#include <vector>
namespace zebkit {
// Validates completely before publishing any arguments or changing state.
bool ApplyStartupSettings(const char* json, std::vector<std::string>* args,
                          bool* application_integration, std::string* error);
}  // namespace zebkit
#endif
