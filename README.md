# єБот для OBS

Плагін OBS Studio, який з'єднує ваш OBS з [єБотом](https://ebot.live): сцени, звук,
знімок сцени, сторож стріму, пауза нагород на сценах і бекапи працюють без пароля
WebSocket і без окремого вікна релею.

## Встановлення

1. Завантажте [`ebot-obs-setup.exe`](https://github.com/paslavskyit/ebot-obs-plugin/releases/latest/download/ebot-obs-setup.exe)
   і запустіть. Windows спитає — «Більше відомостей» → «Виконати однаково» (інсталятор не підписаний).
2. Відкрийте OBS: у меню **Доки** з'явиться «єБот». Натисніть «Підключити канал» і підтвердьте
   код у кабінеті єБота.

Вручну: розпакуйте `ebot-obs-windows.zip` у `%ProgramData%\obs-studio\plugins\`
(вийде `…\plugins\ebot-obs\bin\64bit\ebot-obs.dll`).

**Вимоги:** Windows 10/11 x64, OBS Studio **31** або новіший (плагін зібраний проти SDK 31.1).
macOS і Linux — через скрипт-релей з кабінету.

## Як це працює

Плагін — тонка труба: з'єднується з `wss://api.ebot.live/api/ws/obs-relay` (WinHTTP) і виконує
запити єБота через внутрішній API вбудованого obs-websocket (`src/obs-websocket-api.h`,
вендорено з obs-websocket). Дозволені запити — жорсткий список у `src/core.hpp`
(`OBS_CALL_ALLOWED_REQUESTS`), той самий, що в скриптах-релеях; паритет стереже тест
основного репо. Токен каналу зберігається під DPAPI у конфігу плагіна.

## Розробка

- Збірка — лише в CI (`.github/workflows/windows.yml`, шаблон `obs-plugintemplate`).
- Чиста частина тестується без OBS: `g++ -std=c++17 -Isrc tests/core_test.cpp src/core.cpp -o core_test && ./core_test`.
- Локальний сервер: задайте `EBOT_SERVER=http://localhost:8000` перед запуском OBS.
- Реліз: версія в `buildspec.json`, потім тег `vX.Y.Z` — CI викладе інсталятор, zip і `latest.json`.

## Ліцензія

GPL-2.0 (див. `LICENSE`) — як і OBS Studio, з яким лінкується плагін.
