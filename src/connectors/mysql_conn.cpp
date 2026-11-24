#include "mysql_conn.h"

#include <winsock2.h>

#include <mysql/mysql.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>

#include "../core/sqlgen.h"

namespace dbp {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t ms_since(Clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

constexpr unsigned kErrInterrupted = 1317; // ER_QUERY_INTERRUPTED (KILL QUERY)

ErrKind kind_for(unsigned no)
{
    if (no == kErrInterrupted) return ErrKind::Cancelled;
    if (no == 2006 || no == 2013 || no == 2055) return ErrKind::Connection; // gone away / lost connection
    return ErrKind::Query;
}

[[noreturn]] void throw_conn(MYSQL *my)
{
    unsigned no = mysql_errno(my);
    throw DbError(kind_for(no), mysql_error(my));
}

std::string lower(std::string s)
{
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// -------------------------------------------------------------- connection helpers
MYSQL *open_raw(const MySqlParams &p)
{
    MYSQL *my = mysql_init(nullptr);
    if (!my) throw DbError(ErrKind::Connection, "cannot initialise the MySQL client library");
    unsigned timeout = 10;
    mysql_options(my, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(my, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    my_bool on = 1, off = 0;
    if (p.tls) {
        mysql_options(my, MYSQL_OPT_SSL_ENFORCE, &on);
        mysql_options(my, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, p.trust_server_cert ? &off : &on);
    } else {
        // Connector/C 3.4+ requires TLS unless told otherwise; the user turned it off for this connection.
        mysql_options(my, MYSQL_OPT_SSL_ENFORCE, &off);
        mysql_options(my, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &off);
    }
    // CLIENT_FOUND_ROWS: UPDATE reports matched rows, so "0 rows" really means the row changed or vanished.
    if (!mysql_real_connect(my, p.host.c_str(), p.user.c_str(), p.password.c_str(), p.database.empty() ? nullptr : p.database.c_str(), p.port,
                            nullptr, CLIENT_MULTI_STATEMENTS | CLIENT_FOUND_ROWS)) {
        std::string e = mysql_error(my);
        mysql_close(my);
        if (p.tls && (e.find("SSL") != std::string::npos || e.find("TLS") != std::string::npos))
            e += " (TLS is required. For a trusted local server you may turn TLS off in the connection settings.)";
        throw DbError(ErrKind::Connection, e);
    }
    return my;
}

void run_simple(MYSQL *my, const char *sql)
{
    if (mysql_query(my, sql)) throw_conn(my);
}

// -------------------------------------------------------------- value conversion
TypeClass class_from_data_type(const std::string &t_in)
{
    std::string t = lower(t_in);
    if (t == "tinyint" || t == "smallint" || t == "mediumint" || t == "int" || t == "integer" || t == "bigint" || t == "year") return TypeClass::Integer;
    if (t == "decimal" || t == "numeric") return TypeClass::Decimal;
    if (t == "float" || t == "double" || t == "real") return TypeClass::Float;
    if (t == "char" || t == "varchar" || t == "enum" || t == "set") return TypeClass::Text;
    if (t == "tinytext" || t == "text" || t == "mediumtext" || t == "longtext") return TypeClass::LongText;
    if (t == "date") return TypeClass::Date;
    if (t == "time") return TypeClass::Time;
    if (t == "datetime" || t == "timestamp") return TypeClass::DateTime;
    if (t == "json") return TypeClass::Json;
    if (t == "uuid") return TypeClass::Uuid;
    if (t == "binary" || t == "varbinary" || t == "tinyblob" || t == "blob" || t == "mediumblob" || t == "longblob" || t == "bit" || t == "geometry"
        || t == "point" || t == "linestring" || t == "polygon" || t == "multipoint" || t == "multilinestring" || t == "multipolygon" || t == "geometrycollection")
        return TypeClass::Binary;
    return TypeClass::Other;
}

TypeClass class_from_field(const MYSQL_FIELD &f)
{
    bool binary = f.charsetnr == 63;
    switch (f.type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR: return TypeClass::Integer;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: return TypeClass::Float;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: return TypeClass::Decimal;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE: return TypeClass::Date;
    case MYSQL_TYPE_TIME: return TypeClass::Time;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP: return TypeClass::DateTime;
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY: return TypeClass::Binary;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB: return binary ? TypeClass::Binary : TypeClass::LongText;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING: return binary ? TypeClass::Binary : TypeClass::Text;
    default: return TypeClass::Other;
    }
}

Value to_value(TypeClass cls, std::string t)
{
    switch (cls) {
    case TypeClass::Integer: {
        int64_t n = 0;
        auto r = std::from_chars(t.data(), t.data() + t.size(), n);
        if (r.ec == std::errc() && r.ptr == t.data() + t.size()) return Value::integer(n);
        return Value::text(std::move(t)); // unsigned values beyond int64 keep their exact digits
    }
    case TypeClass::Float: return Value::real(std::strtod(t.c_str(), nullptr));
    case TypeClass::Bool: return Value::boolean(t != "0");
    case TypeClass::Binary: return Value::of(Value::Kind::Bytes, std::move(t));
    default: return Value::from_text_for(cls, std::move(t));
    }
}

// -------------------------------------------------------------- prepared statements
class PStmt {
public:
    PStmt(MYSQL *my, const std::string &sql)
    {
        st_ = mysql_stmt_init(my);
        if (!st_) throw DbError(ErrKind::Query, "cannot allocate a statement");
        if (mysql_stmt_prepare(st_, sql.c_str(), (unsigned long)sql.size())) {
            unsigned no = mysql_stmt_errno(st_);
            std::string e = mysql_stmt_error(st_);
            mysql_stmt_close(st_);
            throw DbError(kind_for(no), e);
        }
    }
    ~PStmt()
    {
        if (st_) mysql_stmt_close(st_);
    }
    PStmt(const PStmt &) = delete;
    PStmt &operator=(const PStmt &) = delete;

    void bind_params(const std::vector<Value> &params)
    {
        if (params.size() != mysql_stmt_param_count(st_)) throw DbError(ErrKind::Validation, "parameter count mismatch");
        if (params.empty()) return;
        n_p_ = params.size();
        binds_.assign(n_p_, MYSQL_BIND {});
        ints_.assign(n_p_, 0);
        dbls_.assign(n_p_, 0.0);
        strs_.assign(n_p_, std::string());
        nulls_.assign(n_p_, 0);
        for (size_t k = 0; k < n_p_; ++k) {
            const Value &v = params[k];
            MYSQL_BIND &b = binds_[k];
            switch (v.kind) {
            case Value::Kind::Null:
            case Value::Kind::Missing:
            case Value::Kind::Default:
                nulls_[k] = 1;
                b.buffer_type = MYSQL_TYPE_NULL;
                b.is_null = &nulls_[k];
                break;
            case Value::Kind::Bool:
            case Value::Kind::Int:
                ints_[k] = v.i;
                b.buffer_type = MYSQL_TYPE_LONGLONG;
                b.buffer = &ints_[k];
                break;
            case Value::Kind::Float:
                dbls_[k] = v.d;
                b.buffer_type = MYSQL_TYPE_DOUBLE;
                b.buffer = &dbls_[k];
                break;
            case Value::Kind::Bytes:
                strs_[k] = v.s;
                b.buffer_type = MYSQL_TYPE_BLOB;
                b.buffer = strs_[k].data();
                b.buffer_length = (unsigned long)strs_[k].size();
                break;
            default:
                strs_[k] = v.s;
                b.buffer_type = MYSQL_TYPE_STRING;
                b.buffer = strs_[k].data();
                b.buffer_length = (unsigned long)strs_[k].size();
                break;
            }
        }
        if (mysql_stmt_bind_param(st_, binds_.data())) throw_stmt();
    }

    void execute()
    {
        if (mysql_stmt_execute(st_)) throw_stmt();
    }

    uint64_t affected() const { return mysql_stmt_affected_rows(st_); }

    // Result columns are read as text (exact for integers and decimals) except floats, which are read as doubles.
    void bind_result(const std::vector<TypeClass> &classes)
    {
        cls_ = classes;
        size_t n = cls_.size();
        if (mysql_stmt_field_count(st_) != n) throw DbError(ErrKind::Query, "unexpected number of result columns");
        rbinds_.assign(n, MYSQL_BIND {});
        bufs_.assign(n, std::string(256, '\0'));
        rdbl_.assign(n, 0.0);
        lens_.assign(n, 0);
        rnull_.assign(n, 0);
        rerr_.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            MYSQL_BIND &b = rbinds_[i];
            if (cls_[i] == TypeClass::Float) {
                b.buffer_type = MYSQL_TYPE_DOUBLE;
                b.buffer = &rdbl_[i];
                b.buffer_length = sizeof(double);
            } else {
                b.buffer_type = MYSQL_TYPE_STRING;
                b.buffer = bufs_[i].data();
                b.buffer_length = (unsigned long)bufs_[i].size();
            }
            b.length = &lens_[i];
            b.is_null = &rnull_[i];
            b.error = &rerr_[i];
        }
        if (mysql_stmt_bind_result(st_, rbinds_.data())) throw_stmt();
    }

    bool fetch(std::vector<Value> &row)
    {
        int rc = mysql_stmt_fetch(st_);
        if (rc == MYSQL_NO_DATA) return false;
        if (rc == 1) throw_stmt();
        row.clear();
        row.reserve(cls_.size());
        for (size_t i = 0; i < cls_.size(); ++i) {
            if (rnull_[i]) {
                row.push_back(Value::null());
                continue;
            }
            if (cls_[i] == TypeClass::Float) {
                row.push_back(Value::real(rdbl_[i]));
                continue;
            }
            size_t len = lens_[i];
            std::string t;
            if (len <= bufs_[i].size()) {
                t.assign(bufs_[i].data(), len);
            } else {
                t.resize(len);
                MYSQL_BIND fb {};
                unsigned long l2 = 0;
                my_bool er = 0;
                fb.buffer_type = MYSQL_TYPE_STRING;
                fb.buffer = t.data();
                fb.buffer_length = (unsigned long)len;
                fb.length = &l2;
                fb.error = &er;
                if (mysql_stmt_fetch_column(st_, &fb, (unsigned)i, 0)) throw_stmt();
            }
            row.push_back(to_value(cls_[i], std::move(t)));
        }
        return true;
    }

private:
    [[noreturn]] void throw_stmt()
    {
        unsigned no = mysql_stmt_errno(st_);
        throw DbError(kind_for(no), mysql_stmt_error(st_));
    }

    MYSQL_STMT *st_ = nullptr;
    size_t n_p_ = 0;
    std::vector<MYSQL_BIND> binds_, rbinds_;
    std::vector<int64_t> ints_;
    std::vector<double> dbls_, rdbl_;
    std::vector<std::string> strs_, bufs_;
    std::vector<my_bool> nulls_, rnull_, rerr_;
    std::vector<unsigned long> lens_;
    std::vector<TypeClass> cls_;
};

class MySqlConn : public Connector {
public:
    explicit MySqlConn(MySqlParams p) : p_(std::move(p)) {}
    ~MySqlConn() override { close(); }

    void open_first()
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
    }

    Capabilities caps() const override
    {
        Capabilities c;
        c.transactions = true;
        c.edit = !p_.read_only;
        c.regex = true;
        c.schemas = false;
        c.sql = true;
        c.default_keyword = true;
        return c;
    }

    std::string server_info() override
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        return mysql_get_server_info(my_);
    }

    void close() override
    {
        std::lock_guard<std::mutex> g(m_);
        closed_ = true;
        if (my_) {
            mysql_close(my_);
            my_ = nullptr;
        }
    }

    std::vector<ObjectInfo> list_databases() override
    {
        if (!p_.database.empty()) return { { p_.database, ObjectKind::Database } };
        auto rows = query_values("SELECT SCHEMA_NAME FROM information_schema.SCHEMATA", {}, 1);
        std::vector<std::string> user, sys;
        for (auto &r : rows) {
            std::string n = str(r[0]);
            std::string l = lower(n);
            (l == "information_schema" || l == "mysql" || l == "performance_schema" || l == "sys" ? sys : user).push_back(n);
        }
        std::sort(user.begin(), user.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
        std::sort(sys.begin(), sys.end());
        std::vector<ObjectInfo> out;
        for (auto &n : user) out.push_back({ n, ObjectKind::Database });
        for (auto &n : sys) out.push_back({ n, ObjectKind::Database });
        return out;
    }

    std::vector<ObjectInfo> list_schemas(const std::string &) override { return {}; }

    std::vector<ObjectInfo> list_tables(const std::string &db, const std::string &) override
    {
        auto rows = query_values("SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? ORDER BY TABLE_TYPE, TABLE_NAME",
                                 { Value::text(db) }, 2);
        std::vector<ObjectInfo> out;
        for (auto &r : rows) {
            ObjectInfo o;
            o.name = str(r[0]);
            o.kind = str(r[1]).find("VIEW") != std::string::npos ? ObjectKind::View : ObjectKind::Table;
            out.push_back(o);
        }
        return out;
    }

    TableMeta table_meta(const TableRef &t) override
    {
        if (t.database.empty()) throw DbError(ErrKind::Validation, "database name required");
        TableMeta m;
        m.table = t;
        m.table.schema.clear();
        std::vector<Value> key { Value::text(t.database), Value::text(t.name) };

        auto info = query_values("SELECT TABLE_TYPE, TABLE_ROWS FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ?", key, 2);
        if (info.empty()) throw DbError(ErrKind::Query, "table not found: " + t.label());
        m.is_view = str(info[0][0]).find("VIEW") != std::string::npos;
        if (!m.is_view && !info[0][1].is_null()) m.estimated_rows = std::strtoll(str(info[0][1]).c_str(), nullptr, 10);

        auto cols = query_values("SELECT COLUMN_NAME, DATA_TYPE, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, EXTRA, GENERATION_EXPRESSION "
                                 "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? ORDER BY ORDINAL_POSITION",
                                 key, 7);
        for (auto &r : cols) {
            ColumnInfo c;
            c.name = str(r[0]);
            c.native_type = str(r[2]);
            c.cls = class_from_data_type(str(r[1]));
            c.nullable = str(r[3]) == "YES";
            std::string extra = lower(str(r[5]));
            c.identity = extra.find("auto_increment") != std::string::npos;
            // MariaDB reports the text 'NULL' as the default of a nullable column without one.
            c.has_default = !r[4].is_null() && !(c.nullable && str(r[4]) == "NULL");
            c.generated = !r[6].is_null() && !str(r[6]).empty();
            m.columns.push_back(std::move(c));
        }
        if (m.columns.empty()) throw DbError(ErrKind::Query, "table has no visible columns: " + t.label());

        auto pk = query_values("SELECT COLUMN_NAME, ORDINAL_POSITION FROM information_schema.KEY_COLUMN_USAGE "
                               "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? AND CONSTRAINT_NAME = 'PRIMARY'",
                               key, 2);
        std::vector<std::pair<int, std::string>> pks;
        for (auto &r : pk) {
            int pos = (int)std::strtol(str(r[1]).c_str(), nullptr, 10);
            pks.push_back({ pos, str(r[0]) });
            for (auto &c : m.columns)
                if (c.name == str(r[0])) c.pk_position = pos;
        }
        std::sort(pks.begin(), pks.end());

        auto fk = query_values("SELECT COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE "
                               "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? AND REFERENCED_TABLE_NAME IS NOT NULL",
                               key, 3);
        for (auto &r : fk)
            for (auto &c : m.columns)
                if (c.name == str(r[0])) c.foreign_key = str(r[1]) + "." + str(r[2]);

        auto ix = query_values("SELECT INDEX_NAME, NON_UNIQUE, COLUMN_NAME FROM information_schema.STATISTICS "
                               "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? ORDER BY INDEX_NAME, SEQ_IN_INDEX",
                               key, 3);
        for (auto &r : ix) {
            std::string name = str(r[0]);
            if (m.indexes.empty() || m.indexes.back().name != name) {
                IndexInfo i;
                i.name = name;
                i.unique = str(r[1]) == "0";
                m.indexes.push_back(i);
            }
            m.indexes.back().columns.push_back(str(r[2]));
        }

        if (m.is_view) m.edit = { {}, false, "Views are read-only" };
        else if (p_.read_only) m.edit = { {}, false, "Connection is read-only" };
        else if (pks.empty()) m.edit = { {}, false, "No primary key: rows cannot be identified unambiguously" };
        else {
            for (auto &p : pks) m.edit.key_columns.push_back(p.second);
            m.edit.editable = true;
        }
        return m;
    }

    Page fetch_page(const TableMeta &m, const PageRequest &req, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        Built b = build_select(Dialect::MySql, m, req);
        Page p;
        p.generated = inline_params(Dialect::MySql, b.sql, b.params);
        auto cls = result_classes(m, req);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        HookGuard hg(c, killer());
        PStmt s(my_, b.sql);
        s.bind_params(b.params);
        s.execute();
        s.bind_result(cls);
        std::vector<Value> row;
        while (s.fetch(row)) {
            if (p.rows.size() >= req.limit) {
                p.has_more = true;
                continue; // the extra row only proves there is a next page; keep reading so the connection stays clean
            }
            p.rows.push_back(row);
        }
        p.elapsed_ms = ms_since(t0);
        return p;
    }

    uint64_t count(const TableMeta &m, const FilterGroup &f, const CancelPtr &c) override
    {
        Built b = build_count(Dialect::MySql, m, f);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        HookGuard hg(c, killer());
        PStmt s(my_, b.sql);
        s.bind_params(b.params);
        s.execute();
        s.bind_result({ TypeClass::Text });
        std::vector<Value> row;
        if (!s.fetch(row) || row[0].is_null()) return 0;
        uint64_t n = std::strtoull(row[0].s.c_str(), nullptr, 10);
        while (s.fetch(row)) {}
        return n;
    }

    std::vector<Value> distinct_values(const TableMeta &m, const DistinctRequest &r, const CancelPtr &c) override
    {
        Built b = build_distinct(Dialect::MySql, m, r);
        const ColumnInfo *ci = m.column(r.column);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        HookGuard hg(c, killer());
        PStmt s(my_, b.sql);
        s.bind_params(b.params);
        s.execute();
        s.bind_result({ ci->cls });
        std::vector<Value> out, row;
        while (s.fetch(row)) out.push_back(row[0]);
        return out;
    }

    uint64_t stream_rows(const TableMeta &m, const PageRequest &req, size_t batch, const RowBatchFn &fn, const CancelPtr &c) override
    {
        PageRequest r = req;
        r.limit = 0x7fffffff;
        r.offset = 0;
        Built b = build_select(Dialect::MySql, m, r, false);
        auto cls = result_classes(m, req);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        HookGuard hg(c, killer());
        PStmt s(my_, b.sql);
        Abandon ab { this }; // destroyed first: an unfinished cursor must not be drained row by row
        s.bind_params(b.params);
        s.execute();
        s.bind_result(cls);
        std::vector<std::vector<Value>> buf;
        std::vector<Value> row;
        uint64_t total = 0;
        while (s.fetch(row)) {
            buf.push_back(std::move(row));
            row = {};
            ++total;
            if (buf.size() >= batch) {
                if (!fn(buf)) return total;
                buf.clear();
            }
        }
        ab.armed = false;
        if (!buf.empty()) fn(buf);
        return total;
    }

    ApplyReport apply_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        if (p_.read_only) throw DbError(ErrKind::ReadOnly, "Connection is read-only");
        if (!m.edit.editable) throw DbError(ErrKind::Validation, m.edit.blocked_reason);
        auto stmts = build_changes(Dialect::MySql, m, changes);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        bool own_tx = !in_tx_;
        if (own_tx) run_simple(my_, "START TRANSACTION");
        ApplyReport rep;
        try {
            for (auto &st : stmts) {
                PStmt s(my_, st.q.sql);
                s.bind_params(st.q.params);
                s.execute();
                uint64_t n = s.affected();
                if (st.must_affect_one && n != 1)
                    throw DbError(ErrKind::Conflict, st.type == Change::Type::Delete
                                                          ? "A row to delete no longer exists or was changed by someone else"
                                                          : "A row was modified or deleted by someone else since it was loaded");
                if (st.type == Change::Type::Insert) rep.inserted += (int)n;
                else if (st.type == Change::Type::Update) rep.updated += (int)n;
                else rep.deleted += (int)n;
            }
            if (own_tx) run_simple(my_, "COMMIT");
        } catch (...) {
            if (own_tx && my_) mysql_query(my_, "ROLLBACK");
            throw;
        }
        return rep;
    }

    std::string preview_query(const TableMeta &m, const PageRequest &req) override
    {
        Built b = build_select(Dialect::MySql, m, req);
        return inline_params(Dialect::MySql, b.sql, b.params);
    }

    std::string preview_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        std::string out;
        for (auto &st : build_changes(Dialect::MySql, m, changes)) out += inline_params(Dialect::MySql, st.q.sql, st.q.params) + ";\n";
        return out;
    }

    QueryOutput execute(const std::string &text, size_t max_rows, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        std::lock_guard<std::mutex> g(m_);
        ensure();
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        HookGuard hg(c, killer());
        QueryOutput out;
        if (mysql_real_query(my_, text.c_str(), (unsigned long)text.size())) throw_conn(my_);
        int sets = 0;
        for (;;) {
            ++sets;
            MYSQL_RES *res = mysql_use_result(my_);
            if (res) {
                out = QueryOutput(); // keep only the last result set
                unsigned n = mysql_num_fields(res);
                MYSQL_FIELD *fields = mysql_fetch_fields(res);
                std::vector<TypeClass> cls;
                for (unsigned k = 0; k < n; ++k) {
                    out.columns.push_back(fields[k].name);
                    cls.push_back(class_from_field(fields[k]));
                }
                MYSQL_ROW r;
                while ((r = mysql_fetch_row(res))) {
                    if (out.rows.size() >= max_rows) {
                        out.truncated = true;
                        break;
                    }
                    unsigned long *len = mysql_fetch_lengths(res);
                    std::vector<Value> row;
                    row.reserve(n);
                    for (unsigned k = 0; k < n; ++k) row.push_back(r[k] ? to_value(cls[k], std::string(r[k], len[k])) : Value::null());
                    out.rows.push_back(std::move(row));
                }
                unsigned no = mysql_errno(my_);
                if (out.truncated) {
                    kill_now(); // stop the server from sending the remaining rows
                    broken_ = true;
                }
                mysql_free_result(res);
                if (!out.truncated && no) throw_conn(my_);
                if (out.truncated) break;
            } else if (mysql_field_count(my_) == 0) {
                out = QueryOutput();
                out.affected = (int64_t)mysql_affected_rows(my_);
            } else {
                throw_conn(my_);
            }
            int next = mysql_next_result(my_);
            if (next == 0) continue;
            if (next > 0) throw_conn(my_);
            break;
        }
        out.elapsed_ms = ms_since(t0);
        out.message = out.columns.empty() ? std::to_string(out.affected < 0 ? 0 : out.affected) + " row(s) affected"
                                           : std::to_string(out.rows.size()) + " row(s)" + (out.truncated ? " (truncated)" : "");
        return out;
    }

    void begin() override
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        if (in_tx_) throw DbError(ErrKind::Query, "a transaction is already open");
        run_simple(my_, "START TRANSACTION");
        in_tx_ = true;
    }
    void commit() override
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        run_simple(my_, "COMMIT");
        in_tx_ = false;
    }
    void rollback() override
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        run_simple(my_, "ROLLBACK");
        in_tx_ = false;
    }
    bool in_transaction() const override { return in_tx_; }

