#include "mssql_conn.h"

#include <windows.h>

#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <optional>

#include "../core/sqlgen.h"
#include "../core/sqlsafety.h"
#include "../platform.h"

namespace dbp {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t ms_since(Clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

constexpr SQLSMALLINT kSsXml = -152, kSsTime2 = -154, kSsTzOffset = -155, kSsUdt = -151;

std::string lower(std::string s)
{
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

SQLWCHAR *wp(std::wstring &s) { return reinterpret_cast<SQLWCHAR *>(s.data()); }
std::string from_wide(const SQLWCHAR *p, size_t n) { return platform::narrow(std::wstring(reinterpret_cast<const wchar_t *>(p), n)); }

// "[Microsoft][ODBC Driver 18 for SQL Server][SQL Server]Real message" -> "Real message"
std::string strip_prefix(std::string m)
{
    while (!m.empty() && m[0] == '[') {
        size_t e = m.find(']');
        if (e == std::string::npos) break;
        m.erase(0, e + 1);
    }
    while (!m.empty() && m[0] == ' ') m.erase(0, 1);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
    return m;
}

struct Diag {
    std::string state, msg;
};

Diag read_diag(SQLSMALLINT type, SQLHANDLE h)
{
    Diag d;
    for (SQLSMALLINT i = 1; i <= 4; ++i) {
        SQLWCHAR st[8] = {}, msg[1024] = {};
        SQLINTEGER native = 0;
        SQLSMALLINT len = 0;
        SQLRETURN r = SQLGetDiagRecW(type, h, i, st, &native, msg, 1023, &len);
        if (!SQL_SUCCEEDED(r)) break;
        std::string m = strip_prefix(from_wide(msg, (size_t)std::min<SQLSMALLINT>(len, 1023)));
        if (i == 1) {
            d.state = from_wide(st, 5);
            d.msg = m;
        } else if (!m.empty() && d.msg.find(m) == std::string::npos) {
            d.msg += " | " + m;
        }
    }
    return d;
}

ErrKind kind_for_state(const std::string &s)
{
    if (s == "HY008") return ErrKind::Cancelled;
    if (s == "HYT00" || s == "HYT01") return ErrKind::Timeout;
    if (s.rfind("08", 0) == 0 || s == "IM002" || s == "28000" || s == "HY000x") return ErrKind::Connection;
    return ErrKind::Query;
}

[[noreturn]] void throw_diag(SQLSMALLINT type, SQLHANDLE h, bool connecting = false)
{
    Diag d = read_diag(type, h);
    ErrKind k = connecting ? ErrKind::Connection : kind_for_state(d.state);
    std::string msg = d.msg;
    if (d.state == "IM002")
        msg = "\"ODBC Driver 18 for SQL Server\" is not installed. Install it from Microsoft (search for \"Download ODBC Driver for SQL Server\") and try again.";
    throw DbError(k, msg.empty() ? "unknown SQL Server error" : msg);
}

#define ODBC_CHECK(call, type, handle) \
    do { \
        SQLRETURN _r = (call); \
        if (!SQL_SUCCEEDED(_r)) throw_diag(type, handle); \
    } while (0)

// -------------------------------------------------------------- type mapping
TypeClass class_from_sql_type(SQLSMALLINT t)
{
    switch (t) {
    case SQL_TINYINT: case SQL_SMALLINT: case SQL_INTEGER: case SQL_BIGINT: return TypeClass::Integer;
    case SQL_DECIMAL: case SQL_NUMERIC: return TypeClass::Decimal;
    case SQL_FLOAT: case SQL_REAL: case SQL_DOUBLE: return TypeClass::Float;
    case SQL_BIT: return TypeClass::Bool;
    case SQL_CHAR: case SQL_VARCHAR: case SQL_WCHAR: case SQL_WVARCHAR: return TypeClass::Text;
    case SQL_LONGVARCHAR: case SQL_WLONGVARCHAR: return TypeClass::LongText;
    case SQL_TYPE_DATE: return TypeClass::Date;
    case SQL_TYPE_TIME: case kSsTime2: return TypeClass::Time;
    case SQL_TYPE_TIMESTAMP: case kSsTzOffset: return TypeClass::DateTime;
    case SQL_GUID: return TypeClass::Uuid;
    case SQL_BINARY: case SQL_VARBINARY: case SQL_LONGVARBINARY: case kSsUdt: return TypeClass::Binary;
    case kSsXml: return TypeClass::Json;
    default: return TypeClass::Other;
    }
}

// From sys.types names (table metadata).
TypeClass class_from_type_name(const std::string &n_in, int max_length)
{
    std::string n = lower(n_in);
    if (n == "tinyint" || n == "smallint" || n == "int" || n == "bigint") return TypeClass::Integer;
    if (n == "decimal" || n == "numeric" || n == "money" || n == "smallmoney") return TypeClass::Decimal;
    if (n == "float" || n == "real") return TypeClass::Float;
    if (n == "bit") return TypeClass::Bool;
    if (n == "char" || n == "varchar" || n == "nchar" || n == "nvarchar" || n == "sysname") return max_length == -1 ? TypeClass::LongText : TypeClass::Text;
    // text / ntext cannot be compared with "=" and have no usable LIKE ESCAPE semantics for every collation: they are handled
    // as XML-like "Json" columns, which are filtered through CAST(... AS NVARCHAR(MAX)) and skipped by the concurrency check.
    if (n == "text" || n == "ntext" || n == "xml") return TypeClass::Json;
    if (n == "date") return TypeClass::Date;
    if (n == "time") return TypeClass::Time;
    if (n == "datetime" || n == "datetime2" || n == "smalldatetime" || n == "datetimeoffset") return TypeClass::DateTime;
    if (n == "uniqueidentifier") return TypeClass::Uuid;
    if (n == "binary" || n == "varbinary" || n == "image" || n == "timestamp" || n == "rowversion" || n == "geography" || n == "geometry" || n == "hierarchyid")
        return TypeClass::Binary;
    return TypeClass::Other;
}

std::string native_type_text(const std::string &name, int max_length, int precision, int scale)
{
    std::string n = lower(name);
    if (n == "varchar" || n == "char" || n == "varbinary" || n == "binary") return name + "(" + (max_length == -1 ? "max" : std::to_string(max_length)) + ")";
    if (n == "nvarchar" || n == "nchar") return name + "(" + (max_length == -1 ? "max" : std::to_string(max_length / 2)) + ")";
    if (n == "decimal" || n == "numeric") return name + "(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
    if (n == "datetime2" || n == "datetimeoffset" || n == "time") return name + "(" + std::to_string(scale) + ")";
    return name;
}

// -------------------------------------------------------------- reading column data
struct Cell {
    bool null = true;
    std::string data;
};

Cell get_text(SQLHSTMT h, SQLUSMALLINT col)
{
    Cell c;
    std::wstring w;
    SQLWCHAR buf[2048];
    for (;;) {
        SQLLEN ind = 0;
        SQLRETURN r = SQLGetData(h, col, SQL_C_WCHAR, buf, sizeof buf, &ind);
        if (r == SQL_NO_DATA) break;
        if (!SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_STMT, h);
        if (ind == SQL_NULL_DATA) return c;
        c.null = false;
        size_t bytes = (ind == SQL_NO_TOTAL || (size_t)ind >= sizeof buf) ? sizeof buf - sizeof(SQLWCHAR) : (size_t)ind;
        w.append(reinterpret_cast<const wchar_t *>(buf), bytes / sizeof(SQLWCHAR));
        if (r == SQL_SUCCESS) break;
    }
    c.data = platform::narrow(w);
    return c;
}

Cell get_bytes(SQLHSTMT h, SQLUSMALLINT col)
{
    Cell c;
    char buf[4096];
    for (;;) {
        SQLLEN ind = 0;
        SQLRETURN r = SQLGetData(h, col, SQL_C_BINARY, buf, sizeof buf, &ind);
        if (r == SQL_NO_DATA) break;
        if (!SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_STMT, h);
        if (ind == SQL_NULL_DATA) return c;
        c.null = false;
        size_t n = (ind == SQL_NO_TOTAL || (size_t)ind > sizeof buf) ? sizeof buf : (size_t)ind;
        c.data.append(buf, n);
        if (r == SQL_SUCCESS) break;
    }
    return c;
}

std::vector<Value> read_row(SQLHSTMT h, const std::vector<TypeClass> &cls)
{
    std::vector<Value> row;
    row.reserve(cls.size());
    for (size_t k = 0; k < cls.size(); ++k) {
        SQLUSMALLINT col = (SQLUSMALLINT)(k + 1);
        switch (cls[k]) {
        case TypeClass::Float: {
            double d = 0;
            SQLLEN ind = 0;
            ODBC_CHECK(SQLGetData(h, col, SQL_C_DOUBLE, &d, 0, &ind), SQL_HANDLE_STMT, h);
            row.push_back(ind == SQL_NULL_DATA ? Value::null() : Value::real(d));
            break;
        }
        case TypeClass::Binary: {
            Cell c = get_bytes(h, col);
            row.push_back(c.null ? Value::null() : Value::of(Value::Kind::Bytes, std::move(c.data)));
            break;
        }
        default: {
            Cell c = get_text(h, col);
            if (c.null) {
                row.push_back(Value::null());
            } else if (cls[k] == TypeClass::Integer) {
                int64_t n = 0;
                auto r = std::from_chars(c.data.data(), c.data.data() + c.data.size(), n);
                row.push_back(r.ec == std::errc() && r.ptr == c.data.data() + c.data.size() ? Value::integer(n) : Value::text(std::move(c.data)));
            } else if (cls[k] == TypeClass::Bool) {
                row.push_back(Value::boolean(c.data != "0"));
            } else {
                row.push_back(Value::from_text_for(cls[k], std::move(c.data)));
            }
            break;
        }
        }
    }
    return row;
}

// -------------------------------------------------------------- statement
struct Hold {
    std::mutex m;
    SQLHSTMT h = nullptr;
};

class Stmt {
public:
    explicit Stmt(SQLHDBC dbc) : hold_(std::make_shared<Hold>())
    {
        SQLRETURN r = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h_);
        if (!SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_DBC, dbc);
        hold_->h = h_;
    }
    ~Stmt()
    {
        std::lock_guard<std::mutex> g(hold_->m);
        hold_->h = nullptr;
        SQLFreeHandle(SQL_HANDLE_STMT, h_);
    }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    SQLHSTMT h() const { return h_; }

    // SQLCancel may be called from another thread while this statement executes.
    std::function<void()> canceller()
    {
        auto hd = hold_;
        return [hd] {
            std::lock_guard<std::mutex> g(hd->m);
            if (hd->h) SQLCancel(hd->h);
        };
    }
    void cancel() { canceller()(); }

    void run(const std::string &sql, const std::vector<Value> &params)
    {
        bind(params);
        std::wstring w = platform::widen(sql);
        SQLRETURN r = SQLExecDirectW(h_, wp(w), SQL_NTS);
        if (r != SQL_NO_DATA && !SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_STMT, h_);
    }

    int64_t affected()
    {
        SQLLEN n = -1;
        SQLRowCount(h_, &n);
        return (int64_t)n;
    }

    bool fetch()
    {
        SQLRETURN r = SQLFetch(h_);
        if (r == SQL_NO_DATA) return false;
        if (!SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_STMT, h_);
        return true;
    }

    struct Col {
        std::string name;
        TypeClass cls;
    };
    std::vector<Col> describe()
    {
        SQLSMALLINT n = 0;
        ODBC_CHECK(SQLNumResultCols(h_, &n), SQL_HANDLE_STMT, h_);
        std::vector<Col> out;
        for (SQLSMALLINT k = 1; k <= n; ++k) {
            SQLWCHAR name[256] = {};
            SQLSMALLINT nl = 0, dt = 0, dd = 0, nullable = 0;
            SQLULEN size = 0;
            ODBC_CHECK(SQLDescribeColW(h_, (SQLUSMALLINT)k, name, 255, &nl, &dt, &size, &dd, &nullable), SQL_HANDLE_STMT, h_);
            out.push_back({ from_wide(name, (size_t)std::min<SQLSMALLINT>(nl, 255)), class_from_sql_type(dt) });
        }
        return out;
    }

    int columns()
    {
        SQLSMALLINT n = 0;
        SQLNumResultCols(h_, &n);
        return n;
    }

private:
    struct PBuf {
        SQLLEN ind = 0;
        std::wstring w;
        std::string b;
        int64_t i = 0;
        double d = 0;
        unsigned char bit = 0;
        char dummy = 0;
    };

    void bind(const std::vector<Value> &params)
    {
        bufs_.assign(params.size(), PBuf());
        for (size_t k = 0; k < params.size(); ++k) {
            const Value &v = params[k];
            PBuf &p = bufs_[k];
            SQLUSMALLINT n = (SQLUSMALLINT)(k + 1);
            SQLRETURN r;
            switch (v.kind) {
            case Value::Kind::Null:
            case Value::Kind::Missing:
            case Value::Kind::Default:
                p.ind = SQL_NULL_DATA;
                r = SQLBindParameter(h_, n, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 1, 0, &p.dummy, 1, &p.ind);
                break;
            case Value::Kind::Bool:
                p.bit = v.i ? 1 : 0;
                r = SQLBindParameter(h_, n, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 1, 0, &p.bit, 0, &p.ind);
                break;
            case Value::Kind::Int:
                p.i = v.i;
                r = SQLBindParameter(h_, n, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &p.i, 0, &p.ind);
                break;
            case Value::Kind::Float:
                p.d = v.d;
                r = SQLBindParameter(h_, n, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 53, 0, &p.d, 0, &p.ind);
                break;
            case Value::Kind::Bytes:
                p.b = v.s;
                p.ind = (SQLLEN)p.b.size();
                r = SQLBindParameter(h_, n, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY, p.b.size() <= 8000 ? std::max<size_t>(p.b.size(), 1) : 0, 0,
                                     p.b.data(), (SQLLEN)p.b.size(), &p.ind);
                break;
            default:
                p.w = platform::widen(v.s);
                p.ind = (SQLLEN)(p.w.size() * sizeof(wchar_t));
                r = SQLBindParameter(h_, n, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, p.w.size() <= 4000 ? std::max<size_t>(p.w.size(), 1) : 0, 0,
                                     p.w.data(), p.ind, &p.ind);
                break;
            }
            if (!SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_STMT, h_);
        }
    }

    SQLHSTMT h_ = nullptr;
    std::shared_ptr<Hold> hold_;
    std::vector<PBuf> bufs_;
};

std::string braced(const std::string &v)
{
    std::string r = "{";
    for (char c : v) {
        if (c == '}') r += '}';
        r += c;
    }
    return r + "}";
}

class MsConn : public Connector {
public:
    explicit MsConn(MsSqlParams p) : p_(std::move(p)) {}
    ~MsConn() override { close(); }

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
        c.regex = false; // T-SQL has no regular-expression operator before SQL Server 2025
        c.schemas = true;
        c.sql = true;
        c.default_keyword = true;
        return c;
    }

    std::string server_info() override
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        SQLWCHAR name[128] = {}, ver[128] = {};
        SQLSMALLINT l1 = 0, l2 = 0;
        SQLGetInfoW(dbc_, SQL_DBMS_NAME, name, sizeof name, &l1);
        SQLGetInfoW(dbc_, SQL_DBMS_VER, ver, sizeof ver, &l2);
        return from_wide(name, wcslen(reinterpret_cast<wchar_t *>(name))) + " " + from_wide(ver, wcslen(reinterpret_cast<wchar_t *>(ver)));
    }

