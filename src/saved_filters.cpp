#include "saved_filters.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "platform.h"

namespace dbp {

using nlohmann::json;
namespace fs = std::filesystem;

std::string SavedFilters::make_key(const std::string &profile_id, const std::string &database, const std::string &schema, const std::string &table)
{
    return profile_id + "|" + database + "|" + schema + "|" + table;
}

SavedFilters::SavedFilters(std::string dir) : dir_(dir.empty() ? platform::data_dir() : std::move(dir))
{
    std::error_code ec;
    fs::create_directories(platform::widen(dir_), ec);
}

const std::vector<SavedFilter> &SavedFilters::list(const std::string &key) const
{
    static const std::vector<SavedFilter> none;
    auto it = items_.find(key);
    return it == items_.end() ? none : it->second;
}

bool SavedFilters::put(const std::string &key, SavedFilter f)
{
    auto b = f.name.find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    auto e = f.name.find_last_not_of(" \t");
    f.name = f.name.substr(b, e - b + 1);
    if (f.name.size() > kMaxSavedFilterName) f.name.resize(kMaxSavedFilterName);
    auto &v = items_[key];
    for (auto &x : v)
        if (x.name == f.name) { x = std::move(f); return true; }
    if (v.size() >= kMaxSavedFilters) return false;
    v.push_back(std::move(f));
    return true;
}

void SavedFilters::remove(const std::string &key, size_t index)
{
    auto it = items_.find(key);
    if (it == items_.end() || index >= it->second.size()) return;
    it->second.erase(it->second.begin() + (long)index);
    if (it->second.empty()) items_.erase(it);
}

void SavedFilters::load()
{
    items_.clear();
    try {
        fs::path p = fs::path(platform::widen(dir_)) / L"saved_filters.json";
        if (!fs::exists(p)) return;
        std::ifstream f(p, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        json j = json::parse(body);
        json tables = j.value("tables", json::object());
        for (auto &[key, arr] : tables.items()) {
            if (!arr.is_array()) continue;
            for (auto &o : arr) {
                SavedFilter sf;
                sf.name = o.value("name", std::string());
                for (auto &i : o.value("items", json::array())) {
                    SavedFilterItem it;
                    it.column = i.value("column", std::string());
                    it.op = i.value("op", std::string());
                    it.text = i.value("text", std::string());
                    if (!it.column.empty()) sf.items.push_back(std::move(it));
                }
                if (!sf.name.empty() && !sf.items.empty()) put(key, std::move(sf));
            }
        }
    } catch (...) {
        items_.clear();
    }
}

bool SavedFilters::save() const
{
    json tables = json::object();
    for (auto &[key, v] : items_) {
        json arr = json::array();
        for (auto &f : v) {
            json items = json::array();
            for (auto &i : f.items) items.push_back({ { "column", i.column }, { "op", i.op }, { "text", i.text } });
            arr.push_back({ { "name", f.name }, { "items", items } });
        }
        tables[key] = arr;
    }
    fs::path dir(platform::widen(dir_));
    fs::path p = dir / L"saved_filters.json";
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << json({ { "version", 1 }, { "tables", tables } }).dump(2);
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
