#include "postgres_conn.h"

#include <winsock2.h>

#include <libpq-fe.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "../core/sqlgen.h"

namespace dbp {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t ms_since(Clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

std::string lower(std::string s)
{
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim_nl(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// PostgreSQL error classification from SQLSTATE.
ErrKind kind_for_state(const char *st)
{
    if (!st) return ErrKind::Query;
    std::string s = st;
    if (s == "57014") return ErrKind::Cancelled;
    if (s == "25006") return ErrKind::ReadOnly;
    if (s.rfind("08", 0) == 0 || s == "57P01" || s == "57P02" || s == "57P03") return ErrKind::Connection;
    return ErrKind::Query;
}

struct Res {
    PGresult *r = nullptr;
    Res() = default;
    explicit Res(PGresult *p) : r(p) {}
    ~Res() { if (r) PQclear(r); }
    Res(const Res &) = delete;
    Res &operator=(const Res &) = delete;
    Res(Res &&o) noexcept : r(o.r) { o.r = nullptr; }
    Res &operator=(Res &&o) noexcept
    {
        if (this != &o) {
            if (r) PQclear(r);
            r = o.r;
            o.r = nullptr;
        }
        return *this;
    }
    ExecStatusType status() const { return PQresultStatus(r); }
};

[[noreturn]] void throw_res(PGconn *c, PGresult *r)
{
    const char *state = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : nullptr;
    const char *prim = r ? PQresultErrorField(r, PG_DIAG_MESSAGE_PRIMARY) : nullptr;
    std::string msg = prim ? prim : trim_nl(PQerrorMessage(c));
    if (r) {
        const char *det = PQresultErrorField(r, PG_DIAG_MESSAGE_DETAIL);
        if (det && *det) msg += " (" + std::string(det) + ")";
    }
    ErrKind k = kind_for_state(state);
    if (!state && PQstatus(c) == CONNECTION_BAD) k = ErrKind::Connection;
    throw DbError(k, msg.empty() ? "unknown PostgreSQL error" : msg);
}

// -------------------------------------------------------------- value conversion
TypeClass class_from_typname(const std::string &t_in, char typtype)
{
    std::string t = lower(t_in);
    if (t == "int2" || t == "int4" || t == "int8" || t == "oid" || t == "serial") return TypeClass::Integer;
    if (t == "numeric") return TypeClass::Decimal;
    if (t == "float4" || t == "float8") return TypeClass::Float;
    if (t == "bool") return TypeClass::Bool;
    if (t == "text") return TypeClass::LongText;
    if (t == "varchar" || t == "bpchar" || t == "name" || t == "citext") return TypeClass::Text;
    if (t == "date") return TypeClass::Date;
    if (t == "time" || t == "timetz") return TypeClass::Time;
    if (t == "timestamp" || t == "timestamptz") return TypeClass::DateTime;
    if (t == "uuid") return TypeClass::Uuid;
    if (t == "json" || t == "jsonb") return TypeClass::Json;
    if (t == "bytea") return TypeClass::Binary;
    if (typtype == 'e') return TypeClass::Text;
    return TypeClass::Other;
}

TypeClass class_from_oid(Oid o)
{
    switch (o) {
    case 16: return TypeClass::Bool;
    case 20: case 21: case 23: case 26: return TypeClass::Integer;
    case 700: case 701: return TypeClass::Float;
    case 1700: return TypeClass::Decimal;
    case 1082: return TypeClass::Date;
    case 1083: case 1266: return TypeClass::Time;
    case 1114: case 1184: return TypeClass::DateTime;
    case 114: case 3802: return TypeClass::Json;
    case 2950: return TypeClass::Uuid;
    case 17: return TypeClass::Binary;
    case 25: return TypeClass::LongText;
    case 18: case 19: case 1042: case 1043: return TypeClass::Text;
    default: return TypeClass::Other;
    }
}

int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Value to_value(TypeClass cls, const char *v, int len)
{
    switch (cls) {
    case TypeClass::Integer: {
        int64_t n = 0;
        auto r = std::from_chars(v, v + len, n);
        if (r.ec == std::errc() && r.ptr == v + len) return Value::integer(n);
        return Value::text(std::string(v, len));
    }
    case TypeClass::Float: return Value::real(std::strtod(std::string(v, len).c_str(), nullptr));
    case TypeClass::Bool: return Value::boolean(len > 0 && (v[0] == 't' || v[0] == 'T' || v[0] == '1'));
    case TypeClass::Binary: {
        std::string out;
        if (len >= 2 && v[0] == '\\' && v[1] == 'x') {
            out.reserve((len - 2) / 2);
            for (int k = 2; k + 1 < len; k += 2) {
                int a = hexval(v[k]), b = hexval(v[k + 1]);
                if (a < 0 || b < 0) break;
                out += (char)((a << 4) | b);
            }
        } else out.assign(v, len);
        return Value::of(Value::Kind::Bytes, std::move(out));
    }
    default: return Value::from_text_for(cls, std::string(v, len));
    }
}

// -------------------------------------------------------------- parameters (all sent as text; the server infers the type)
struct ParamSet {
    std::vector<std::string> store;
    std::vector<const char *> ptrs;
    explicit ParamSet(const std::vector<Value> &params)
    {
        store.reserve(params.size());
        std::vector<bool> nul;
        for (auto &v : params) {
            switch (v.kind) {
            case Value::Kind::Null:
            case Value::Kind::Missing:
            case Value::Kind::Default:
                store.emplace_back();
                nul.push_back(true);
                continue;
            case Value::Kind::Bool: store.push_back(v.i ? "true" : "false"); break;
            case Value::Kind::Int: store.push_back(std::to_string(v.i)); break;
            case Value::Kind::Float: {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.17g", v.d);
                store.push_back(buf);
                break;
            }
            case Value::Kind::Bytes: {
                static const char *H = "0123456789abcdef";
                std::string h = "\\x";
                for (unsigned char c : v.s) {
                    h += H[c >> 4];
                    h += H[c & 15];
                }
                store.push_back(std::move(h));
                break;
            }
            default: store.push_back(v.s); break;
            }
            nul.push_back(false);
        }
        for (size_t k = 0; k < store.size(); ++k) ptrs.push_back(nul[k] ? nullptr : store[k].c_str());
    }
    int n() const { return (int)ptrs.size(); }
};

struct Sess {
    PGconn *c = nullptr;
    bool in_tx = false;
};

class PgConn : public Connector {
public:
    explicit PgConn(PgParams p) : p_(std::move(p)) {}
    ~PgConn() override { close(); }

    void open_first()
    {
        std::lock_guard<std::mutex> g(m_);
        home();
    }

    Capabilities caps() const override
    {
        Capabilities c;
        c.transactions = true;
        c.edit = !p_.read_only;
        c.regex = true;
        c.schemas = true;
        c.sql = true;
        c.default_keyword = true;
        return c;
    }

    std::string server_info() override
    {
        std::lock_guard<std::mutex> g(m_);
        PGconn *c = home().c;
        const char *v = PQparameterStatus(c, "server_version");
        return std::string("PostgreSQL ") + (v ? v : "");
    }

    void close() override
    {
        std::lock_guard<std::mutex> g(m_);
        closed_ = true;
        for (auto &kv : sessions_)
            if (kv.second.c) PQfinish(kv.second.c);
        sessions_.clear();
    }

    std::vector<ObjectInfo> list_databases() override
    {
        if (!p_.database.empty()) return { { p_.database, ObjectKind::Database } };
        auto rows = query_values(home_name(), "SELECT datname FROM pg_database WHERE datallowconn AND NOT datistemplate ORDER BY datname", {}, 1);
        std::vector<ObjectInfo> out;
        for (auto &r : rows) out.push_back({ str(r[0]), ObjectKind::Database });
        return out;
    }

    std::vector<ObjectInfo> list_schemas(const std::string &db) override
    {
        auto rows = query_values(db,
                                 "SELECT nspname FROM pg_namespace WHERE nspname NOT LIKE 'pg\\_toast%' AND nspname NOT LIKE 'pg\\_temp%' "
                                 "ORDER BY (nspname IN ('pg_catalog','information_schema')), nspname",
                                 {}, 1);
        std::vector<ObjectInfo> out;
        for (auto &r : rows) out.push_back({ str(r[0]), ObjectKind::Schema });
        return out;
    }

    std::vector<ObjectInfo> list_tables(const std::string &db, const std::string &schema) override
    {
        auto rows = query_values(db,
                                 "SELECT c.relname, c.relkind FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
                                 "WHERE n.nspname = $1 AND c.relkind IN ('r','p','v','m','f') AND NOT c.relispartition ORDER BY c.relkind IN ('v','m'), c.relname",
                                 { Value::text(schema.empty() ? "public" : schema) }, 2);
        std::vector<ObjectInfo> out;
        for (auto &r : rows) {
            std::string k = str(r[1]);
            out.push_back({ str(r[0]), k == "v" || k == "m" ? ObjectKind::View : ObjectKind::Table });
        }
        return out;
    }

    TableMeta table_meta(const TableRef &t) override
    {
        TableMeta m;
        m.table = t;
        if (m.table.schema.empty()) m.table.schema = "public";
        std::vector<Value> key { Value::text(m.table.schema), Value::text(m.table.name) };
        const std::string &db = t.database;

        auto info = query_values(db,
                                 "SELECT c.relkind, c.reltuples::bigint::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
                                 "WHERE n.nspname = $1 AND c.relname = $2 AND c.relkind IN ('r','p','v','m','f')",
                                 key, 2);
        if (info.empty()) throw DbError(ErrKind::Query, "table not found: " + t.label());
        std::string kind = str(info[0][0]);
        m.is_view = kind == "v" || kind == "m";
        if (!m.is_view) {
            int64_t est = std::strtoll(str(info[0][1]).c_str(), nullptr, 10);
            if (est >= 0) m.estimated_rows = est;
        }

        auto cols = query_values(db,
                                 "SELECT a.attname, format_type(a.atttypid, a.atttypmod), t.typname, t.typtype::text, a.attnotnull::text, a.atthasdef::text, "
                                 "a.attidentity::text, a.attgenerated::text "
                                 "FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
                                 "JOIN pg_type t ON t.oid = a.atttypid "
                                 "WHERE n.nspname = $1 AND c.relname = $2 AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum",
                                 key, 8);
        for (auto &r : cols) {
            ColumnInfo c;
            c.name = str(r[0]);
            c.native_type = str(r[1]);
            std::string tt = str(r[3]);
            c.cls = class_from_typname(str(r[2]), tt.empty() ? 'b' : tt[0]);
            c.nullable = str(r[4]) != "true";
            c.has_default = str(r[5]) == "true";
            c.identity = !str(r[6]).empty();
            c.generated = !str(r[7]).empty();
            m.columns.push_back(std::move(c));
        }
        if (m.columns.empty()) throw DbError(ErrKind::Query, "table has no visible columns: " + t.label());

        auto pk = query_values(db,
                               "SELECT a.attname, k.ord::text FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
                               "CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS k(attnum, ord) "
                               "JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = k.attnum "
                               "WHERE i.indisprimary AND n.nspname = $1 AND c.relname = $2 ORDER BY k.ord",
                               key, 2);
        std::vector<std::string> pks;
        for (auto &r : pk) {
            int pos = (int)std::strtol(str(r[1]).c_str(), nullptr, 10);
            pks.push_back(str(r[0]));
            for (auto &c : m.columns)
                if (c.name == str(r[0])) c.pk_position = pos;
        }

        auto fk = query_values(db,
                               "SELECT a.attname, cf.relname, af.attname FROM pg_constraint con JOIN pg_class c ON c.oid = con.conrelid "
                               "JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_class cf ON cf.oid = con.confrelid "
                               "CROSS JOIN LATERAL unnest(con.conkey, con.confkey) AS u(k, fk) "
                               "JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = u.k JOIN pg_attribute af ON af.attrelid = cf.oid AND af.attnum = u.fk "
                               "WHERE con.contype = 'f' AND n.nspname = $1 AND c.relname = $2",
                               key, 3);
        for (auto &r : fk)
            for (auto &c : m.columns)
                if (c.name == str(r[0])) c.foreign_key = str(r[1]) + "." + str(r[2]);

        auto ix = query_values(db,
                               "SELECT ic.relname, i.indisunique::text, pg_get_indexdef(i.indexrelid, k.ord::int, false) "
                               "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid JOIN pg_class c ON c.oid = i.indrelid "
                               "JOIN pg_namespace n ON n.oid = c.relnamespace "
                               "CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS k(attnum, ord) "
                               "WHERE n.nspname = $1 AND c.relname = $2 AND k.ord <= i.indnkeyatts ORDER BY ic.relname, k.ord",
                               key, 3);
        for (auto &r : ix) {
            std::string name = str(r[0]);
            if (m.indexes.empty() || m.indexes.back().name != name) {
                IndexInfo i;
                i.name = name;
                i.unique = str(r[1]) == "true";
                m.indexes.push_back(i);
            }
            m.indexes.back().columns.push_back(str(r[2]));
        }

        if (m.is_view) m.edit = { {}, false, kind == "m" ? "Materialized views are read-only" : "Views are read-only" };
        else if (kind == "f") m.edit = { {}, false, "Foreign tables are not editable" };
        else if (p_.read_only) m.edit = { {}, false, "Connection is read-only" };
        else if (pks.empty()) m.edit = { {}, false, "No primary key: rows cannot be identified unambiguously" };
        else {
            m.edit.key_columns = pks;
            m.edit.editable = true;
        }
        return m;
    }

    Page fetch_page(const TableMeta &m, const PageRequest &req, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        Built b = build_select(Dialect::Postgres, m, req);
        Page p;
        p.generated = inline_params(Dialect::Postgres, b.sql, b.params);
        auto cls = result_classes(m, req);
        std::lock_guard<std::mutex> g(m_);
        Sess &s = session(m.table.database);
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        HookGuard hg(c, canceller(s));
        Res r = run(s, b.sql, b.params);
        int n = PQntuples(r.r);
        if ((size_t)PQnfields(r.r) != cls.size()) throw DbError(ErrKind::Query, "unexpected number of result columns");
        for (int i = 0; i < n; ++i) {
            if (p.rows.size() >= req.limit) {
                p.has_more = true;
                break;
            }
            p.rows.push_back(read_row(r.r, i, cls));
        }
        p.elapsed_ms = ms_since(t0);
        return p;
    }

    uint64_t count(const TableMeta &m, const FilterGroup &f, const CancelPtr &c) override
    {
        Built b = build_count(Dialect::Postgres, m, f);
        std::lock_guard<std::mutex> g(m_);
        Sess &s = session(m.table.database);
        HookGuard hg(c, canceller(s));
        Res r = run(s, b.sql, b.params);
        if (PQntuples(r.r) < 1 || PQgetisnull(r.r, 0, 0)) return 0;
        return std::strtoull(PQgetvalue(r.r, 0, 0), nullptr, 10);
    }

    std::vector<Value> distinct_values(const TableMeta &m, const DistinctRequest &r, const CancelPtr &c) override
    {
        Built b = build_distinct(Dialect::Postgres, m, r);
        const ColumnInfo *ci = m.column(r.column);
        std::lock_guard<std::mutex> g(m_);
        Sess &s = session(m.table.database);
        HookGuard hg(c, canceller(s));
        Res res = run(s, b.sql, b.params);
        std::vector<Value> out;
        for (int i = 0; i < PQntuples(res.r); ++i)
            out.push_back(PQgetisnull(res.r, i, 0) ? Value::null() : to_value(ci->cls, PQgetvalue(res.r, i, 0), PQgetlength(res.r, i, 0)));
        return out;
    }

    uint64_t stream_rows(const TableMeta &m, const PageRequest &req, size_t batch, const RowBatchFn &fn, const CancelPtr &c) override
    {
        PageRequest r = req;
        r.limit = 0x7fffffff;
        r.offset = 0;
        Built b = build_select(Dialect::Postgres, m, r, false);
        auto cls = result_classes(m, req);
        std::lock_guard<std::mutex> g(m_);
        Sess &s = session(m.table.database);
        HookGuard hg(c, canceller(s));
        ParamSet ps(b.params);
        if (!PQsendQueryParams(s.c, b.sql.c_str(), ps.n(), nullptr, ps.ptrs.data(), nullptr, nullptr, 0)) throw_res(s.c, nullptr);
        PQsetSingleRowMode(s.c);
        std::vector<std::vector<Value>> buf;
        uint64_t total = 0;
        bool stopped = false;
        std::string failure;
        ErrKind fkind = ErrKind::Query;
        for (;;) {
            Res res(PQgetResult(s.c));
            if (!res.r) break;
            ExecStatusType st = res.status();
            if (st == PGRES_SINGLE_TUPLE) {
                if (stopped) continue;
                buf.push_back(read_row(res.r, 0, cls));
                ++total;
                if (buf.size() >= batch) {
                    if (!fn(buf)) {
                        stopped = true;
                        cancel_now(s);
                    }
                    buf.clear();
                }
            } else if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
                const char *state = PQresultErrorField(res.r, PG_DIAG_SQLSTATE);
                const char *prim = PQresultErrorField(res.r, PG_DIAG_MESSAGE_PRIMARY);
                if (!stopped) {
                    failure = prim ? prim : trim_nl(PQerrorMessage(s.c));
                    fkind = kind_for_state(state);
                }
            }
        }
        if (!failure.empty()) throw DbError(fkind, failure);
        if (!stopped && !buf.empty()) fn(buf);
        return total;
    }

    ApplyReport apply_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        if (p_.read_only) throw DbError(ErrKind::ReadOnly, "Connection is read-only");
        if (!m.edit.editable) throw DbError(ErrKind::Validation, m.edit.blocked_reason);
        auto stmts = build_changes(Dialect::Postgres, m, changes);
        std::lock_guard<std::mutex> g(m_);
        Sess &s = session(m.table.database);
        bool own_tx = !s.in_tx;
        if (own_tx) simple(s, "BEGIN");
        ApplyReport rep;
        try {
            for (auto &st : stmts) {
                Res r = run(s, st.q.sql, st.q.params);
                uint64_t n = std::strtoull(PQcmdTuples(r.r), nullptr, 10);
                if (st.must_affect_one && n != 1)
                    throw DbError(ErrKind::Conflict, st.type == Change::Type::Delete
                                                          ? "A row to delete no longer exists or was changed by someone else"
                                                          : "A row was modified or deleted by someone else since it was loaded");
                if (st.type == Change::Type::Insert) rep.inserted += (int)n;
                else if (st.type == Change::Type::Update) rep.updated += (int)n;
                else rep.deleted += (int)n;
            }
            if (own_tx) simple(s, "COMMIT");
        } catch (...) {
            if (own_tx && s.c) {
                PGresult *r = PQexec(s.c, "ROLLBACK");
                if (r) PQclear(r);
            }
            throw;
        }
        return rep;
    }

    std::string preview_query(const TableMeta &m, const PageRequest &req) override
    {
        Built b = build_select(Dialect::Postgres, m, req);
        return inline_params(Dialect::Postgres, b.sql, b.params);
    }

    std::string preview_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        std::string out;
        for (auto &st : build_changes(Dialect::Postgres, m, changes)) out += inline_params(Dialect::Postgres, st.q.sql, st.q.params) + ";\n";
        return out;
    }

    QueryOutput execute(const std::string &text, size_t max_rows, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        std::lock_guard<std::mutex> g(m_);
        Sess &s = home();
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        HookGuard hg(c, canceller(s));
        QueryOutput out;
        if (!PQsendQuery(s.c, text.c_str())) throw_res(s.c, nullptr);
        PQsetSingleRowMode(s.c);
        std::vector<TypeClass> cls;
        bool in_set = false, stopped = false;
        std::string failure;
        ErrKind fkind = ErrKind::Query;
        for (;;) {
            Res res(PQgetResult(s.c));
            if (!res.r) break;
            ExecStatusType st = res.status();
            if (st == PGRES_SINGLE_TUPLE || st == PGRES_TUPLES_OK) {
                if (!in_set) {
                    out = QueryOutput(); // keep only the last result set
                    cls.clear();
                    int nf = PQnfields(res.r);
                    for (int k = 0; k < nf; ++k) {
                        out.columns.push_back(PQfname(res.r, k));
                        cls.push_back(class_from_oid(PQftype(res.r, k)));
                    }
                    in_set = true;
                }
                // Single-row results hold one row; a plain TUPLES_OK holds all rows of its statement (or none, when it ends a streamed one).
                for (int i = 0; i < PQntuples(res.r) && !stopped; ++i) {
                    if (out.rows.size() >= max_rows) {
                        out.truncated = true;
                        stopped = true;
                        cancel_now(s);
                    } else out.rows.push_back(read_row(res.r, i, cls));
                }
                if (st == PGRES_TUPLES_OK) in_set = false; // end of this statement's rows
            } else if (st == PGRES_COMMAND_OK) {
                out = QueryOutput();
                out.affected = std::strtoll(PQcmdTuples(res.r), nullptr, 10);
                in_set = false;
            } else if (st != PGRES_EMPTY_QUERY && !stopped) {
                const char *state = PQresultErrorField(res.r, PG_DIAG_SQLSTATE);
                const char *prim = PQresultErrorField(res.r, PG_DIAG_MESSAGE_PRIMARY);
                failure = prim ? prim : trim_nl(PQerrorMessage(s.c));
                fkind = kind_for_state(state);
            }
        }
        if (!failure.empty()) {
            throw DbError(fkind, failure);
        }
        out.elapsed_ms = ms_since(t0);
        out.message = out.columns.empty() ? std::to_string(out.affected < 0 ? 0 : out.affected) + " row(s) affected"
                                           : std::to_string(out.rows.size()) + " row(s)" + (out.truncated ? " (truncated)" : "");
        return out;
    }

    void begin() override
    {
        std::lock_guard<std::mutex> g(m_);
        Sess &s = home();
        if (s.in_tx) throw DbError(ErrKind::Query, "a transaction is already open");
        simple(s, "BEGIN");
        s.in_tx = true;
    }
    void commit() override
    {
        std::lock_guard<std::mutex> g(m_);
        Sess &s = home();
        simple(s, "COMMIT");
        s.in_tx = false;
    }
    void rollback() override
    {
        std::lock_guard<std::mutex> g(m_);
        Sess &s = home();
        simple(s, "ROLLBACK");
        s.in_tx = false;
    }
    bool in_transaction() const override
    {
        auto it = sessions_.find(home_name());
        return it != sessions_.end() && it->second.in_tx;
    }

private:
    std::string home_name() const { return p_.database.empty() ? "postgres" : p_.database; }
    Sess &home() { return session(home_name()); }

    // Called with m_ held. PostgreSQL connections are bound to one database, so the explorer keeps one per database it visits.
    Sess &session(const std::string &db_in)
    {
        if (closed_) throw DbError(ErrKind::Connection, "connection closed");
        std::string db = db_in.empty() ? home_name() : db_in;
        Sess &s = sessions_[db];
        if (s.c && PQstatus(s.c) == CONNECTION_OK) return s;
        if (s.c) {
            PQfinish(s.c);
            s.c = nullptr;
        }
        s.in_tx = false;
        s.c = open_raw(db);
        return s;
    }

    PGconn *open_raw(const std::string &db)
    {
        std::string port = std::to_string(p_.port ? p_.port : 5432);
        std::string opts = "-c datestyle=ISO,MDY -c bytea_output=hex -c client_min_messages=warning";
        if (p_.read_only) opts += " -c default_transaction_read_only=on";
        const char *sslmode = !p_.tls ? "prefer" : (p_.trust_server_cert ? "require" : "verify-full");
        std::vector<const char *> keys { "host", "port", "dbname", "user", "password", "sslmode", "connect_timeout", "client_encoding", "application_name", "options" };
        std::vector<const char *> vals { p_.host.c_str(), port.c_str(), db.c_str(), p_.user.c_str(), p_.password.c_str(), sslmode, "10", "UTF8", "Db Pro", opts.c_str() };
        std::string rootcert = "system";
        if (p_.tls && !p_.trust_server_cert) {
            keys.push_back("sslrootcert");
            vals.push_back(rootcert.c_str());
        }
        keys.push_back(nullptr);
        vals.push_back(nullptr);
        PGconn *c = PQconnectdbParams(keys.data(), vals.data(), 0);
        if (!c) throw DbError(ErrKind::Connection, "cannot initialise the PostgreSQL client library");
        if (PQstatus(c) != CONNECTION_OK) {
            std::string e = trim_nl(PQerrorMessage(c));
            PQfinish(c);
            if (p_.tls && (e.find("SSL") != std::string::npos || e.find("certificate") != std::string::npos))
                e += " (TLS is required and the certificate is validated. For a trusted server you may change the TLS options in the connection settings.)";
            throw DbError(ErrKind::Connection, e);
        }
        return c;
    }

    // Sends a cancel request to the backend. Safe from any thread; the PGcancel object is independent of the connection.
    static void cancel_now(Sess &s)
    {
        if (!s.c) return;
        if (PGcancel *cn = PQgetCancel(s.c)) {
            char err[256];
            PQcancel(cn, err, sizeof err);
            PQfreeCancel(cn);
        }
    }
    std::function<void()> canceller(Sess &s)
    {
        std::shared_ptr<PGcancel> cn(PQgetCancel(s.c), [](PGcancel *p) { if (p) PQfreeCancel(p); });
        return [cn] {
            if (!cn) return;
            char err[256];
            PQcancel(cn.get(), err, sizeof err);
        };
    }

    Res run(Sess &s, const std::string &sql, const std::vector<Value> &params)
    {
        ParamSet ps(params);
        Res r(PQexecParams(s.c, sql.c_str(), ps.n(), nullptr, ps.ptrs.data(), nullptr, nullptr, 0));
        if (!r.r) throw_res(s.c, nullptr);
        ExecStatusType st = r.status();
        if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) throw_res(s.c, r.r);
        return r;
    }

