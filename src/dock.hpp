#pragma once
namespace ebot {
// Док «єБот» у меню «Доки» OBS.
void create_dock();
// Події OBS для пульта: інтерфейс готовий (можна піднімати браузер), тема
// змінилась (пульт перевідкривається з новими кольорами), вихід (закрити браузер).
void dock_frontend_ready();
void dock_theme_changed();
void dock_shutdown();
} // namespace ebot
