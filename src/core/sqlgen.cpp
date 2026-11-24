#include "sqlgen.h"

#include <algorithm>

namespace dbp {

namespace {

[[noreturn]] void invalid(const std::string &m) { throw DbError(ErrKind::Validation, m); }

void check_ident(const std::string &n)
{
    if (n.empty()) invalid("empty identifier");
    if (n.size() > 256) invalid("identifier too long");
    if (n.find('\0') != std::string::npos) invalid("invalid identifier");
}

struct Ctx {
    Dialect d;
    const TableMeta &m;
    std::vector<Value> params;

    std::string ph()
    {
        if (d == Dialect::Postgres) return "$" + std::to_string(params.size());
        return "?";
    }
    std::string bind(Value v)
    {
        params.push_back(std::move(v));
        return ph();
    }
    const ColumnInfo &col(const std::string &name) const
    {
        const ColumnInfo *c = m.column(name);
        if (!c) invalid("unknown column: " + name);
        return *c;
    }
};

bool is_text_class(TypeClass c)
{
    return c == TypeClass::Text || c == TypeClass::LongText || c == TypeClass::Uuid || c == TypeClass::Other;
}

std::string as_text(Ctx &x, const ColumnInfo &c)
{
    std::string q = quote_ident(x.d, c.name);
    if (is_text_class(c.cls) && c.cls != TypeClass::Uuid) return q;
    switch (x.d) {
    case Dialect::MySql: return "CAST(" + q + " AS CHAR)";
    case Dialect::MsSql: return "CAST(" + q + " AS NVARCHAR(MAX))";
    default: return "CAST(" + q + " AS TEXT)";
    }
}

std::string glob_escape(const std::string &s)
{
    std::string r;
    for (char c : s) {
        if (c == '*' || c == '?' || c == '[') { r += '['; r += c; r += ']'; }
        else r += c;
    }
    return r;
}

enum class Pat { Contains, Starts, Ends };

// One LIKE-style text match honouring the dialect and case sensitivity.
std::string like_expr(Ctx &x, const ColumnInfo &c, Pat p, const std::string &text, bool cs, bool negate)
{
    std::string col = as_text(x, c);
    if (x.d == Dialect::Sqlite && cs) {
        std::string g = glob_escape(text);
        std::string pat = (p == Pat::Contains ? "*" + g + "*" : p == Pat::Starts ? g + "*" : "*" + g);
        return col + (negate ? " NOT GLOB " : " GLOB ") + x.bind(Value::text(pat));
    }
    std::string esc = like_escape(text);
    std::string pat = (p == Pat::Contains ? "%" + esc + "%" : p == Pat::Starts ? esc + "%" : "%" + esc);
    std::string kw = negate ? " NOT LIKE " : " LIKE ";
    std::string lhs = col, rhs;
    switch (x.d) {
    case Dialect::Postgres:
        kw = negate ? (cs ? " NOT LIKE " : " NOT ILIKE ") : (cs ? " LIKE " : " ILIKE ");
        rhs = x.bind(Value::text(pat));
        break;
    case Dialect::MySql:
        if (cs) {
            lhs = "CAST(" + col + " AS BINARY)";
            rhs = "CAST(" + x.bind(Value::text(pat)) + " AS BINARY)";
        } else rhs = x.bind(Value::text(pat));
        break;
    case Dialect::MsSql:
        if (cs) lhs = col + " COLLATE Latin1_General_100_CS_AS";
        rhs = x.bind(Value::text(pat));
        break;
    default: rhs = x.bind(Value::text(pat)); break;
    }
    return lhs + kw + rhs + " ESCAPE '!'";
}

std::string cond_sql(Ctx &x, const Condition &c)
{
    if (!c.well_formed()) invalid("malformed condition on " + c.column);
    const ColumnInfo &ci = x.col(c.column);
    std::string q = quote_ident(x.d, ci.name);
    bool txt = is_text_class(ci.cls);
    auto first_text = [&] { return c.values[0].display(); };

    switch (c.op) {
    case Op::IsNull: return q + " IS NULL";
    case Op::NotNull: return q + " IS NOT NULL";
    case Op::IsEmpty:
        return txt ? "(" + q + " IS NULL OR " + q + " = '')" : q + " IS NULL";
    case Op::NotEmpty:
        return txt ? "(" + q + " IS NOT NULL AND " + q + " <> '')" : q + " IS NOT NULL";
    case Op::IsTrue: return x.d == Dialect::Postgres ? q + " IS TRUE" : q + " = 1";
    case Op::IsFalse: return x.d == Dialect::Postgres ? q + " IS FALSE" : q + " = 0";
    case Op::Contains: return like_expr(x, ci, Pat::Contains, first_text(), c.case_sensitive, false);
    case Op::StartsWith: return like_expr(x, ci, Pat::Starts, first_text(), c.case_sensitive, false);
    case Op::EndsWith: return like_expr(x, ci, Pat::Ends, first_text(), c.case_sensitive, false);
    case Op::NotContains:
        return "(" + like_expr(x, ci, Pat::Contains, first_text(), c.case_sensitive, true) + " OR " + q + " IS NULL)";
    case Op::Equals: return q + " = " + x.bind(c.values[0]);
    case Op::NotEquals: {
        std::string p = x.bind(c.values[0]);
        return "(" + q + " <> " + p + " OR " + q + " IS NULL)";
    }
    case Op::Gt: return q + " > " + x.bind(c.values[0]);
    case Op::Lt: return q + " < " + x.bind(c.values[0]);
    case Op::Gte: return q + " >= " + x.bind(c.values[0]);
    case Op::Lte: return q + " <= " + x.bind(c.values[0]);
    case Op::Between: {
        std::string a = x.bind(c.values[0]);
        std::string b = x.bind(c.values[1]);
        return q + " BETWEEN " + a + " AND " + b;
    }
    case Op::In:
    case Op::NotIn: {
        if (c.values.size() > 1000) invalid("too many values in list (max 1000)");
        bool has_null = false;
        std::vector<const Value *> vals;
        for (auto &v : c.values) (v.is_null() ? has_null = true : (vals.push_back(&v), false));
        bool neg = c.op == Op::NotIn;
        if (vals.empty()) return q + (neg ? " IS NOT NULL" : " IS NULL");
        std::string list;
        for (size_t k = 0; k < vals.size(); ++k) list += (k ? "," : "") + x.bind(*vals[k]);
        if (!neg) return has_null ? "(" + q + " IN (" + list + ") OR " + q + " IS NULL)" : q + " IN (" + list + ")";
        return has_null ? "(" + q + " NOT IN (" + list + ") AND " + q + " IS NOT NULL)"
                        : "(" + q + " NOT IN (" + list + ") OR " + q + " IS NULL)";
    }
    case Op::Regex: {
        if (!dialect_has_regex(x.d)) throw DbError(ErrKind::Unsupported, "regular expressions are not supported by this engine");
        std::string col = as_text(x, ci);
        std::string p = x.bind(Value::text(first_text()));
        if (x.d == Dialect::Postgres) return col + (c.case_sensitive ? " ~ " : " ~* ") + p;
        return col + " REGEXP " + p;
    }
    case Op::Exists:
    case Op::NotExists:
        throw DbError(ErrKind::Unsupported, "field existence applies to document databases only");
    }
    invalid("unsupported operator");
}

std::string group_sql(Ctx &x, const FilterGroup &g)
{
    std::vector<std::string> parts;
    for (auto &c : g.conds) parts.push_back(cond_sql(x, c));
    for (auto &s : g.groups) {
        std::string t = group_sql(x, s);
        if (!t.empty()) parts.push_back("(" + t + ")");
    }
    std::string r;
    const char *join = g.logic == Logic::And ? " AND " : " OR ";
    for (size_t k = 0; k < parts.size(); ++k) r += (k ? join : "") + parts[k];
    return r;
}

std::string where_clause(Ctx &x, const FilterGroup &g)
{
    std::string w = group_sql(x, g);
    return w.empty() ? "" : " WHERE " + w;
}

std::vector<SortKey> effective_sort(const TableMeta &m, const std::vector<SortKey> &sort)
{
    std::vector<SortKey> out = sort;
    // Deterministic paging: always finish with the primary key (or the edit key).
    std::vector<std::string> keys = m.edit.key_columns;
    if (keys.empty())
        for (auto &c : m.columns)
            if (c.is_pk()) keys.push_back(c.name);
    for (auto &k : keys) {
        bool present = std::any_of(out.begin(), out.end(), [&](const SortKey &s) { return s.column == k; });
        if (!present) out.push_back({ k, false });
    }
    return out;
}

std::string order_clause(Ctx &x, const std::vector<SortKey> &sort)
{
    if (sort.empty()) return "";
    std::string r = " ORDER BY ";
    for (size_t k = 0; k < sort.size(); ++k) {
        const ColumnInfo &c = x.col(sort[k].column);
        r += (k ? ", " : "") + quote_ident(x.d, c.name) + (sort[k].desc ? " DESC" : " ASC");
    }
    return r;
}

std::string select_list(Ctx &x, const std::vector<std::string> &cols)
{
    std::string r;
    if (cols.empty()) {
        for (size_t k = 0; k < x.m.columns.size(); ++k)
            r += (k ? ", " : "") + quote_ident(x.d, x.m.columns[k].name);
    } else {
        for (size_t k = 0; k < cols.size(); ++k)
            r += (k ? ", " : "") + quote_ident(x.d, x.col(cols[k]).name);
    }
    return r;
}

} // namespace

std::string quote_ident(Dialect d, const std::string &name)
{
    check_ident(name);
    std::string r;
    switch (d) {
    case Dialect::MySql:
        r = "`";
        for (char c : name) { if (c == '`') r += '`'; r += c; }
        return r + "`";
    case Dialect::MsSql:
        r = "[";
        for (char c : name) { if (c == ']') r += ']'; r += c; }
        return r + "]";
    default:
        r = "\"";
        for (char c : name) { if (c == '"') r += '"'; r += c; }
        return r + "\"";
    }
}

std::string qualified_table(Dialect d, const TableRef &t)
{
    switch (d) {
    case Dialect::Sqlite:
        return t.database.empty() || t.database == "main" ? quote_ident(d, t.name) : quote_ident(d, t.database) + "." + quote_ident(d, t.name);
    case Dialect::Postgres:
        return (t.schema.empty() ? "" : quote_ident(d, t.schema) + ".") + quote_ident(d, t.name);
    case Dialect::MySql:
        return (t.database.empty() ? "" : quote_ident(d, t.database) + ".") + quote_ident(d, t.name);
    case Dialect::MsSql:
        return (t.database.empty() ? "" : quote_ident(d, t.database) + ".") + quote_ident(d, t.schema.empty() ? "dbo" : t.schema) + "." + quote_ident(d, t.name);
    }
    return quote_ident(d, t.name);
}

bool dialect_has_regex(Dialect d) { return d == Dialect::Postgres || d == Dialect::MySql; }

std::string like_escape(const std::string &s)
{
    std::string r;
    for (char c : s) {
        if (c == '!' || c == '%' || c == '_') r += '!';
        r += c;
    }
    return r;
}

Built build_select(Dialect d, const TableMeta &m, const PageRequest &req, bool extra)
{
    Ctx x { d, m, {} };
    std::string cols = select_list(x, req.columns);
    std::string where = where_clause(x, req.filter);
    auto sort = effective_sort(m, req.sort);
    uint64_t lim = (uint64_t)req.limit + (extra ? 1 : 0);
    std::string sql = "SELECT " + cols + " FROM " + qualified_table(d, m.table) + where;
    if (d == Dialect::MsSql) {
        if (sort.empty()) {
            if (m.columns.empty()) invalid("table has no columns");
            sort.push_back({ m.columns[0].name, false });
        }
        sql += order_clause(x, sort) + " OFFSET " + std::to_string(req.offset) + " ROWS FETCH NEXT " + std::to_string(lim) + " ROWS ONLY";
    } else {
        sql += order_clause(x, sort) + " LIMIT " + std::to_string(lim) + " OFFSET " + std::to_string(req.offset);
    }
    return { sql, std::move(x.params) };
}

Built build_count(Dialect d, const TableMeta &m, const FilterGroup &f)
{
    Ctx x { d, m, {} };
    std::string where = where_clause(x, f);
    return { std::string("SELECT ") + (d == Dialect::MsSql ? "COUNT_BIG(*)" : "COUNT(*)") + " FROM " + qualified_table(d, m.table) + where, std::move(x.params) };
}

Built build_distinct(Dialect d, const TableMeta &m, const DistinctRequest &r)
{
    Ctx x { d, m, {} };
    const ColumnInfo &c = x.col(r.column);
    if (c.cls == TypeClass::Binary) invalid("distinct values are not available for binary columns");
    uint32_t lim = std::clamp<uint32_t>(r.limit, 1, 1000);
    std::string q = quote_ident(d, c.name);
    std::string where;
    if (!r.search.empty()) where = " WHERE " + like_expr(x, c, Pat::Contains, r.search, false, false);
    std::string sql;
    if (d == Dialect::MsSql)
        sql = "SELECT DISTINCT TOP (" + std::to_string(lim) + ") " + q + " FROM " + qualified_table(d, m.table) + where + " ORDER BY " + q;
    else
        sql = "SELECT DISTINCT " + q + " FROM " + qualified_table(d, m.table) + where + " ORDER BY " + q + " LIMIT " + std::to_string(lim);
    return { sql, std::move(x.params) };
}

std::vector<Stmt> build_changes(Dialect d, const TableMeta &m, const std::vector<Change> &changes)
{
    std::vector<Stmt> out;
    std::string tbl = qualified_table(d, m.table);

    auto key_where = [&](Ctx &x, const std::vector<std::pair<std::string, Value>> &key) {
        if (key.empty()) throw DbError(ErrKind::Validation, "row cannot be identified: no key");
        std::string w;
        for (size_t k = 0; k < key.size(); ++k) {
            const ColumnInfo &c = x.col(key[k].first);
            if (key[k].second.is_null()) invalid("key column " + c.name + " is NULL");
            w += (k ? " AND " : "") + quote_ident(d, c.name) + " = " + x.bind(key[k].second);
        }
        return w;
    };

    for (auto &ch : changes) {
        Ctx x { d, m, {} };
        Stmt st;
        st.type = ch.type;
        switch (ch.type) {
        case Change::Type::Insert: {
            std::string cols, vals;
            int n = 0;
            for (auto &kv : ch.values) {
                const ColumnInfo &c = x.col(kv.first);
                if (c.generated) invalid("column " + c.name + " is generated and cannot be written");
                if (kv.second.kind == Value::Kind::Default) continue;
                cols += (n ? ", " : "") + quote_ident(d, c.name);
                vals += (n ? ", " : "") + x.bind(kv.second);
                ++n;
            }
            if (n == 0)
                st.q.sql = d == Dialect::MySql ? "INSERT INTO " + tbl + " () VALUES ()" : "INSERT INTO " + tbl + " DEFAULT VALUES";
            else
                st.q.sql = "INSERT INTO " + tbl + " (" + cols + ") VALUES (" + vals + ")";
            break;
        }
        case Change::Type::Update: {
            if (ch.set.empty()) continue;
            std::string set;
            for (size_t k = 0; k < ch.set.size(); ++k) {
                const ColumnInfo &c = x.col(ch.set[k].column);
                if (c.generated) invalid("column " + c.name + " is generated and cannot be written");
                set += (k ? ", " : "") + quote_ident(d, c.name) + " = ";
                if (ch.set[k].value.kind == Value::Kind::Default) {
                    if (d == Dialect::Sqlite) throw DbError(ErrKind::Unsupported, "DEFAULT is not supported by SQLite");
                    set += "DEFAULT";
                } else set += x.bind(ch.set[k].value);
            }
            std::string where = key_where(x, ch.key);
            // Optimistic concurrency: the row must still hold the values the user saw.
            for (auto &s : ch.set) {
                const ColumnInfo &c = x.col(s.column);
                if (c.cls == TypeClass::Json || c.cls == TypeClass::Binary) continue;
                if (s.original.kind == Value::Kind::Missing || s.original.kind == Value::Kind::Default) continue;
                std::string q = quote_ident(d, c.name);
                where += s.original.is_null() ? " AND " + q + " IS NULL" : " AND " + q + " = " + x.bind(s.original);
            }
            st.q.sql = "UPDATE " + tbl + " SET " + set + " WHERE " + where;
            st.must_affect_one = true;
            break;
        }
        case Change::Type::Delete:
            st.q.sql = "DELETE FROM " + tbl + " WHERE " + key_where(x, ch.key);
            st.must_affect_one = true;
            break;
        }
        st.q.params = std::move(x.params);
        out.push_back(std::move(st));
    }
    return out;
}

std::string render_literal(const Value &v)
{
    auto quote = [](const std::string &s) {
        std::string r = "'";
        for (char c : s) { if (c == '\'') r += '\''; r += c; }
        return r + "'";
    };
    switch (v.kind) {
    case Value::Kind::Null: return "NULL";
    case Value::Kind::Missing: return "NULL";
    case Value::Kind::Default: return "DEFAULT";
    case Value::Kind::Bool: return v.i ? "1" : "0";
    case Value::Kind::Int: return std::to_string(v.i);
    case Value::Kind::Float: return v.display();
    case Value::Kind::Decimal: return v.s;
    case Value::Kind::Bytes: return hex_preview(v.s, 4096);
    default: return quote(v.s);
    }
}

std::string inline_params(Dialect d, const std::string &sql, const std::vector<Value> &params)
{
    std::string out;
    if (d == Dialect::Postgres) {
        for (size_t k = 0; k < sql.size();) {
            if (sql[k] == '$' && k + 1 < sql.size() && isdigit((unsigned char)sql[k + 1])) {
                size_t e = k + 1;
                while (e < sql.size() && isdigit((unsigned char)sql[e])) ++e;
                size_t n = std::stoul(sql.substr(k + 1, e - k - 1));
                out += n >= 1 && n <= params.size() ? render_literal(params[n - 1]) : sql.substr(k, e - k);
                k = e;
            } else out += sql[k++];
        }
        return out;
    }
    size_t n = 0;
    bool in_str = false;
    for (char c : sql) {
        if (c == '\'') in_str = !in_str;
        if (c == '?' && !in_str && n < params.size()) out += render_literal(params[n++]);
        else out += c;
    }
    return out;
}

} // namespace dbp
