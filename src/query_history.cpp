#include "query_history.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "platform.h"

namespace dbp {

using nlohmann::json;
namespace fs = std::filesystem;

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string trimmed(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

QueryHistory::QueryHistory(std::string dir) : dir_(dir.empty() ? platform::data_dir() : std::move(dir))
{
    std::error_code ec;
    fs::create_directories(platform::widen(dir_), ec);
}

bool QueryHistory::looks_secret(const std::string &text)
{
    static const char *words[] = { "password", "passwd", "identified by", "secret", "api_key", "apikey", "token", "pwd=", "pwd =" };
    std::string l = lower(text);
    for (const char *w : words)
        if (l.find(w) != std::string::npos) return true;
    return false;
}

std::string QueryHistory::preview(const std::string &text, size_t max_chars)
{
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += (char)c;
        if (out.size() >= max_chars) break;
    }
    if (out.size() >= max_chars) {
        // do not cut a UTF-8 sequence in half
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && (unsigned char)out.back() >= 0xC0) out.pop_back();
        out += "...";
    }
    return out;
}

bool QueryHistory::record(const std::string &text, long long now)
{
    std::string t = trimmed(text);
    if (t.empty() || t.size() > kMaxChars || looks_secret(t)) return false;
    for (size_t k = 0; k < entries_.size(); ++k) {
        if (entries_[k].text == t) {
            HistoryEntry e = entries_[k];
            e.last_used = now;
            e.runs += 1;
            entries_.erase(entries_.begin() + k);
            entries_.insert(entries_.begin(), std::move(e));
            return true;
        }
    }
    HistoryEntry e;
    e.text = std::move(t);
    e.last_used = now;
    entries_.insert(entries_.begin(), std::move(e));
    trim();
    return true;
}

void QueryHistory::trim()
{
    size_t plain = 0, fav = 0;
    std::vector<HistoryEntry> kept;
    for (auto &e : entries_) {
        size_t &n = e.favorite ? fav : plain;
        size_t cap = e.favorite ? kMaxFavorites : kMaxPlain;
        if (n >= cap) continue;
        ++n;
        kept.push_back(std::move(e));
    }
    entries_ = std::move(kept);
}

bool QueryHistory::set_favorite(size_t index, bool on)
{
    if (index >= entries_.size()) return false;
    entries_[index].favorite = on;
    trim();
    return true;
}

bool QueryHistory::remove(size_t index)
{
    if (index >= entries_.size()) return false;
    entries_.erase(entries_.begin() + index);
    return true;
}

void QueryHistory::clear_plain()
{
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const HistoryEntry &e) { return !e.favorite; }), entries_.end());
}

void QueryHistory::clear_all() { entries_.clear(); }

std::vector<size_t> QueryHistory::view(const std::string &needle, bool favorites_only) const
{
    std::string n = lower(trimmed(needle));
    std::vector<size_t> out;
    for (size_t k = 0; k < entries_.size(); ++k) {
        if (favorites_only && !entries_[k].favorite) continue;
        if (!n.empty() && lower(entries_[k].text).find(n) == std::string::npos) continue;
        out.push_back(k);
    }
    return out;
}

void QueryHistory::load()
{
    entries_.clear();
    try {
        fs::path p = fs::path(platform::widen(dir_)) / L"query_history.json";
        if (!fs::exists(p)) return;
        std::ifstream f(p, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        json j = json::parse(body);
        for (auto &e : j.value("entries", json::array())) {
            HistoryEntry h;
            h.text = e.value("text", "");
            h.favorite = e.value("favorite", false);
            h.last_used = e.value("last_used", 0LL);
            h.runs = std::max(1, e.value("runs", 1));
            if (h.text.empty() || h.text.size() > kMaxChars || looks_secret(h.text)) continue;
            entries_.push_back(std::move(h));
        }
        std::stable_sort(entries_.begin(), entries_.end(), [](const HistoryEntry &a, const HistoryEntry &b) { return a.last_used > b.last_used; });
        trim();
    } catch (...) {
        entries_.clear(); // a corrupt file must never block the editor
    }
}

bool QueryHistory::save() const
{
    json arr = json::array();
    for (auto &e : entries_) arr.push_back({ { "text", e.text }, { "favorite", e.favorite }, { "last_used", e.last_used }, { "runs", e.runs } });
    fs::path dir(platform::widen(dir_));
    fs::path p = dir / L"query_history.json";
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << json({ { "version", 1 }, { "entries", arr } }).dump(2);
        if (!f.good()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

} // namespace dbp