    void simple(Sess &s, const char *sql)
    {
        Res r(PQexec(s.c, sql));
        if (!r.r) throw_res(s.c, nullptr);
        if (r.status() != PGRES_COMMAND_OK) throw_res(s.c, r.r);
    }

    static std::vector<Value> read_row(PGresult *r, int row, const std::vector<TypeClass> &cls)
    {
        std::vector<Value> out;
        out.reserve(cls.size());
        for (size_t k = 0; k < cls.size(); ++k) {
            if (PQgetisnull(r, row, (int)k)) out.push_back(Value::null());
            else out.push_back(to_value(cls[k], PQgetvalue(r, row, (int)k), PQgetlength(r, row, (int)k)));
        }
        return out;
    }

    static std::string str(const Value &v) { return v.is_null() ? std::string() : v.s; }

    // Small metadata queries: every column comes back as text. Takes m_ itself.
    std::vector<std::vector<Value>> query_values(const std::string &db, const std::string &sql, const std::vector<Value> &params, size_t ncols)
    {
        std::lock_guard<std::mutex> g(m_);
        Sess &s = session(db);
        Res r = run(s, sql, params);
        std::vector<std::vector<Value>> out;
        std::vector<TypeClass> cls(ncols, TypeClass::Text);
        if ((size_t)PQnfields(r.r) != ncols) throw DbError(ErrKind::Query, "unexpected number of result columns");
        for (int i = 0; i < PQntuples(r.r); ++i) out.push_back(read_row(r.r, i, cls));
        return out;
    }

    static std::vector<TypeClass> result_classes(const TableMeta &m, const PageRequest &req)
    {
        std::vector<TypeClass> v;
        if (req.columns.empty())
            for (auto &c : m.columns) v.push_back(c.cls);
        else
            for (auto &n : req.columns) v.push_back(m.column(n)->cls);
        return v;
    }

    PgParams p_;
    std::map<std::string, Sess> sessions_;
    bool closed_ = false;
    std::mutex m_;
};

} // namespace

std::unique_ptr<Connector> connect_postgres(const PgParams &p)
{
    if (p.host.empty()) throw DbError(ErrKind::Validation, "host is required");
    auto c = std::make_unique<PgConn>(p);
    c->open_first(); // fail here, with the server's message, not on first use
    return c;
}

} // namespace dbp