private:
    struct Abandon {
        MySqlConn *c;
        bool armed = true;
        ~Abandon()
        {
            if (armed) {
                c->kill_now();
                c->broken_ = true;
            }
        }
    };

    // Called with m_ held. Opens the connection, or reopens it after an aborted cursor.
    void ensure()
    {
        if (closed_) throw DbError(ErrKind::Connection, "connection closed");
        if (my_ && !broken_) return;
        if (my_) {
            mysql_close(my_);
            my_ = nullptr;
        }
        broken_ = false;
        in_tx_ = false;
        MYSQL *my = open_raw(p_);
        try {
            // Strict mode: a value that does not fit is an error, never a silent truncation.
            run_simple(my, "SET SESSION sql_mode = IF(@@SESSION.sql_mode = '', 'STRICT_ALL_TABLES', CONCAT(@@SESSION.sql_mode, ',STRICT_ALL_TABLES'))");
            if (p_.read_only) run_simple(my, "SET SESSION TRANSACTION READ ONLY");
        } catch (...) {
            mysql_close(my);
            throw;
        }
        my_ = my;
        tid_ = mysql_thread_id(my_);
    }

    // KILL QUERY from a second connection: this is how Cancel interrupts a running statement.
    static void kill_with(const MySqlParams &p, unsigned long tid)
    {
        try {
            MYSQL *k = open_raw(p);
            std::string q = "KILL QUERY " + std::to_string(tid);
            mysql_query(k, q.c_str());
            mysql_close(k);
        } catch (...) {
        }
    }
    void kill_now() { kill_with(p_, tid_); }
    std::function<void()> killer() { return [p = p_, tid = tid_] { kill_with(p, tid); }; }

    static std::string str(const Value &v) { return v.is_null() ? std::string() : v.s; }

    // Small metadata queries: every column comes back as text.
    std::vector<std::vector<Value>> query_values(const std::string &sql, const std::vector<Value> &params, size_t ncols)
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        PStmt s(my_, sql);
        s.bind_params(params);
        s.execute();
        s.bind_result(std::vector<TypeClass>(ncols, TypeClass::Text));
        std::vector<std::vector<Value>> out;
        std::vector<Value> row;
        while (s.fetch(row)) out.push_back(row);
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

    MySqlParams p_;
    MYSQL *my_ = nullptr;
    unsigned long tid_ = 0;
    bool broken_ = false, closed_ = false, in_tx_ = false;
    std::mutex m_;
};

} // namespace

std::unique_ptr<Connector> connect_mysql(const MySqlParams &p)
{
    if (p.host.empty()) throw DbError(ErrKind::Validation, "host is required");
    auto c = std::make_unique<MySqlConn>(p);
    c->open_first(); // fail here, with the server's message, not on first use
    return c;
}

} // namespace dbp
