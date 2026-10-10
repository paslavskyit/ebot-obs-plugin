#pragma once
#include <string>

namespace ebot {
struct Saved {
	std::string token;
	std::string channel;
};
Saved load_config();
void save_config(const Saved &saved);
void clear_config();
} // namespace ebot
