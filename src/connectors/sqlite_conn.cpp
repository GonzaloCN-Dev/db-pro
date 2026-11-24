#include "sqlite_conn.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>

#include "../core/sqlgen.h"

namespace dbp {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t ms_since(Clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

TypeClass classify(const std::string &decl_in)
{
    std::string t;
    for (char c : decl_in) t += (char)std::toupper((unsigned char)c);
    auto has = [&](const char *s) { return t.find(s) != std::string::npos; };
    if (has("INT")) return TypeClass::Integer;
    if (has("BOOL")) return TypeClass::Bool;
    if (has("CHAR") || has("CLOB")) return TypeClass::Text;
    if (has("TEXT")) return TypeClass::LongText;
    if (has("BLOB")) return TypeClass::Binary;
    if (has("REAL") || has("FLOA") || has("DOUB")) return TypeClass::Float;
    if (has("DECIMAL") || has("NUMERIC")) return TypeClass::Decimal;
    if (has("DATETIME") || has("TIMESTAMP")) return TypeClass::DateTime;
    if (has("DATE")) return TypeClass::Date;
    if (has("TIME")) return TypeClass::Time;
    if (has("JSON")) return TypeClass::Json;
    if (t.empty()) return TypeClass::Other;
    return TypeClass::Other;
}

class Stmt {
public:
    Stmt(sqlite3 *db, const std::string &sql) : db_(db)
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), (int)sql.size(), &s_, nullptr) != SQLITE_OK)
            throw DbError(ErrKind::Query, sqlite3_errmsg(db));
    }
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    sqlite3_stmt *get() const { return s_; }

    void bind(const std::vector<Value> &params)
    {
        for (size_t k = 0; k < params.size(); ++k) {
            int i = (int)k + 1;
            const Value &v = params[k];
            int rc = SQLITE_OK;
            switch (v.kind) {
            case Value::Kind::Null:
            case Value::Kind::Missing:
            case Value::Kind::Default: rc = sqlite3_bind_null(s_, i); break;
            case Value::Kind::Bool:
            case Value::Kind::Int: rc = sqlite3_bind_int64(s_, i, v.i); break;
            case Value::Kind::Float: rc = sqlite3_bind_double(s_, i, v.d); break;
            case Value::Kind::Bytes: rc = sqlite3_bind_blob64(s_, i, v.s.data(), v.s.size(), SQLITE_TRANSIENT); break;
            default: rc = sqlite3_bind_text(s_, i, v.s.data(), (int)v.s.size(), SQLITE_TRANSIENT); break;
            }
            if (rc != SQLITE_OK) throw DbError(ErrKind::Query, sqlite3_errmsg(db_));
        }
    }

    // Returns true when a row is available.
    bool step()
    {
        int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        if (rc == SQLITE_INTERRUPT) throw DbError(ErrKind::Cancelled, "operation cancelled");
        throw DbError(ErrKind::Query, sqlite3_errmsg(db_));
    }

    Value column(int c, TypeClass cls) const
    {
        switch (sqlite3_column_type(s_, c)) {
        case SQLITE_NULL: return Value::null();
        case SQLITE_INTEGER: {
            int64_t n = sqlite3_column_int64(s_, c);
            if (cls == TypeClass::Bool) return Value::boolean(n != 0);
            return Value::integer(n);
        }
        case SQLITE_FLOAT: return Value::real(sqlite3_column_double(s_, c));
        case SQLITE_BLOB: {
            const void *p = sqlite3_column_blob(s_, c);
            return Value::of(Value::Kind::Bytes, std::string((const char *)p, (size_t)sqlite3_column_bytes(s_, c)));
        }
        default: {
            std::string t((const char *)sqlite3_column_text(s_, c), (size_t)sqlite3_column_bytes(s_, c));
            return Value::from_text_for(cls, std::move(t));
        }
        }
    }

private:
    sqlite3 *db_;
    sqlite3_stmt *s_ = nullptr;
};

