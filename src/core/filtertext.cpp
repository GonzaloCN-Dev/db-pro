#include "filtertext.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <regex>

namespace dbp {

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::vector<std::string> split(const std::string &s, const std::string &sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(trim(s.substr(start)));
            break;
        }
        out.push_back(trim(s.substr(start, p - start)));
        start = p + sep.size();
    }
    return out;
}

// dd/mm/yyyy[ time] -> yyyy-mm-dd[ time]; anything else is passed through.
std::string normalize_date(const std::string &in)
{
    static const std::regex re(R"(^(\d{1,2})[/.](\d{1,2})[/.](\d{4})(.*)$)");
    std::smatch m;
    if (std::regex_match(in, m, re)) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%s-%02d-%02d", m[3].str().c_str(), std::stoi(m[2]), std::stoi(m[1]));
        return std::string(buf) + m[4].str();
    }
    return in;
}

} // namespace

const char *op_symbol(Op op)
{
    switch (op) {
    case Op::Contains: return "~";
    case Op::NotContains: return "!~";
    case Op::Equals: return "=";
    case Op::NotEquals: return "<>";
    case Op::StartsWith: return "a*";
    case Op::EndsWith: return "*a";
    case Op::IsEmpty: return "''";
    case Op::NotEmpty: return "!''";
    case Op::Regex: return "re";
    case Op::Gt: return ">";
    case Op::Lt: return "<";
    case Op::Gte: return ">=";
    case Op::Lte: return "<=";
    case Op::Between: return "..";
    case Op::In: return "in";
    case Op::NotIn: return "!in";
    case Op::IsNull: return "nul";
    case Op::NotNull: return "!nul";
    case Op::IsTrue: return "T";
    case Op::IsFalse: return "F";
    case Op::Exists: return "ex";
    case Op::NotExists: return "!ex";
    }
    return "?";
}

std::vector<Op> ops_for(const ColumnInfo &c, const Capabilities &caps)
{
    std::vector<Op> v;
    if (is_numeric(c.cls)) {
        v = { Op::Equals, Op::NotEquals, Op::Gt, Op::Lt, Op::Gte, Op::Lte, Op::Between, Op::In, Op::NotIn, Op::Contains };
    } else if (is_temporal(c.cls)) {
        v = { Op::Equals, Op::NotEquals, Op::Gt, Op::Lt, Op::Gte, Op::Lte, Op::Between };
    } else if (c.cls == TypeClass::Bool) {
        v = { Op::IsTrue, Op::IsFalse };
    } else if (c.cls == TypeClass::Binary) {
        v = {};
    } else {
        v = { Op::Contains, Op::NotContains, Op::Equals, Op::NotEquals, Op::StartsWith, Op::EndsWith, Op::IsEmpty, Op::NotEmpty };
        if (c.cls == TypeClass::Text || c.cls == TypeClass::LongText) v.push_back(Op::In);
        if (caps.regex) v.push_back(Op::Regex);
    }
    v.push_back(Op::IsNull);
    v.push_back(Op::NotNull);
    return v;
}

std::optional<Value> parse_typed(TypeClass cls, const std::string &raw, std::string *err)
{
    std::string t = trim(raw);
    auto fail = [&](const char *m) -> std::optional<Value> {
        if (err) *err = m;
        return std::nullopt;
    };
    switch (cls) {
    case TypeClass::Integer: {
        if (t.empty()) return fail("Enter an integer");
        errno = 0;
        char *end = nullptr;
        long long n = std::strtoll(t.c_str(), &end, 10);
        if (errno || *end) return fail("Not a valid integer");
        return Value::integer(n);
    }
    case TypeClass::Decimal: {
        static const std::regex re(R"(^[+-]?\d+([.,]\d+)?$)");
        if (!std::regex_match(t, re)) return fail("Not a valid number");
        std::replace(t.begin(), t.end(), ',', '.');
        return Value::of(Value::Kind::Decimal, t);
    }
    case TypeClass::Float: {
        std::replace(t.begin(), t.end(), ',', '.');
        char *end = nullptr;
        errno = 0;
        double d = std::strtod(t.c_str(), &end);
        if (t.empty() || errno || *end) return fail("Not a valid number");
        return Value::real(d);
    }
    case TypeClass::Bool: {
        std::string l = lower(t);
        if (l == "true" || l == "1" || l == "yes" || l == "si" || l == "sí" || l == "verdadero") return Value::boolean(true);
        if (l == "false" || l == "0" || l == "no" || l == "falso") return Value::boolean(false);
        return fail("Use true or false");
    }
    case TypeClass::Date:
    case TypeClass::DateTime:
    case TypeClass::Time: {
        std::string n = cls == TypeClass::Time ? t : normalize_date(t);
        if (n.empty() || !std::isdigit((unsigned char)n[0])) return fail("Not a valid date");
        return Value::of(Value::Kind::Temporal, n);
    }
    case TypeClass::Uuid: return Value::of(Value::Kind::Uuid, t);
    case TypeClass::Json: return Value::of(Value::Kind::Json, raw);
    case TypeClass::ObjectId: return Value::of(Value::Kind::ObjectId, t);
    default: return Value::text(raw);
    }
}