    void close() override
    {
        std::lock_guard<std::mutex> g(m_);
        closed_ = true;
        drop();
    }

    std::vector<ObjectInfo> list_databases() override
    {
        if (!p_.database.empty()) return { { p_.database, ObjectKind::Database } };
        auto rows = query_values("SELECT name FROM sys.databases WHERE state = 0 AND HAS_DBACCESS(name) = 1 ORDER BY (CASE WHEN database_id <= 4 THEN 1 ELSE 0 END), name", {}, 1);
        std::vector<ObjectInfo> out;
        for (auto &r : rows) out.push_back({ str(r[0]), ObjectKind::Database });
        return out;
    }

    std::vector<ObjectInfo> list_schemas(const std::string &db) override
    {
        auto rows = query_values("SELECT name FROM " + pre(db) + "sys.schemas WHERE schema_id NOT BETWEEN 16384 AND 16399 "
                                 "AND name NOT IN ('guest', 'INFORMATION_SCHEMA', 'sys') ORDER BY name",
                                 {}, 1);
        std::vector<ObjectInfo> out;
        for (auto &r : rows) out.push_back({ str(r[0]), ObjectKind::Schema });
        return out;
    }

    std::vector<ObjectInfo> list_tables(const std::string &db, const std::string &schema) override
    {
        auto rows = query_values("SELECT o.name, o.type FROM " + pre(db) + "sys.objects o JOIN " + pre(db) + "sys.schemas s ON s.schema_id = o.schema_id "
                                 "WHERE s.name = ? AND o.type IN ('U', 'V') AND o.is_ms_shipped = 0 ORDER BY o.type, o.name",
                                 { Value::text(schema.empty() ? "dbo" : schema) }, 2);
        std::vector<ObjectInfo> out;
        for (auto &r : rows) out.push_back({ str(r[0]), str(r[1]).rfind("V", 0) == 0 ? ObjectKind::View : ObjectKind::Table });
        return out;
    }

