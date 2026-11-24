// All user-visible text lives in i18n.cpp (Spanish and English). Missing keys fall back to English, then to the key.
#pragma once

#include <initializer_list>
#include <string>

namespace dbp::i18n {

void set_language(const std::string &lang); // "es" or "en"
const std::string &language();
std::string tr(const std::string &key);
// Replaces {0}, {1}, ... with the given arguments.
std::string trf(const std::string &key, std::initializer_list<std::string> args);
std::string with_commas(unsigned long long n); // 1234567 -> "1,234,567" (en) / "1.234.567" (es)

} // namespace dbp::i18n