class SqliteConn : public Connector {
public:
    SqliteConn(sqlite3 *db, std::string path, bool ro) : db_(db), path_(std::move(path)), ro_(ro) {}
    ~SqliteConn() override { close(); }

    Capabilities caps() const override
    {
        Capabilities c;
        c.transactions = true;
        c.edit = !ro_;
        c.sql = true;
        c.regex = false;
        c.default_keyword = false;
        return c;
    }

    std::string server_info() override { return std::string("SQLite ") + sqlite3_libversion(); }

    void close() override
    {
        std::lock_guard<std::mutex> g(m_);
        if (db_) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    }

    std::vector<ObjectInfo> list_databases() override { return { { "main", ObjectKind::Database } }; }
    std::vector<ObjectInfo> list_schemas(const std::string &) override { return {}; }

    std::vector<ObjectInfo> list_tables(const std::string &, const std::string &) override
    {
        std::lock_guard<std::mutex> g(m_);
        Stmt s(db_, "SELECT name, type FROM sqlite_master WHERE type IN ('table','view') AND name NOT LIKE 'sqlite!_%' ESCAPE '!' ORDER BY type, name COLLATE NOCASE");
        std::vector<ObjectInfo> out;
        while (s.step()) {
            ObjectInfo o;
            o.name = (const char *)sqlite3_column_text(s.get(), 0);
            o.kind = std::string((const char *)sqlite3_column_text(s.get(), 1)) == "view" ? ObjectKind::View : ObjectKind::Table;
            out.push_back(o);
        }
        return out;
    }

