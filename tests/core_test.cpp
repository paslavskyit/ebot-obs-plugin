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

	std::puts("core_test: ok");
	return 0;
}
