#pragma once
#include <obs.h>
#include <string>

namespace ebot {
inline constexpr const char *PLUGIN_CLIENT = "plugin";
// Метод релею → JSON-рядок результату; помилка — std::runtime_error.
std::string dispatch(const std::string &method, obs_data_t *params);
// Поточна програмна сцена (у studio mode — програма, не прев'ю); "" — сцени нема.
std::string current_scene_name();
// Один раз у post_load: знаходить proc-хендлер obs-websocket.
void bridge_init();
// Від'єднує всі лічильники мікрофона (вихід OBS, зміна колекції сцен).
void bridge_shutdown();
} // namespace ebot