    TableMeta table_meta(const TableRef &t) override
    {
        TableMeta m;
        m.table = t;
        if (m.table.schema.empty()) m.table.schema = "dbo";
        const std::string P = pre(t.database);
        std::vector<Value> key { Value::text(m.table.schema), Value::text(m.table.name) };
        const std::string from_obj = " FROM " + P + "sys.objects o JOIN " + P + "sys.schemas s ON s.schema_id = o.schema_id ";

        auto info = query_values("SELECT o.type, (SELECT SUM(p.rows) FROM " + P + "sys.partitions p WHERE p.object_id = o.object_id AND p.index_id IN (0, 1))" + from_obj +
                                     "WHERE s.name = ? AND o.name = ? AND o.type IN ('U', 'V')",
                                 key, 2);
        if (info.empty()) throw DbError(ErrKind::Query, "table not found: " + t.label());
        m.is_view = str(info[0][0]).rfind("V", 0) == 0;
        if (!m.is_view && !info[0][1].is_null()) m.estimated_rows = std::strtoll(str(info[0][1]).c_str(), nullptr, 10);

        auto cols = query_values("SELECT c.name, CASE WHEN ty.is_user_defined = 1 THEN bt.name ELSE ty.name END, c.max_length, c.precision, c.scale, c.is_nullable, c.is_identity, "
                                 "c.is_computed, CASE WHEN c.default_object_id <> 0 THEN 1 ELSE 0 END" + from_obj +
                                     "JOIN " + P + "sys.columns c ON c.object_id = o.object_id JOIN " + P + "sys.types ty ON ty.user_type_id = c.user_type_id "
                                     "LEFT JOIN " + P + "sys.types bt ON bt.user_type_id = c.system_type_id AND ty.is_user_defined = 1 "
                                     "WHERE s.name = ? AND o.name = ? ORDER BY c.column_id",
                                 key, 9);
        for (auto &r : cols) {
            ColumnInfo c;
            c.name = str(r[0]);
            std::string tn = str(r[1]);
            int ml = (int)std::strtol(str(r[2]).c_str(), nullptr, 10);
            int prec = (int)std::strtol(str(r[3]).c_str(), nullptr, 10);
            int scale = (int)std::strtol(str(r[4]).c_str(), nullptr, 10);
            c.native_type = native_type_text(tn, ml, prec, scale);
            c.cls = class_from_type_name(tn, ml);
            c.nullable = str(r[5]) == "1";
            c.identity = str(r[6]) == "1";
            c.generated = str(r[7]) == "1" || lower(tn) == "timestamp" || lower(tn) == "rowversion";
            c.has_default = str(r[8]) == "1";
            m.columns.push_back(std::move(c));
        }
        if (m.columns.empty()) throw DbError(ErrKind::Query, "table has no visible columns: " + t.label());

        auto pk = query_values("SELECT c.name, ic.key_ordinal FROM " + P + "sys.indexes i JOIN " + P + "sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
                               "JOIN " + P + "sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id JOIN " + P + "sys.objects o ON o.object_id = i.object_id "
                               "JOIN " + P + "sys.schemas s ON s.schema_id = o.schema_id WHERE i.is_primary_key = 1 AND s.name = ? AND o.name = ? ORDER BY ic.key_ordinal",
                               key, 2);
        std::vector<std::string> pks;
        for (auto &r : pk) {
            int pos = (int)std::strtol(str(r[1]).c_str(), nullptr, 10);
            pks.push_back(str(r[0]));
            for (auto &c : m.columns)
                if (c.name == str(r[0])) c.pk_position = pos;
        }

        auto fk = query_values("SELECT pc.name, rt.name, rc.name FROM " + P + "sys.foreign_key_columns fkc JOIN " + P + "sys.objects o ON o.object_id = fkc.parent_object_id "
                               "JOIN " + P + "sys.schemas s ON s.schema_id = o.schema_id JOIN " + P + "sys.columns pc ON pc.object_id = fkc.parent_object_id AND pc.column_id = fkc.parent_column_id "
                               "JOIN " + P + "sys.objects rt ON rt.object_id = fkc.referenced_object_id JOIN " + P + "sys.columns rc ON rc.object_id = fkc.referenced_object_id AND rc.column_id = fkc.referenced_column_id "
                               "WHERE s.name = ? AND o.name = ?",
                               key, 3);
        for (auto &r : fk)
            for (auto &c : m.columns)
                if (c.name == str(r[0])) c.foreign_key = str(r[1]) + "." + str(r[2]);

        auto ix = query_values("SELECT i.name, i.is_unique, c.name FROM " + P + "sys.indexes i JOIN " + P + "sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
                               "JOIN " + P + "sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id JOIN " + P + "sys.objects o ON o.object_id = i.object_id "
                               "JOIN " + P + "sys.schemas s ON s.schema_id = o.schema_id WHERE i.name IS NOT NULL AND ic.key_ordinal > 0 AND s.name = ? AND o.name = ? ORDER BY i.name, ic.key_ordinal",
                               key, 3);
        for (auto &r : ix) {
            std::string name = str(r[0]);
            if (m.indexes.empty() || m.indexes.back().name != name) {
                IndexInfo i;
                i.name = name;
                i.unique = str(r[1]) == "1";
                m.indexes.push_back(i);
            }
            m.indexes.back().columns.push_back(str(r[2]));
        }

        if (m.is_view) m.edit = { {}, false, "Views are read-only" };
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
        Built b = build_select(Dialect::MsSql, m, req);
        Page p;
        p.generated = inline_params(Dialect::MsSql, b.sql, b.params);
        auto cls = result_classes(m, req);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        Stmt s(dbc_);
        HookGuard hg(c, s.canceller());
        guarded([&] {
            s.run(b.sql, b.params);
            if ((size_t)s.columns() != cls.size()) throw DbError(ErrKind::Query, "unexpected number of result columns");
            while (s.fetch()) {
                if (p.rows.size() >= req.limit) {
                    p.has_more = true;
                    s.cancel();
                    break;
                }
                p.rows.push_back(read_row(s.h(), cls));
            }
        });
        p.elapsed_ms = ms_since(t0);
        return p;
    }

