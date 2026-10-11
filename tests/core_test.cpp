// Чиста частина плагіна — без OBS і без Windows. Ганяється в CI на ubuntu.
#include "core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace ebot;

int main()
{
	assert(OBS_CALL_ALLOWED_REQUESTS.size() == 16);
	assert(is_allowed_request("GetStreamStatus"));
	assert(!is_allowed_request("TriggerHotkeyByName"));

	assert(clamp_screenshot_size(0, 0) == std::make_pair(1280, 720));
	assert(clamp_screenshot_size(3840, 2160) == std::make_pair(1280, 720));
	assert(clamp_screenshot_size(-5, 100) == std::make_pair(1280, 720));
	assert(clamp_screenshot_size(4, 4) == std::make_pair(8, 8));
	assert(clamp_screenshot_size(640, 360) == std::make_pair(640, 360));

	std::tm when{};
	when.tm_year = 126;
	when.tm_mon = 9;
	when.tm_mday = 10;
	when.tm_hour = 7;
	when.tm_min = 5;
	assert(backup_filename("auto", when) == "ebot_backup_20261010_0705_auto.zip");
	assert(backup_filename("../evil", when) == "ebot_backup_20261010_0705_evil.zip");
	assert(backup_filename("", when) == "ebot_backup_20261010_0705_manual.zip");

	assert(is_newer_version("0.1.10", "0.1.9"));
	assert(!is_newer_version("0.1.0", "0.1.0"));
	assert(!is_newer_version("0.0.9", "0.1.0"));
	assert(!is_newer_version("garbage", "0.1.0"));

	assert(is_safe_token("abc-DEF_123"));
	assert(!is_safe_token(""));
	assert(!is_safe_token("abc&x=1"));

	assert(json_quote("Mic") == "\"Mic\"");
	assert(json_quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"");
	assert(json_quote("\xD0\x84") == "\"\xD0\x84\""); // UTF-8 («Є») лишається як є

	const float silent[2] = {-std::numeric_limits<float>::infinity(), NAN};
	assert(peak_to_db(silent, 2) == -120.0);
	const float loud[2] = {-30.0f, -6.0f};
	assert(peak_to_db(loud, 2) == -6.0);

	MicPeaks peaks;
	assert(peaks.read("Mic", 100.0).samples == 0);
	peaks.add("Mic", 100.0, -40.0);
	peaks.add("Mic", 110.0, -12.0);
	MicLevel level = peaks.read("Mic", 115.0);
	assert(level.samples == 2 && level.peak_db == -12.0);
	assert(peaks.read("Mic", 135.0).samples == 1); // перший вийшов за 30 с
	assert(peaks.read("Other", 115.0).samples == 0);

	// 4001 — сервер не прийняв токен (перевипущено в кабінеті): не довбати сервер.
	assert(classify_close(4001) == CloseKind::Rejected);
	assert(classify_close(1006) == CloseKind::Retry);
	assert(classify_close(1000) == CloseKind::Retry);

	// Поганий токен uvicorn відхиляє ще на рукостисканні: HTTP 403, а не WS 4001.
	assert(classify_handshake(403) == CloseKind::Rejected);
	assert(classify_handshake(401) == CloseKind::Rejected);
	assert(classify_handshake(502) == CloseKind::Retry); // деплой Railway
	assert(classify_handshake(0) == CloseKind::Retry);   // мережа

	// 1013 «Try Again Later»: сервер не зміг перевірити токен (база зайнята) —
	// це не відмова, а звичайне перепідключення.
	assert(classify_close(1013) == CloseKind::Retry);

	// GetProfileParameter — лише пари, які читає сервер (VOD-доріжки), а не
	// будь-який ключ профілю (там лежать Twitch-токени входу OBS).
	assert(is_allowed_profile_param("AdvOut", "VodTrackIndex"));
	assert(is_allowed_profile_param("Output", "Mode"));
	assert(!is_allowed_profile_param("Twitch", "Token"));
	assert(!is_allowed_profile_param("AdvOut", "RecFilePath"));

	// Перша спроба після обриву — за пів секунди, далі вдвічі довше до стелі,
	// розкид не виводить за стелю.
	assert(reconnect_delay_ms(0, 0.0) == RECONNECT_FIRST_MS);
	assert(reconnect_delay_ms(0, 1.0) <= 2 * RECONNECT_FIRST_MS);
	assert(reconnect_delay_ms(1, 0.0) == 2 * RECONNECT_FIRST_MS);
	assert(reconnect_delay_ms(30, 1.0) == RECONNECT_MAX_MS);
	assert(reconnect_delay_ms(-3, 0.0) == RECONNECT_FIRST_MS);

	// «Токен не прийнято» теж пробуємо знову: 30 с, 60 с, … до 5 хв.
	assert(rejected_retry_delay_ms(0, 0.0) == REJECTED_RETRY_FIRST_MS);
	assert(rejected_retry_delay_ms(1, 0.0) == 2 * REJECTED_RETRY_FIRST_MS);
	assert(rejected_retry_delay_ms(20, 1.0) == REJECTED_RETRY_MAX_MS);

	// Текст помилки — формат obsws-python, який сервер уже розбирає.
	assert(obs_error_text("SetCurrentProgramScene", 600, "No source was found by the name of `X`.") ==
	       "Request SetCurrentProgramScene returned code 600. With message: No source was found by the name of `X`.");
	assert(obs_error_text("GetStats", 207, "") == "Request GetStats returned code 207");

	// Важкі запити — у свою чергу; решта (і всі зміни стану OBS) — по порядку.
	assert(is_heavy_request("get_screenshot", ""));
	assert(is_heavy_request("obs_call", "GetStats"));
	assert(!is_heavy_request("obs_call", "StartStream"));
	assert(!is_heavy_request("set_scene", ""));

	assert(is_safe_verify_url("https://ebot.live/widgets?tab=obs&pair=ABCD-2345"));
	assert(is_safe_verify_url("http://localhost:3000/widgets?pair=X"));
	assert(!is_safe_verify_url("http://localhost@evil.com/widgets"));
	assert(!is_safe_verify_url("https://ebot.live@evil.com/"));
	assert(!is_safe_verify_url("file:///C:/Windows/system32/calc.exe"));
	assert(!is_safe_verify_url("http://ebot.live/"));

	std::puts("core_test: ok");
	return 0;
}
