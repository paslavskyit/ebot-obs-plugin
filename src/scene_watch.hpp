// Пуш {"type":"item_transform","scene","source"} (0.5): стример посунув,
// розтягнув, обрізав чи повернув елемент поточної програмної сцени. Сервер
// перечитує сцену (камера кліпера, рамка вебки) лише тоді, а не опитує OBS.
#pragma once

namespace ebot {
void scene_watch_start();  // post_load: tick-колбек дебаунсу
void scene_watch_bind();   // слухати поточну програмну сцену (та сама — нічого не робить)
void scene_watch_unbind(); // колекція сцен міняється: сцени от-от зникнуть
void scene_watch_stop();   // вихід OBS: відв'язка й tick-колбек геть (повторно — безпечно)
} // namespace ebot