    uint64_t count(const TableMeta &m, const FilterGroup &f, const CancelPtr &c) override
    {
        Built b = build_count(Dialect::MsSql, m, f);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        Stmt s(dbc_);
        HookGuard hg(c, s.canceller());
        uint64_t n = 0;
        guarded([&] {
            s.run(b.sql, b.params);
            if (s.fetch()) {
                Cell cell = get_text(s.h(), 1);
                if (!cell.null) n = std::strtoull(cell.data.c_str(), nullptr, 10);
            }
        });
        return n;
    }

    std::vector<Value> distinct_values(const TableMeta &m, const DistinctRequest &r, const CancelPtr &c) override
    {
        Built b = build_distinct(Dialect::MsSql, m, r);
        const ColumnInfo *ci = m.column(r.column);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        Stmt s(dbc_);
        HookGuard hg(c, s.canceller());
        std::vector<Value> out;
        guarded([&] {
            s.run(b.sql, b.params);
            while (s.fetch()) out.push_back(read_row(s.h(), { ci->cls })[0]);
        });
        return out;
    }

    uint64_t stream_rows(const TableMeta &m, const PageRequest &req, size_t batch, const RowBatchFn &fn, const CancelPtr &c) override
    {
        PageRequest r = req;
        r.limit = 0x7fffffff;
        r.offset = 0;
        Built b = build_select(Dialect::MsSql, m, r, false);
        auto cls = result_classes(m, req);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        Stmt s(dbc_);
        HookGuard hg(c, s.canceller());
        std::vector<std::vector<Value>> buf;
        uint64_t total = 0;
        guarded([&] {
            s.run(b.sql, b.params);
            while (s.fetch()) {
                buf.push_back(read_row(s.h(), cls));
                ++total;
                if (buf.size() >= batch) {
                    bool more = fn(buf);
                    buf.clear();
                    if (!more) {
                        s.cancel(); // stop the server from sending the remaining rows
                        return;
                    }
                }
            }
            if (!buf.empty()) fn(buf);
        });
        return total;
    }

