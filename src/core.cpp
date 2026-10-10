#include "core.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

namespace ebot {

bool is_allowed_request(const std::string &request)
{
	return OBS_CALL_ALLOWED_REQUESTS.count(request) > 0;
}

bool is_allowed_profile_param(const std::string &category, const std::string &name)
{
	return OBS_PROFILE_PARAMS_ALLOWED.count({category, name}) > 0;
}

bool is_heavy_request(const std::string &method, const std::string &obs_request)
{
	return method == "get_screenshot" || (method == "obs_call" && obs_request == "GetStats");
}

bool is_safe_verify_url(const std::string &url)
{
	// «http://localhost» без двокрапки пропускав би http://localhost@evil.com/…
	for (const char *prefix : {"https://", "http://localhost:", "http://127.0.0.1:"})
		if (url.rfind(prefix, 0) == 0)
			return url.find('@') == std::string::npos;
	return false;
}

// base·2^attempt, розкид до +100 % від бази, і все — не вище стелі.
static int backoff_ms(int attempt, double jitter, int first, int cap)
{
	attempt = std::clamp(attempt, 0, 20);
	double base = std::min(double(cap), double(first) * std::pow(2.0, attempt));
	double with_jitter = base * (1.0 + std::clamp(jitter, 0.0, 1.0));
	return int(std::min(double(cap), with_jitter));
}

int reconnect_delay_ms(int attempt, double jitter)
{
	return backoff_ms(attempt, jitter, RECONNECT_FIRST_MS, RECONNECT_MAX_MS);
}

int rejected_retry_delay_ms(int attempt, double jitter)
{
	return backoff_ms(attempt, jitter, REJECTED_RETRY_FIRST_MS, REJECTED_RETRY_MAX_MS);
}

std::string obs_error_text(const std::string &request, unsigned int code, const std::string &comment)
{
	std::string text = "Request " + request + " returned code " + std::to_string(code);
	return comment.empty() ? text : text + ". With message: " + comment;
}

std::pair<int, int> clamp_screenshot_size(long long width, long long height)
{
	long long w = width, h = height;
	if (w <= 0 || h <= 0) {
		w = SCREENSHOT_DEFAULT_WIDTH;
		h = SCREENSHOT_DEFAULT_HEIGHT;
	}
	long long longest = std::max(w, h);
	if (longest > SCREENSHOT_MAX_EDGE) {
		double scale = double(SCREENSHOT_MAX_EDGE) / double(longest);
		w = (long long)(double(w) * scale);
		h = (long long)(double(h) * scale);
	}
	w = std::clamp<long long>(w, SCREENSHOT_MIN_EDGE, SCREENSHOT_MAX_EDGE);
	h = std::clamp<long long>(h, SCREENSHOT_MIN_EDGE, SCREENSHOT_MAX_EDGE);
	return {int(w), int(h)};
}

std::string backup_filename(const std::string &trigger, const std::tm &when)
{
	std::string safe;
	for (char ch : trigger)
		if (std::isalnum((unsigned char)ch) || ch == '_')
			safe += ch;
	if (safe.empty())
		safe = "manual";
	char stamp[32];
	std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M", &when);
	return std::string("ebot_backup_") + stamp + "_" + safe + ".zip";
}

static bool parse_version(const std::string &text, std::vector<int> &out)
{
	std::stringstream ss(text);
	std::string part;
	while (std::getline(ss, part, '.')) {
		if (part.empty() || part.size() > 6 ||
		    !std::all_of(part.begin(), part.end(), [](char ch) { return std::isdigit((unsigned char)ch); }))
			return false;
		out.push_back(std::stoi(part));
	}
	return !out.empty();
}

bool is_newer_version(const std::string &latest, const std::string &current)
{
	std::vector<int> a, b;
	if (!parse_version(latest, a) || !parse_version(current, b))
		return false;
	return a > b;
}

bool is_safe_token(const std::string &token)
{
	return !token.empty() && std::all_of(token.begin(), token.end(), [](char ch) {
		return std::isalnum((unsigned char)ch) || ch == '-' || ch == '_';
	});
}

std::string json_quote(const std::string &text)
{
	std::string out = "\"";
	for (unsigned char ch : text) {
		if (ch == '"' || ch == '\\') {
			out += '\\';
			out += (char)ch;
		} else if (ch == '\n') {
			out += "\\n";
		} else if (ch < 0x20) {
			char buf[8];
			std::snprintf(buf, sizeof buf, "\\u%04x", ch);
			out += buf;
		} else {
			out += (char)ch;
		}
	}
	return out + "\"";
}

double peak_to_db(const float *peaks, int channels)
{
	double best = MIC_PEAK_FLOOR_DB;
	for (int i = 0; i < channels; ++i)
		if (std::isfinite(peaks[i]) && peaks[i] > best)
			best = peaks[i];
	return best;
}

CloseKind classify_close(unsigned short code)
{
	return code == 4001 ? CloseKind::Rejected : CloseKind::Retry;
}

CloseKind classify_handshake(unsigned long http_status)
{
	return (http_status == 401 || http_status == 403) ? CloseKind::Rejected : CloseKind::Retry;
}

void MicPeaks::add(const std::string &input, double now, double peak_db)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto &buf = buffers_[input];
	buf.emplace_back(now, peak_db);
	while (!buf.empty() && buf.front().first < now - MIC_PEAK_WINDOW_SEC)
		buf.pop_front();
}

MicLevel MicPeaks::read(const std::string &input, double now) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	MicLevel level;
	auto it = buffers_.find(input);
	if (it == buffers_.end())
		return level;
	for (const auto &[ts, peak] : it->second) {
		if (ts < now - MIC_PEAK_WINDOW_SEC)
			continue;
		level.peak_db = level.samples ? std::max(level.peak_db, peak) : peak;
		++level.samples;
	}
	return level;
}

} // namespace ebot
