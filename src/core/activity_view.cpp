#include "activity_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace dbp {
namespace {

std::string lower_ascii(std::string s)
{
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool number(const Value &v, double &out)
{
    if (v.kind == Value::Kind::Int) { out = (double)v.i; return true; }
    if (v.kind == Value::Kind::Float) { out = v.d; return std::isfinite(out); }
    if (v.kind != Value::Kind::Decimal && v.kind != Value::Kind::Text) return false;
    if (v.s.empty()) return false;
    char *end = nullptr;
    out = std::strtod(v.s.c_str(), &end);
    return end == v.s.c_str() + v.s.size() && std::isfinite(out);
}

bool row_blocked(const Activity &a, size_t i)
{
    const auto &r = a.rows[i];
    if (a.blocked_col >= 0 && a.blocked_col < (int)r.size()) {
        const Value &v = r[a.blocked_col];
        if (v.kind != Value::Kind::Null && v.kind != Value::Kind::Missing && !v.display().empty()) return true;
    }
    return false;
}

} // namespace

std::vector<size_t> activity_visible_rows(const Activity &a, const ActivityViewOptions &o)
{
    const std::string needle = lower_ascii(o.search);
    std::vector<size_t> indices;
    for (size_t i = 0; i < a.rows.size(); ++i) {
        const auto &row = a.rows[i];
        if (o.blocked_only && !row_blocked(a, i)) continue;
        if (!needle.empty()) {
            bool found = false;
            for (const Value &v : row) {
                if (lower_ascii(v.display()).find(needle) != std::string::npos) { found = true; break; }
            }
            if (!found) continue;
        }
        indices.push_back(i);
    }
    if (o.sort_column < 0 || o.sort_column >= (int)a.columns.size()) return indices;
    std::stable_sort(indices.begin(), indices.end(), [&](size_t x, size_t y) {
        const auto &rx = a.rows[x];
        const auto &ry = a.rows[y];
        const Value vx = o.sort_column < (int)rx.size() ? rx[o.sort_column] : Value::missing();
        const Value vy = o.sort_column < (int)ry.size() ? ry[o.sort_column] : Value::missing();
        const bool nx = vx.kind == Value::Kind::Null || vx.kind == Value::Kind::Missing;
        const bool ny = vy.kind == Value::Kind::Null || vy.kind == Value::Kind::Missing;
        if (nx != ny) return !nx; // missing values last in either direction
        int cmp = 0;
        double dx = 0, dy = 0;
        if (number(vx, dx) && number(vy, dy)) cmp = dx < dy ? -1 : dx > dy ? 1 : 0;
        else {
            const std::string sx = lower_ascii(vx.display()), sy = lower_ascii(vy.display());
            cmp = sx < sy ? -1 : sx > sy ? 1 : 0;
        }
        if (cmp == 0 && a.id_col >= 0 && a.id_col < (int)rx.size() && a.id_col < (int)ry.size()) {
            const std::string ix = rx[a.id_col].display(), iy = ry[a.id_col].display();
            cmp = ix < iy ? -1 : ix > iy ? 1 : 0;
        }
        return o.ascending ? cmp < 0 : cmp > 0;
    });
    return indices;
}

} // namespace dbp
