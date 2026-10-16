// Токен на ПК — під DPAPI (прив'язаний до користувача Windows), а не
// відкритим текстом, як .env скрипта.
#include "config.hpp"
#include "core.hpp"
#include <obs-module.h>
#include <obs.hpp>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <windows.h>
#include <wincrypt.h>
#include <vector>

namespace ebot {

static std::string module_path(const char *file)
{
	char *path = obs_module_config_path(file);
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

static std::string b64(const std::vector<BYTE> &bytes)
{
	DWORD n = 0;
	const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
	CryptBinaryToStringA(bytes.data(), (DWORD)bytes.size(), flags, nullptr, &n);
	std::string out(n, '\0');
	CryptBinaryToStringA(bytes.data(), (DWORD)bytes.size(), flags, out.data(), &n);
	out.resize(n);
	return out;
}

static std::vector<BYTE> unb64(const std::string &text)
{
	DWORD n = 0;
	if (text.empty() || !CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr))
		return {};
	std::vector<BYTE> out(n);
	CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, out.data(), &n, nullptr, nullptr);
	return out;
}

static std::string protect(const std::string &plain)
{
	if (plain.empty())
		return "";
	DATA_BLOB in{(DWORD)plain.size(), (BYTE *)plain.data()};
	DATA_BLOB out{};
	if (!CryptProtectData(&in, L"ebot", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		// Мовчки збережений порожній токен — це «від'єднано» після рестарту OBS
		// без жодного сліду чому.
		obs_log(LOG_ERROR, "could not protect the relay token with DPAPI (error %lu): it is not saved",
			GetLastError());
		return "";
	}
	std::vector<BYTE> bytes(out.pbData, out.pbData + out.cbData);
	LocalFree(out.pbData);
	return b64(bytes);
}

static std::string unprotect(const std::string &encoded)
{
	std::vector<BYTE> bytes = unb64(encoded);
	DATA_BLOB in{(DWORD)bytes.size(), bytes.data()};
	DATA_BLOB out{};
	if (bytes.empty() ||
	    !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return "";
	std::string plain((char *)out.pbData, out.cbData);
	LocalFree(out.pbData);
	return plain;
}

Saved load_config()
{
	Saved saved;
	OBSDataAutoRelease data = obs_data_create_from_json_file(module_path("config.json").c_str());
	if (!data)
		return saved;
	saved.token = unprotect(obs_data_get_string(data, "token_dpapi"));
	saved.channel = obs_data_get_string(data, "channel");
	if (!is_safe_token(saved.token))
		saved.token.clear();
	return saved;
}

void save_config(const Saved &saved)
{
	os_mkdirs(module_path("").c_str());
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "token_dpapi", protect(saved.token).c_str());
	obs_data_set_string(data, "channel", saved.channel.c_str());
	obs_data_save_json_safe(data, module_path("config.json").c_str(), "tmp", "bak");
}

void clear_config()
{
	save_config(Saved{});
}

} // namespace ebot