    ApplyReport apply_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        if (p_.read_only) throw DbError(ErrKind::ReadOnly, "Connection is read-only");
        if (!m.edit.editable) throw DbError(ErrKind::Validation, m.edit.blocked_reason);
        auto stmts = build_changes(Dialect::MsSql, m, changes);
        std::lock_guard<std::mutex> g(m_);
        ensure();
        bool own_tx = !in_tx_;
        if (own_tx) set_autocommit(false);
        ApplyReport rep;
        try {
            for (auto &st : stmts) {
                Stmt s(dbc_);
                s.run(st.q.sql, st.q.params);
                int64_t n = std::max<int64_t>(s.affected(), 0);
                if (st.must_affect_one && n != 1)
                    throw DbError(ErrKind::Conflict, st.type == Change::Type::Delete
                                                          ? "A row to delete no longer exists or was changed by someone else"
                                                          : "A row was modified or deleted by someone else since it was loaded");
                if (st.type == Change::Type::Insert) rep.inserted += (int)n;
                else if (st.type == Change::Type::Update) rep.updated += (int)n;
                else rep.deleted += (int)n;
            }
            if (own_tx) end_tx(SQL_COMMIT);
        } catch (...) {
            if (own_tx && dbc_) end_tx(SQL_ROLLBACK, false);
            throw;
        }
        return rep;
    }

    std::string preview_query(const TableMeta &m, const PageRequest &req) override
    {
        Built b = build_select(Dialect::MsSql, m, req);
        return inline_params(Dialect::MsSql, b.sql, b.params);
    }

    std::string preview_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        std::string out;
        for (auto &st : build_changes(Dialect::MsSql, m, changes)) out += inline_params(Dialect::MsSql, st.q.sql, st.q.params) + ";\n";
        return out;
    }

    QueryOutput execute(const std::string &text, size_t max_rows, const CancelPtr &c) override
    {
        // SQL Server has no read-only session mode: the connector itself refuses anything that is not a plain read.
        if (p_.read_only && !statement_is_read_only(text)) throw DbError(ErrKind::ReadOnly, "Connection is read-only: only SELECT statements are allowed");
        auto t0 = Clock::now();
        std::lock_guard<std::mutex> g(m_);
        ensure();
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        Stmt s(dbc_);
        HookGuard hg(c, s.canceller());
        QueryOutput out;
        guarded([&] {
            s.run(text, {});
            for (;;) {
                if (s.columns() > 0) {
                    out = QueryOutput(); // keep only the last result set
                    auto cols = s.describe();
                    std::vector<TypeClass> cls;
                    for (auto &col : cols) {
                        out.columns.push_back(col.name);
                        cls.push_back(col.cls);
                    }
                    while (s.fetch()) {
                        if (out.rows.size() >= max_rows) {
                            out.truncated = true;
                            s.cancel();
                            return;
                        }
                        out.rows.push_back(read_row(s.h(), cls));
                    }
                } else {
                    out = QueryOutput();
                    out.affected = s.affected();
                }
                SQLRETURN r = SQLMoreResults(s.h());
                if (r == SQL_NO_DATA) break;
                if (!SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_STMT, s.h());
            }
        });
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
        set_autocommit(false);
        in_tx_ = true;
    }
    void commit() override
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        end_tx(SQL_COMMIT);
    }
    void rollback() override
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        end_tx(SQL_ROLLBACK);
    }
    bool in_transaction() const override { return in_tx_; }

