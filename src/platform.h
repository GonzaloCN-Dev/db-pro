// Thin Windows layer: credential vault, clipboard, native file dialogs, per-user data dir.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dbp::platform {

std::wstring widen(const std::string &utf8);
std::string narrow(const std::wstring &w);

// %APPDATA%\DbPro (created on demand).
std::string data_dir();

// Windows Credential Manager. The secret never touches disk files of ours.
bool cred_write(const std::string &target, const std::string &user, const std::string &secret);
std::optional<std::string> cred_read(const std::string &target);
void cred_delete(const std::string &target);

bool clipboard_set(const std::string &utf8);

using Filter = std::pair<std::string, std::string>; // label, pattern e.g. {"CSV", "*.csv"}
std::optional<std::string> save_dialog(const std::string &title, const std::string &default_name, const std::vector<Filter> &filters);
std::optional<std::string> open_dialog(const std::string &title, const std::vector<Filter> &filters);

// Opens a folder/file in Explorer.
void reveal_in_explorer(const std::string &path);
void open_folder(const std::string &dir); // opens the folder in Explorer (created when missing)
void open_url(const std::string &url); // opens a trusted, application-owned HTTPS URL

} // namespace dbp::platform