    TableMeta table_meta(const TableRef &t) override
    {
        std::lock_guard<std::mutex> g(m_);
        TableMeta m;
        m.table = t;
        m.table.database = "main";
        {
            Stmt s(db_, "SELECT type FROM sqlite_master WHERE name = ?");
            s.bind({ Value::text(t.name) });
            if (!s.step()) throw DbError(ErrKind::Query, "table not found: " + t.name);
            m.is_view = std::string((const char *)sqlite3_column_text(s.get(), 0)) == "view";
        }
        bool has_rowid = true;
        {
            Stmt s(db_, "SELECT sql FROM sqlite_master WHERE name = ?");
            s.bind({ Value::text(t.name) });
            if (s.step() && sqlite3_column_text(s.get(), 0)) {
                std::string ddl = (const char *)sqlite3_column_text(s.get(), 0);
                for (auto &c : ddl) c = (char)std::toupper((unsigned char)c);
                if (ddl.find("WITHOUT ROWID") != std::string::npos) has_rowid = false;
            }
        }
        {
            Stmt s(db_, "SELECT cid, name, type, \"notnull\", dflt_value, pk, hidden FROM pragma_table_xinfo(?)");
            s.bind({ Value::text(t.name) });
            while (s.step()) {
                ColumnInfo c;
                c.name = (const char *)sqlite3_column_text(s.get(), 1);
                c.native_type = sqlite3_column_text(s.get(), 2) ? (const char *)sqlite3_column_text(s.get(), 2) : "";
                c.cls = classify(c.native_type);
                c.nullable = sqlite3_column_int(s.get(), 3) == 0;
                c.has_default = sqlite3_column_type(s.get(), 4) != SQLITE_NULL;
                c.pk_position = sqlite3_column_int(s.get(), 5);
                int hidden = sqlite3_column_int(s.get(), 6);
                c.generated = hidden == 2 || hidden == 3;
                if (hidden == 1) continue; // virtual table hidden columns
                m.columns.push_back(c);
            }
        }
        {
            // INTEGER PRIMARY KEY is an alias of rowid: the server assigns it.
            int pk_cols = 0;
            for (auto &c : m.columns) pk_cols += c.is_pk();
            if (pk_cols == 1)
                for (auto &c : m.columns)
                    if (c.is_pk() && c.native_type.size() == 7) {
                        std::string u;
                        for (char ch : c.native_type) u += (char)std::toupper((unsigned char)ch);
                        if (u == "INTEGER") c.identity = true;
                    }
        }
        {
            Stmt s(db_, "SELECT name, \"unique\" FROM pragma_index_list(?)");
            s.bind({ Value::text(t.name) });
            while (s.step()) {
                IndexInfo ix;
                ix.name = (const char *)sqlite3_column_text(s.get(), 0);
                ix.unique = sqlite3_column_int(s.get(), 1) != 0;
                m.indexes.push_back(ix);
            }
            for (auto &ix : m.indexes) {
                Stmt cs(db_, "SELECT name FROM pragma_index_info(?) ORDER BY seqno");
                cs.bind({ Value::text(ix.name) });
                while (cs.step())
                    if (sqlite3_column_text(cs.get(), 0)) ix.columns.push_back((const char *)sqlite3_column_text(cs.get(), 0));
            }
        }
        {
            Stmt s(db_, "SELECT \"from\", \"table\", \"to\" FROM pragma_foreign_key_list(?)");
            s.bind({ Value::text(t.name) });
            while (s.step()) {
                const char *from = (const char *)sqlite3_column_text(s.get(), 0);
                const char *tb = (const char *)sqlite3_column_text(s.get(), 1);
                const char *to = (const char *)sqlite3_column_text(s.get(), 2);
                for (auto &c : m.columns)
                    if (from && c.name == from) c.foreign_key = std::string(tb ? tb : "") + "." + (to ? to : "");
            }
        }
        // Row identity.
        if (m.is_view) {
            m.edit = { {}, false, "Views are read-only" };
        } else if (ro_) {
            m.edit = { {}, false, "Connection is read-only" };
        } else {
            std::vector<std::pair<int, std::string>> pk;
            for (auto &c : m.columns)
                if (c.is_pk()) pk.push_back({ c.pk_position, c.name });
            std::sort(pk.begin(), pk.end());
            if (!pk.empty()) {
                for (auto &p : pk) m.edit.key_columns.push_back(p.second);
                m.edit.editable = true;
            } else if (has_rowid) {
                // Hidden rowid is a stable identifier in rowid tables unless VACUUM renumbers it;
                // without an explicit key we refuse rather than risk editing the wrong row.
                m.edit = { {}, false, "No primary key: rows cannot be identified unambiguously" };
            } else {
                m.edit = { {}, false, "No primary key" };
            }
        }
        return m;
    }