private:
    // A failure that kills the connection (08xxx) leaves it unusable: reopen on the next call.
    template <class F> void guarded(F &&f)
    {
        try {
            f();
        } catch (const DbError &e) {
            if (e.kind == ErrKind::Connection) broken_ = true;
            throw;
        }
    }

    static std::string pre(const std::string &db) { return db.empty() ? std::string() : quote_ident(Dialect::MsSql, db) + "."; }
    static std::string str(const Value &v) { return v.is_null() ? std::string() : v.s; }

    void set_autocommit(bool on)
    {
        ODBC_CHECK(SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)(on ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF), 0), SQL_HANDLE_DBC, dbc_);
    }

    void end_tx(SQLSMALLINT how, bool throw_on_error = true)
    {
        SQLRETURN r = SQLEndTran(SQL_HANDLE_DBC, dbc_, how);
        SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);
        in_tx_ = false;
        if (throw_on_error && !SQL_SUCCEEDED(r)) throw_diag(SQL_HANDLE_DBC, dbc_);
    }

    void drop()
    {
        if (dbc_) {
            SQLDisconnect(dbc_);
            SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
            dbc_ = nullptr;
        }
        if (env_) {
            SQLFreeHandle(SQL_HANDLE_ENV, env_);
            env_ = nullptr;
        }
        in_tx_ = false;
    }

    // Called with m_ held. Opens the connection, or reopens it when it died.
    void ensure()
    {
        if (closed_) throw DbError(ErrKind::Connection, "connection closed");
        if (dbc_ && !broken_) {
            SQLUINTEGER dead = SQL_CD_FALSE;
            SQLGetConnectAttr(dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr);
            if (dead != SQL_CD_TRUE) return;
        }
        drop();
        broken_ = false;
        connect_raw();
    }

    std::string connection_string() const
    {
        auto bad = [](const std::string &v) { return v.find(';') != std::string::npos || v.find('{') != std::string::npos || v.find('}') != std::string::npos; };
        if (bad(p_.host)) throw DbError(ErrKind::Validation, "invalid characters in the server name");
        std::string server = p_.host;
        if (server.find('\\') == std::string::npos && p_.port) server += "," + std::to_string(p_.port);
        std::string cs = "Driver={ODBC Driver 18 for SQL Server};Server=" + server + ";";
        if (!p_.database.empty()) cs += "Database=" + braced(p_.database) + ";";
        if (p_.user.empty()) cs += "Trusted_Connection=yes;";
        else cs += "UID=" + braced(p_.user) + ";PWD=" + braced(p_.password) + ";";
        cs += p_.tls ? "Encrypt=yes;" : "Encrypt=no;";
        cs += p_.tls && p_.trust_server_cert ? "TrustServerCertificate=yes;" : "TrustServerCertificate=no;";
        cs += "APP=Db Pro;";
        return cs;
    }

    void connect_raw()
    {
        std::string cs = connection_string();
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_))) throw DbError(ErrKind::Connection, "cannot initialise ODBC");
        SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_))) {
            drop();
            throw DbError(ErrKind::Connection, "cannot allocate an ODBC connection");
        }
        SQLSetConnectAttr(dbc_, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)10, 0);
        std::wstring w = platform::widen(cs);
        SQLWCHAR out[1024];
        SQLSMALLINT outlen = 0;
        SQLRETURN r = SQLDriverConnectW(dbc_, nullptr, wp(w), SQL_NTS, out, 1023, &outlen, SQL_DRIVER_NOPROMPT);
        if (!SQL_SUCCEEDED(r)) {
            Diag d = read_diag(SQL_HANDLE_DBC, dbc_);
            drop();
            std::string msg = d.state == "IM002"
                                  ? "\"ODBC Driver 18 for SQL Server\" is not installed. Install it from Microsoft (search for \"Download ODBC Driver for SQL Server\") and try again."
                                  : d.msg;
            if (p_.tls && !p_.trust_server_cert && (msg.find("certificate") != std::string::npos || msg.find("SSL") != std::string::npos))
                msg += " (TLS is required and the certificate is validated. For a trusted server you may change the TLS options in the connection settings.)";
            throw DbError(ErrKind::Connection, msg.empty() ? "cannot connect to SQL Server" : msg);
        }
        // Parameters travel as text: make date parsing independent of the login's language.
        try {
            Stmt s(dbc_);
            s.run("SET DATEFORMAT ymd", {});
        } catch (...) {
            drop();
            throw;
        }
    }

    // Small metadata queries: every column comes back as text. Takes m_ itself.
    std::vector<std::vector<Value>> query_values(const std::string &sql, const std::vector<Value> &params, size_t ncols)
    {
        std::lock_guard<std::mutex> g(m_);
        ensure();
        Stmt s(dbc_);
        std::vector<std::vector<Value>> out;
        guarded([&] {
            s.run(sql, params);
            std::vector<TypeClass> cls(ncols, TypeClass::Text);
            while (s.fetch()) out.push_back(read_row(s.h(), cls));
        });
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

    MsSqlParams p_;
    SQLHENV env_ = nullptr;
    SQLHDBC dbc_ = nullptr;
    bool broken_ = false, closed_ = false, in_tx_ = false;
    std::mutex m_;
};

} // namespace

std::unique_ptr<Connector> connect_mssql(const MsSqlParams &p)
{
    if (p.host.empty()) throw DbError(ErrKind::Validation, "server is required");
    auto c = std::make_unique<MsConn>(p);
    c->open_first(); // fail here, with the server's message, not on first use
    return c;
}

} // namespace dbp
