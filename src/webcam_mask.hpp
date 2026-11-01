// Фільтр «єБот: заокруглення камери» (0.5) і команда релею set_webcam_mask.
#pragma once
#include <obs.h>
#include <string>

namespace ebot {
// Один раз в obs_module_load.
void register_webcam_mask();
// {"source", "radius", "enabled", "crop"?: {left, top, right, bottom}} — px
// джерела. Додає, оновлює (і вмикає) або прибирає фільтр на джерелі; помилка —
// std::runtime_error у форматі OBS, як решта методів релею.
std::string set_webcam_mask(obs_data_t *params);
} // namespace ebot