    Page fetch_page(const TableMeta &m, const PageRequest &req, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        Built b = build_select(Dialect::Sqlite, m, req);
        Page p;
        p.generated = inline_params(Dialect::Sqlite, b.sql, b.params);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [db = db_] { sqlite3_interrupt(db); });
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        Stmt s(db_, b.sql);
        s.bind(b.params);
        std::vector<const ColumnInfo *> cols = result_cols(m, req);
        int n = sqlite3_column_count(s.get());
        while (s.step()) {
            if (p.rows.size() >= req.limit) {
                p.has_more = true;
                break;
            }
            std::vector<Value> row;
            row.reserve(n);
            for (int k = 0; k < n; ++k) row.push_back(s.column(k, cols[k]->cls));
            p.rows.push_back(std::move(row));
        }
        p.elapsed_ms = ms_since(t0);
        return p;
    }

    uint64_t count(const TableMeta &m, const FilterGroup &f, const CancelPtr &c) override
    {
        Built b = build_count(Dialect::Sqlite, m, f);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [db = db_] { sqlite3_interrupt(db); });
        Stmt s(db_, b.sql);
        s.bind(b.params);
        if (!s.step()) return 0;
        return (uint64_t)sqlite3_column_int64(s.get(), 0);
    }

    std::vector<Value> distinct_values(const TableMeta &m, const DistinctRequest &r, const CancelPtr &c) override
    {
        Built b = build_distinct(Dialect::Sqlite, m, r);
        const ColumnInfo *ci = m.column(r.column);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [db = db_] { sqlite3_interrupt(db); });
        Stmt s(db_, b.sql);
        s.bind(b.params);
        std::vector<Value> out;
        while (s.step()) out.push_back(s.column(0, ci->cls));
        return out;
    }

    uint64_t stream_rows(const TableMeta &m, const PageRequest &req, size_t batch, const RowBatchFn &fn, const CancelPtr &c) override
    {
        // Same query without LIMIT/OFFSET so the whole filtered set is walked by one cursor.
        PageRequest r = req;
        r.limit = 0x7fffffff;
        r.offset = 0;
        Built b = build_select(Dialect::Sqlite, m, r, false);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [db = db_] { sqlite3_interrupt(db); });
        Stmt s(db_, b.sql);
        s.bind(b.params);
        std::vector<const ColumnInfo *> cols = result_cols(m, req);
        int n = sqlite3_column_count(s.get());
        std::vector<std::vector<Value>> buf;
        uint64_t total = 0;
        while (s.step()) {
            std::vector<Value> row;
            row.reserve(n);
            for (int k = 0; k < n; ++k) row.push_back(s.column(k, cols[k]->cls));
            buf.push_back(std::move(row));
            ++total;
            if (buf.size() >= batch) {
                if (!fn(buf)) return total;
                buf.clear();
            }
        }
        if (!buf.empty()) fn(buf);
        return total;
    }

    ApplyReport apply_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        if (ro_) throw DbError(ErrKind::ReadOnly, "Connection is read-only");
        if (!m.edit.editable) throw DbError(ErrKind::Validation, m.edit.blocked_reason);
        auto stmts = build_changes(Dialect::Sqlite, m, changes);
        std::lock_guard<std::mutex> g(m_);
        bool own_tx = !in_tx_;
        if (own_tx) exec_raw("BEGIN IMMEDIATE");
        ApplyReport rep;
        try {
            for (auto &st : stmts) {
                Stmt s(db_, st.q.sql);
                s.bind(st.q.params);
                while (s.step()) {}
                int n = sqlite3_changes(db_);
                if (st.must_affect_one && n != 1)
                    throw DbError(ErrKind::Conflict, st.type == Change::Type::Delete
                        ? "A row to delete no longer exists or was changed by someone else"
                        : "A row was modified or deleted by someone else since it was loaded");
                if (st.type == Change::Type::Insert) rep.inserted += n;
                else if (st.type == Change::Type::Update) rep.updated += n;
                else rep.deleted += n;
            }
            if (own_tx) exec_raw("COMMIT");
        } catch (...) {
            if (own_tx) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
        return rep;
    }

    std::string preview_query(const TableMeta &m, const PageRequest &req) override
    {
        Built b = build_select(Dialect::Sqlite, m, req);
        return inline_params(Dialect::Sqlite, b.sql, b.params);
    }

    std::string preview_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        std::string out;
        for (auto &st : build_changes(Dialect::Sqlite, m, changes))
            out += inline_params(Dialect::Sqlite, st.q.sql, st.q.params) + ";\n";
        return out;
    }

    QueryOutput execute(const std::string &text, size_t max_rows, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [db = db_] { sqlite3_interrupt(db); });
        QueryOutput out;
        const char *tail = text.c_str();
        const char *end = tail + text.size();
        int stmts = 0;
        while (tail < end) {
            sqlite3_stmt *raw = nullptr;
            if (sqlite3_prepare_v2(db_, tail, (int)(end - tail), &raw, &tail) != SQLITE_OK)
                throw DbError(ErrKind::Query, sqlite3_errmsg(db_));
            if (!raw) continue; // whitespace / comment only
            struct Fin { sqlite3_stmt *s; ~Fin() { sqlite3_finalize(s); } } fin { raw };
            ++stmts;
            if (ro_ && !sqlite3_stmt_readonly(raw)) throw DbError(ErrKind::ReadOnly, "Connection is read-only");
            int ncol = sqlite3_column_count(raw);
            out = QueryOutput(); // keep only the last statement's result set
            for (int k = 0; k < ncol; ++k) out.columns.push_back(sqlite3_column_name(raw, k));
            int rc;
            while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
                if (out.rows.size() >= max_rows) {
                    out.truncated = true;
                    break;
                }
                std::vector<Value> row;
                for (int k = 0; k < ncol; ++k) {
                    switch (sqlite3_column_type(raw, k)) {
                    case SQLITE_NULL: row.push_back(Value::null()); break;
                    case SQLITE_INTEGER: row.push_back(Value::integer(sqlite3_column_int64(raw, k))); break;
                    case SQLITE_FLOAT: row.push_back(Value::real(sqlite3_column_double(raw, k))); break;
                    case SQLITE_BLOB: row.push_back(Value::of(Value::Kind::Bytes, std::string((const char *)sqlite3_column_blob(raw, k), (size_t)sqlite3_column_bytes(raw, k)))); break;
                    default: row.push_back(Value::text(std::string((const char *)sqlite3_column_text(raw, k), (size_t)sqlite3_column_bytes(raw, k))));
                    }
                }
                out.rows.push_back(std::move(row));
            }
            if (rc == SQLITE_INTERRUPT) throw DbError(ErrKind::Cancelled, "operation cancelled");
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) throw DbError(ErrKind::Query, sqlite3_errmsg(db_));
            if (ncol == 0) out.affected = sqlite3_changes(db_);
            if (out.truncated) break;
        }
        out.elapsed_ms = ms_since(t0);
        out.message = stmts == 0 ? "Nothing to execute" : out.columns.empty()
            ? std::to_string(out.affected < 0 ? 0 : out.affected) + " row(s) affected"
            : std::to_string(out.rows.size()) + " row(s)" + (out.truncated ? " (truncated)" : "");
        return out;
    }

    void begin() override
    {
        std::lock_guard<std::mutex> g(m_);
        if (in_tx_) throw DbError(ErrKind::Query, "a transaction is already open");
        exec_raw("BEGIN");
        in_tx_ = true;
    }
    void commit() override
    {
        std::lock_guard<std::mutex> g(m_);
        exec_raw("COMMIT");
        in_tx_ = false;
    }
    void rollback() override
    {
        std::lock_guard<std::mutex> g(m_);
        exec_raw("ROLLBACK");
        in_tx_ = false;
    }
    bool in_transaction() const override { return in_tx_; }

