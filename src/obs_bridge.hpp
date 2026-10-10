#pragma once
#include <obs.h>
#include <string>

namespace ebot {
inline constexpr const char *PLUGIN_CLIENT = "plugin";
// Метод релею → JSON-рядок результату; помилка — std::runtime_error.
std::string dispatch(const std::string &method, obs_data_t *params);
std::string current_scene_name();
// Від'єднує всі лічильники мікрофона (вихід OBS, зміна колекції сцен).
void bridge_shutdown();
} // namespace ebot
