#include "column_layouts.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#include "platform.h"

namespace dbp {

using nlohmann::json;
namespace fs = std::filesystem;

ColumnView resolve_layout(const ColumnLayout &layout, const std::vector<std::string> &columns)
{
    ColumnView v;
    std::vector<bool> used(columns.size(), false);
    for (const auto &name : layout.order) {
        for (size_t k = 0; k < columns.size(); ++k)
            if (!used[k] && columns[k] == name) {
                v.order.push_back(k);
                used[k] = true;
                break;
            }
    }
    for (size_t k = 0; k < columns.size(); ++k)
        if (!used[k]) v.order.push_back(k);
    v.hidden.assign(columns.size(), false);
    std::set<std::string> hid(layout.hidden.begin(), layout.hidden.end());
    size_t visible = columns.size();
    for (size_t k = 0; k < columns.size(); ++k)
        if (hid.count(columns[k])) {
            v.hidden[k] = true;
            --visible;
        }
    if (visible == 0 && !columns.empty()) v.hidden[v.order[0]] = false;
    return v;
}

ColumnLayout layout_from_view(const ColumnView &view, const std::vector<std::string> &columns)
{
    ColumnLayout l;
    bool natural = true;
    for (size_t p = 0; p < view.order.size(); ++p) {
        if (view.order[p] != p) natural = false;
        l.order.push_back(columns[view.order[p]]);
    }
    if (natural) l.order.clear();
    for (size_t k = 0; k < view.hidden.size() && k < columns.size(); ++k)
        if (view.hidden[k]) l.hidden.push_back(columns[k]);
    return l;
}

std::string ColumnLayouts::make_key(const std::string &profile_id, const std::string &database, const std::string &schema, const std::string &table)
{
    return profile_id + "|" + database + "|" + schema + "|" + table;
}

ColumnLayouts::ColumnLayouts(std::string dir) : dir_(dir.empty() ? platform::data_dir() : std::move(dir))
{
    std::error_code ec;
    fs::create_directories(platform::widen(dir_), ec);
}

ColumnLayout ColumnLayouts::get(const std::string &key) const
{
    auto it = items_.find(key);
    return it == items_.end() ? ColumnLayout() : it->second;
}

void ColumnLayouts::set(const std::string &key, const ColumnLayout &layout)
{
    if (layout.order.empty() && layout.hidden.empty()) items_.erase(key);
    else items_[key] = layout;
}

void ColumnLayouts::load()
{
    items_.clear();
    try {
        fs::path p = fs::path(platform::widen(dir_)) / L"column_layouts.json";
        if (!fs::exists(p)) return;
        std::ifstream f(p, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        json j = json::parse(body);
        json tables = j.value("tables", json::object());
        for (auto &[key, v] : tables.items()) {
            ColumnLayout l;
            for (auto &n : v.value("order", json::array())) if (n.is_string()) l.order.push_back(n.get<std::string>());
            for (auto &n : v.value("hidden", json::array())) if (n.is_string()) l.hidden.push_back(n.get<std::string>());
            if (!l.order.empty() || !l.hidden.empty()) items_[key] = std::move(l);
        }
    } catch (...) {
        items_.clear(); // a corrupt file must never block opening tables
    }
}

bool ColumnLayouts::save() const
{
    json tables = json::object();
    for (auto &[key, l] : items_) tables[key] = { { "order", l.order }, { "hidden", l.hidden } };
    fs::path dir(platform::widen(dir_));
    fs::path p = dir / L"column_layouts.json";
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