private:
    void exec_raw(const char *sql)
    {
        char *err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "error";
            sqlite3_free(err);
            throw DbError(ErrKind::Query, e);
        }
    }

    static std::vector<const ColumnInfo *> result_cols(const TableMeta &m, const PageRequest &req)
    {
        std::vector<const ColumnInfo *> v;
        if (req.columns.empty())
            for (auto &c : m.columns) v.push_back(&c);
        else
            for (auto &n : req.columns) v.push_back(m.column(n));
        return v;
    }

    sqlite3 *db_;
    std::string path_;
    bool ro_;
    bool in_tx_ = false;
    std::mutex m_;
};

} // namespace

std::unique_ptr<Connector> connect_sqlite(const std::string &path, bool read_only)
{
    if (path.empty()) throw DbError(ErrKind::Validation, "SQLite file path is required");
    sqlite3 *db = nullptr;
    int flags = (read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string e = db ? sqlite3_errmsg(db) : "cannot open database";
        if (db) sqlite3_close(db);
        throw DbError(ErrKind::Connection, e);
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    // Touch the schema so a non-database file fails here, not later.
    char *err = nullptr;
    if (sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "not a database";
        sqlite3_free(err);
        sqlite3_close(db);
        throw DbError(ErrKind::Connection, e);
    }
    return std::make_unique<SqliteConn>(db, path, read_only);
}

} // namespace dbp