std::optional<Condition> make_condition(const ColumnInfo &c, Op op, const std::string &text, std::string *err)
{
    if (err) err->clear();
    Condition cond;
    cond.column = c.name;
    cond.op = op;
    int arity = op_arity(op);
    std::string t = op == Op::Contains || op == Op::NotContains || op == Op::StartsWith || op == Op::EndsWith || op == Op::Equals || op == Op::NotEquals || op == Op::Regex ? text : trim(text);
    if (arity == 0) return cond;
    if (trim(text).empty() && !(is_textual_class(c.cls) && (op == Op::Equals || op == Op::NotEquals) && !text.empty())) return std::nullopt;

    auto parse = [&](const std::string &s) { return parse_typed(op == Op::Contains || op == Op::NotContains || op == Op::StartsWith || op == Op::EndsWith || op == Op::Regex ? TypeClass::Text : c.cls, s, err); };

    if (op == Op::Between) {
        std::vector<std::string> parts = t.find("..") != std::string::npos ? split(t, "..") : split(t, ";");
        if (parts.size() != 2 || parts[0].empty() || parts[1].empty()) {
            if (err) *err = "Use two values: from..to";
            return std::nullopt;
        }
        for (auto &p : parts) {
            auto v = parse(p);
            if (!v) return std::nullopt;
            cond.values.push_back(*v);
        }
        return cond;
    }
    if (op == Op::In || op == Op::NotIn) {
        std::vector<std::string> parts = t.find(';') != std::string::npos ? split(t, ";") : (is_numeric(c.cls) ? split(t, ",") : std::vector<std::string> { t });
        for (auto &p : parts) {
            if (p.empty()) continue;
            auto v = parse(p);
            if (!v) return std::nullopt;
            cond.values.push_back(*v);
        }
        if (cond.values.empty()) return std::nullopt;
        return cond;
    }
    auto v = parse(t);
    if (!v) return std::nullopt;
    cond.values.push_back(*v);
    return cond;
}

bool matches_local(const Condition &c, const Value &v)
{
    auto text = [&](const Value &x) { return c.case_sensitive ? x.display() : lower(x.display()); };
    auto arg = [&](size_t i) { return c.case_sensitive ? c.values[i].display() : lower(c.values[i].display()); };
    auto num = [](const Value &x, double &out) {
        if (x.kind == Value::Kind::Int || x.kind == Value::Kind::Bool) { out = (double)x.i; return true; }
        if (x.kind == Value::Kind::Float) { out = x.d; return true; }
        if (x.kind == Value::Kind::Decimal || x.kind == Value::Kind::Text) {
            char *end = nullptr;
            out = std::strtod(x.s.c_str(), &end);
            return end && *end == 0 && !x.s.empty();
        }
        return false;
    };
    auto cmp = [&](const Value &a, const Value &b, int &r) {
        double x, y;
        if (num(a, x) && num(b, y)) { r = x < y ? -1 : x > y ? 1 : 0; return true; }
        std::string s1 = a.display(), s2 = b.display();
        r = s1 < s2 ? -1 : s1 > s2 ? 1 : 0;
        return true;
    };
    bool null = v.is_null() || v.kind == Value::Kind::Missing;
    switch (c.op) {
    case Op::IsNull: return null;
    case Op::NotNull: return !null;
    case Op::IsEmpty: return null || v.display().empty();
    case Op::NotEmpty: return !null && !v.display().empty();
    case Op::IsTrue: return v.kind == Value::Kind::Bool ? v.i != 0 : (!null && (v.display() == "1" || lower(v.display()) == "true"));
    case Op::IsFalse: return v.kind == Value::Kind::Bool ? v.i == 0 : (!null && (v.display() == "0" || lower(v.display()) == "false"));
    case Op::Contains: return !null && text(v).find(arg(0)) != std::string::npos;
    case Op::NotContains: return null || text(v).find(arg(0)) == std::string::npos;
    case Op::StartsWith: return !null && text(v).rfind(arg(0), 0) == 0;
    case Op::EndsWith: { if (null) return false; std::string a = text(v), b = arg(0); return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0; }
    case Op::Equals: { if (null) return false; int r; cmp(v, c.values[0], r); return r == 0; }
    case Op::NotEquals: { if (null) return true; int r; cmp(v, c.values[0], r); return r != 0; }
    case Op::Gt: { if (null) return false; int r; cmp(v, c.values[0], r); return r > 0; }
    case Op::Lt: { if (null) return false; int r; cmp(v, c.values[0], r); return r < 0; }
    case Op::Gte: { if (null) return false; int r; cmp(v, c.values[0], r); return r >= 0; }
    case Op::Lte: { if (null) return false; int r; cmp(v, c.values[0], r); return r <= 0; }
    case Op::Between: { if (null) return false; int a, b; cmp(v, c.values[0], a); cmp(v, c.values[1], b); return a >= 0 && b <= 0; }
    case Op::In:
    case Op::NotIn: {
        bool found = false;
        for (auto &x : c.values) { if (null) { found = found || x.is_null(); continue; } int r; cmp(v, x, r); if (r == 0) { found = true; break; } }
        return c.op == Op::In ? found : !found;
    }
    case Op::Regex: {
        if (null) return false;
        try {
            std::regex re(c.values[0].display(), c.case_sensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
            return std::regex_search(v.display(), re);
        } catch (...) { return false; }
    }
    default: return true;
    }
}

} // namespace dbp
