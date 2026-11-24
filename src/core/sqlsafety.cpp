#include "sqlsafety.h"

#include <cctype>
#include <vector>

namespace dbp {

namespace {

std::string lower(std::string s)
{
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Drops comments and string literals so keywords inside them are not seen.
std::string strip(const std::string &sql)
{
    std::string out;
    size_t i = 0, n = sql.size();
    while (i < n) {
        char c = sql[i];
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
            i += 2;
            out += ' ';
        } else if (c == '\'' || c == '"' || c == '`') {
            char q = c;
            ++i;
            while (i < n) {
                if (sql[i] == q) {
                    if (i + 1 < n && sql[i + 1] == q) { i += 2; continue; }
                    break;
                }
                ++i;
            }
            ++i;
            out += "''";
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

std::vector<std::string> words(const std::string &s)
{
    std::vector<std::string> w;
    std::string cur;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '_') cur += c;
        else if (!cur.empty()) { w.push_back(lower(cur)); cur.clear(); }
    }
    if (!cur.empty()) w.push_back(lower(cur));
    return w;
}

} // namespace

bool statement_is_read_only(const std::string &sql)
{
    std::string clean = strip(sql);
    size_t start = 0;
    bool any = false;
    while (start <= clean.size()) {
        size_t end = clean.find(';', start);
        std::string piece = clean.substr(start, end == std::string::npos ? std::string::npos : end - start);
        auto w = words(piece);
        if (!w.empty()) {
            any = true;
            const std::string &k = w[0];
            if (k == "select" || k == "show" || k == "describe" || k == "desc" || k == "explain") {
                // EXPLAIN ANALYZE actually executes the statement on some engines.
                if (k == "explain")
                    for (auto &x : w)
                        if (x == "analyze" || x == "analyse") return false;
                // SELECT ... INTO creates a table on SQL Server/PostgreSQL.
                if (k == "select")
                    for (auto &x : w)
                        if (x == "into") return false;
            } else if (k == "with") {
                for (auto &x : w)
                    if (x == "insert" || x == "update" || x == "delete" || x == "merge" || x == "into") return false;
            } else {
                return false;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return any;
}

} // namespace dbp
