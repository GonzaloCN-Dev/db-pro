#include "duckdb_conn.h"

#include <duckdb.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../core/sqlgen.h"

namespace dbp {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t ms_since(Clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

std::string upper(std::string s)
{
    for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

TypeClass classify(const std::string &decl)
{
    std::string t = upper(decl);
    auto starts = [&](const char *p) { return t.rfind(p, 0) == 0; };
    if (t.find('[') != std::string::npos || starts("STRUCT") || starts("MAP") || starts("LIST") || starts("UNION") || starts("ARRAY")) return TypeClass::Json;
    if (starts("ENUM")) return TypeClass::Text;
    if (t == "BOOLEAN" || t == "BOOL") return TypeClass::Bool;
    if (t == "JSON") return TypeClass::Json;
    if (t == "UUID") return TypeClass::Uuid;
    if (t == "BLOB" || t == "BYTEA" || t == "BINARY" || t == "VARBINARY") return TypeClass::Binary;
    if (starts("DECIMAL") || starts("NUMERIC")) return TypeClass::Decimal;
    if (t == "FLOAT" || t == "DOUBLE" || t == "REAL" || t == "FLOAT4" || t == "FLOAT8") return TypeClass::Float;
    if (t.find("INT") != std::string::npos && t != "INTERVAL") return TypeClass::Integer;
    if (t.find("TIMESTAMP") != std::string::npos || starts("DATETIME")) return TypeClass::DateTime;
    if (t == "DATE") return TypeClass::Date;
    if (starts("TIME")) return TypeClass::Time;
    if (starts("VARCHAR") || t == "TEXT" || t == "STRING" || starts("CHAR") || t == "BPCHAR") return TypeClass::Text;
    return TypeClass::Other;
}

// ---------------------------------------------------------------- value rendering
std::string fmt_float(double d, bool single)
{
    if (d != d) return "NaN";
    if (d > 1e308 * 10) return "Infinity";
    if (d < -1e308 * 10) return "-Infinity";
    char buf[40];
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, d);
        double back = 0;
        std::sscanf(buf, "%lf", &back);
        if (single ? (float)back == (float)d : back == d) break;
    }
    return buf;
}

// 128-bit two's-complement integer printed in decimal (no native 128-bit type in MSVC).
std::string int128_to_string(uint64_t lo, int64_t hi, bool is_signed)
{
    bool neg = false;
    uint32_t limb[4] = { (uint32_t)lo, (uint32_t)(lo >> 32), (uint32_t)(uint64_t)hi, (uint32_t)((uint64_t)hi >> 32) };
    if (is_signed && hi < 0) {
        neg = true;
        uint64_t carry = 1;
        for (auto &l : limb) {
            uint64_t v = (uint64_t)(uint32_t)~l + carry;
            l = (uint32_t)v;
            carry = v >> 32;
        }
    }
    std::string out;
    bool zero = false;
    while (!zero) {
        uint64_t rem = 0;
        zero = true;
        for (int k = 3; k >= 0; --k) {
            uint64_t cur = (rem << 32) | limb[k];
            limb[k] = (uint32_t)(cur / 10);
            rem = cur % 10;
            if (limb[k]) zero = false;
        }
        out += (char)('0' + rem);
    }
    if (neg) out += '-';
    std::reverse(out.begin(), out.end());
    return out;
}

std::string with_scale(std::string digits, int scale)
{
    if (scale <= 0) return digits;
    bool neg = !digits.empty() && digits[0] == '-';
    if (neg) digits.erase(0, 1);
    if ((int)digits.size() <= scale) digits.insert(0, (size_t)scale - digits.size() + 1, '0');
    digits.insert(digits.size() - (size_t)scale, ".");
    return neg ? "-" + digits : digits;
}

std::string two(int v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%02d", v);
    return b;
}

std::string fmt_time(int64_t micros)
{
    duckdb_time t;
    t.micros = micros;
    duckdb_time_struct s = duckdb_from_time(t);
    std::string r = two(s.hour) + ":" + two(s.min) + ":" + two(s.sec);
    if (s.micros) {
        char b[16];
        std::snprintf(b, sizeof b, ".%06d", s.micros);
        r += b;
    }
    return r;
}

std::string fmt_date(int32_t days)
{
    if (days == INT32_MAX) return "infinity";
    if (days == -INT32_MAX) return "-infinity";
    duckdb_date d;
    d.days = days;
    duckdb_date_struct s = duckdb_from_date(d);
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", s.year, s.month, s.day);
    return b;
}

std::string fmt_timestamp(int64_t micros, const char *suffix)
{
    if (micros == INT64_MAX) return "infinity";
    if (micros == -INT64_MAX) return "-infinity";
    duckdb_timestamp ts;
    ts.micros = micros;
    duckdb_timestamp_struct s = duckdb_from_timestamp(ts);
    char b[48];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", s.date.year, s.date.month, s.date.day, s.time.hour, s.time.min, s.time.sec);
    std::string r = b;
    if (s.time.micros) {
        std::snprintf(b, sizeof b, ".%06d", s.time.micros);
        r += b;
    }
    return r + suffix;
}

std::string fmt_uuid(uint64_t lo, int64_t hi)
{
    uint64_t up = (uint64_t)hi ^ 0x8000000000000000ULL; // DuckDB flips the sign bit to keep UUIDs ordered
    char b[40];
    std::snprintf(b, sizeof b, "%08x-%04x-%04x-%04x-%012llx", (unsigned)(up >> 32), (unsigned)((up >> 16) & 0xffff), (unsigned)(up & 0xffff),
                  (unsigned)(lo >> 48), (unsigned long long)(lo & 0xffffffffffffULL));
    return b;
}

std::string fmt_interval(int32_t months, int32_t days, int64_t micros)
{
    std::string r;
    auto part = [&](long long n, const char *unit) {
        if (!n) return;
        if (!r.empty()) r += ' ';
        r += std::to_string(n) + " " + unit + (n == 1 || n == -1 ? "" : "s");
    };
    part(months / 12, "year");
    part(months % 12, "month");
    part(days, "day");
    if (micros) {
        if (!r.empty()) r += ' ';
        bool neg = micros < 0;
        int64_t m = neg ? -micros : micros;
        r += (neg ? "-" : "") + fmt_time(m);
    } else if (r.empty()) r = "00:00:00";
    return r;
}

struct LType {
    duckdb_logical_type t = nullptr;
    explicit LType(duckdb_logical_type x) : t(x) {}
    ~LType() { if (t) duckdb_destroy_logical_type(&t); }
    LType(const LType &) = delete;
    LType &operator=(const LType &) = delete;
};

std::string take(char *s)
{
    std::string r = s ? s : "";
    if (s) duckdb_free(s);
    return r;
}

bool valid_at(duckdb_vector v, idx_t row)
{
    uint64_t *m = duckdb_vector_get_validity(v);
    return !m || duckdb_validity_row_is_valid(m, row);
}

std::string render(duckdb_vector vec, duckdb_logical_type t, idx_t row);

std::string render_nested_item(duckdb_vector vec, duckdb_logical_type t, idx_t row)
{
    return render(vec, t, row);
}

std::string render(duckdb_vector vec, duckdb_logical_type t, idx_t row)
{
    if (!valid_at(vec, row)) return "NULL";
    void *data = duckdb_vector_get_data(vec);
    duckdb_type id = duckdb_get_type_id(t);
    switch (id) {
    case DUCKDB_TYPE_BOOLEAN: return ((bool *)data)[row] ? "true" : "false";
    case DUCKDB_TYPE_TINYINT: return std::to_string(((int8_t *)data)[row]);
    case DUCKDB_TYPE_SMALLINT: return std::to_string(((int16_t *)data)[row]);
    case DUCKDB_TYPE_INTEGER: return std::to_string(((int32_t *)data)[row]);
    case DUCKDB_TYPE_BIGINT: return std::to_string(((int64_t *)data)[row]);
    case DUCKDB_TYPE_UTINYINT: return std::to_string(((uint8_t *)data)[row]);
    case DUCKDB_TYPE_USMALLINT: return std::to_string(((uint16_t *)data)[row]);
    case DUCKDB_TYPE_UINTEGER: return std::to_string(((uint32_t *)data)[row]);
    case DUCKDB_TYPE_UBIGINT: return std::to_string(((uint64_t *)data)[row]);
    case DUCKDB_TYPE_FLOAT: return fmt_float(((float *)data)[row], true);
    case DUCKDB_TYPE_DOUBLE: return fmt_float(((double *)data)[row], false);
    case DUCKDB_TYPE_HUGEINT: {
        auto h = ((duckdb_hugeint *)data)[row];
        return int128_to_string(h.lower, h.upper, true);
    }
    case DUCKDB_TYPE_UHUGEINT: {
        auto h = ((duckdb_uhugeint *)data)[row];
        return int128_to_string(h.lower, (int64_t)h.upper, false);
    }
    case DUCKDB_TYPE_UUID: {
        auto h = ((duckdb_hugeint *)data)[row];
        return fmt_uuid(h.lower, h.upper);
    }
    case DUCKDB_TYPE_DATE: return fmt_date(((int32_t *)data)[row]);
    case DUCKDB_TYPE_TIME: return fmt_time(((int64_t *)data)[row]);
    case DUCKDB_TYPE_TIME_NS: return fmt_time(((int64_t *)data)[row] / 1000);
    case DUCKDB_TYPE_TIMESTAMP: return fmt_timestamp(((int64_t *)data)[row], "");
    case DUCKDB_TYPE_TIMESTAMP_TZ: return fmt_timestamp(((int64_t *)data)[row], "+00");
    case DUCKDB_TYPE_TIMESTAMP_S: return fmt_timestamp(((int64_t *)data)[row] * 1000000, "");
    case DUCKDB_TYPE_TIMESTAMP_MS: return fmt_timestamp(((int64_t *)data)[row] * 1000, "");
    case DUCKDB_TYPE_TIMESTAMP_NS: return fmt_timestamp(((int64_t *)data)[row] / 1000, "");
    case DUCKDB_TYPE_INTERVAL: {
        auto iv = ((duckdb_interval *)data)[row];
        return fmt_interval(iv.months, iv.days, iv.micros);
    }
    case DUCKDB_TYPE_VARCHAR: {
        duckdb_string_t s = ((duckdb_string_t *)data)[row];
        uint32_t n = s.value.inlined.length;
        return std::string(n <= 12 ? s.value.inlined.inlined : s.value.pointer.ptr, n);
    }
    case DUCKDB_TYPE_BLOB: {
        duckdb_string_t s = ((duckdb_string_t *)data)[row];
        uint32_t n = s.value.inlined.length;
        return hex_preview(std::string(n <= 12 ? s.value.inlined.inlined : s.value.pointer.ptr, n), 64);
    }
    case DUCKDB_TYPE_DECIMAL: {
        int scale = duckdb_decimal_scale(t);
        switch (duckdb_decimal_internal_type(t)) {
        case DUCKDB_TYPE_SMALLINT: return with_scale(std::to_string(((int16_t *)data)[row]), scale);
        case DUCKDB_TYPE_INTEGER: return with_scale(std::to_string(((int32_t *)data)[row]), scale);
        case DUCKDB_TYPE_BIGINT: return with_scale(std::to_string(((int64_t *)data)[row]), scale);
        default: {
            auto h = ((duckdb_hugeint *)data)[row];
            return with_scale(int128_to_string(h.lower, h.upper, true), scale);
        }
        }
    }
    case DUCKDB_TYPE_ENUM: {
        idx_t k = 0;
        switch (duckdb_enum_internal_type(t)) {
        case DUCKDB_TYPE_UTINYINT: k = ((uint8_t *)data)[row]; break;
        case DUCKDB_TYPE_USMALLINT: k = ((uint16_t *)data)[row]; break;
        default: k = ((uint32_t *)data)[row]; break;
        }
        return take(duckdb_enum_dictionary_value(t, k));
    }
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_MAP: {
        auto e = ((duckdb_list_entry *)data)[row];
        duckdb_vector child = duckdb_list_vector_get_child(vec);
        std::string out = id == DUCKDB_TYPE_MAP ? "{" : "[";
        if (id == DUCKDB_TYPE_MAP) {
            LType kt(duckdb_map_type_key_type(t)), vt(duckdb_map_type_value_type(t));
            duckdb_vector keys = duckdb_struct_vector_get_child(child, 0), vals = duckdb_struct_vector_get_child(child, 1);
            for (idx_t k = 0; k < e.length; ++k)
                out += (k ? ", " : "") + render_nested_item(keys, kt.t, e.offset + k) + "=" + render_nested_item(vals, vt.t, e.offset + k);
        } else {
            LType ct(duckdb_list_type_child_type(t));
            for (idx_t k = 0; k < e.length; ++k) out += (k ? ", " : "") + render_nested_item(child, ct.t, e.offset + k);
        }
        return out + (id == DUCKDB_TYPE_MAP ? "}" : "]");
    }
    case DUCKDB_TYPE_ARRAY: {
        idx_t size = duckdb_array_type_array_size(t);
        LType ct(duckdb_array_type_child_type(t));
        duckdb_vector child = duckdb_array_vector_get_child(vec);
        std::string out = "[";
        for (idx_t k = 0; k < size; ++k) out += (k ? ", " : "") + render_nested_item(child, ct.t, row * size + k);
        return out + "]";
    }
    case DUCKDB_TYPE_STRUCT: {
        idx_t n = duckdb_struct_type_child_count(t);
        std::string out = "{";
        for (idx_t k = 0; k < n; ++k) {
            LType ct(duckdb_struct_type_child_type(t, k));
            out += (k ? ", " : "") + std::string("'") + take(duckdb_struct_type_child_name(t, k)) + "': "
                   + render_nested_item(duckdb_struct_vector_get_child(vec, k), ct.t, row);
        }
        return out + "}";
    }
    case DUCKDB_TYPE_UNION: {
        duckdb_vector tags = duckdb_struct_vector_get_child(vec, 0);
        uint8_t tag = ((uint8_t *)duckdb_vector_get_data(tags))[row];
        LType mt(duckdb_union_type_member_type(t, tag));
        return render_nested_item(duckdb_struct_vector_get_child(vec, 1 + tag), mt.t, row);
    }
    default: return "(unsupported DuckDB type " + std::to_string((int)id) + ")";
    }
}

// Top-level cell: native Value for the simple types, DuckDB's own text for everything else.
Value cell(duckdb_vector vec, duckdb_logical_type t, idx_t row, TypeClass cls)
{
    if (!valid_at(vec, row)) return Value::null();
    void *data = duckdb_vector_get_data(vec);
    switch (duckdb_get_type_id(t)) {
    case DUCKDB_TYPE_BOOLEAN: return Value::boolean(((bool *)data)[row]);
    case DUCKDB_TYPE_TINYINT: return Value::integer(((int8_t *)data)[row]);
    case DUCKDB_TYPE_SMALLINT: return Value::integer(((int16_t *)data)[row]);
    case DUCKDB_TYPE_INTEGER: return Value::integer(((int32_t *)data)[row]);
    case DUCKDB_TYPE_BIGINT: return Value::integer(((int64_t *)data)[row]);
    case DUCKDB_TYPE_UTINYINT: return Value::integer(((uint8_t *)data)[row]);
    case DUCKDB_TYPE_USMALLINT: return Value::integer(((uint16_t *)data)[row]);
    case DUCKDB_TYPE_UINTEGER: return Value::integer(((uint32_t *)data)[row]);
    case DUCKDB_TYPE_UBIGINT: {
        uint64_t n = ((uint64_t *)data)[row];
        if (n <= (uint64_t)INT64_MAX) return Value::integer((int64_t)n);
        return Value::of(Value::Kind::Decimal, std::to_string(n));
    }
    case DUCKDB_TYPE_FLOAT: return Value::real(((float *)data)[row]);
    case DUCKDB_TYPE_DOUBLE: return Value::real(((double *)data)[row]);
    case DUCKDB_TYPE_BLOB: {
        duckdb_string_t s = ((duckdb_string_t *)data)[row];
        uint32_t n = s.value.inlined.length;
        return Value::of(Value::Kind::Bytes, std::string(n <= 12 ? s.value.inlined.inlined : s.value.pointer.ptr, n));
    }
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT: return Value::of(Value::Kind::Decimal, render(vec, t, row));
    default: return Value::from_text_for(cls, render(vec, t, row));
    }
}

// ---------------------------------------------------------------- statements
struct Result {
    duckdb_result r;
    Result() { std::memset(&r, 0, sizeof r); }
    ~Result() { duckdb_destroy_result(&r); }
    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;
};

struct Prep {
    duckdb_prepared_statement p = nullptr;
    ~Prep() { duckdb_destroy_prepare(&p); }
    Prep() = default;
    Prep(const Prep &) = delete;
    Prep &operator=(const Prep &) = delete;
};

[[noreturn]] void fail_query(const char *msg)
{
    std::string m = msg && *msg ? msg : "query failed";
    if (m.find("INTERRUPT") != std::string::npos) throw DbError(ErrKind::Cancelled, "operation cancelled");
    throw DbError(ErrKind::Query, m);
}

void bind_params(duckdb_prepared_statement p, const std::vector<Value> &params)
{
    for (size_t k = 0; k < params.size(); ++k) {
        const Value &v = params[k];
        duckdb_value dv = nullptr;
        switch (v.kind) {
        case Value::Kind::Null:
        case Value::Kind::Missing:
        case Value::Kind::Default: dv = duckdb_create_null_value(); break;
        case Value::Kind::Bool: dv = duckdb_create_bool(v.i != 0); break;
        case Value::Kind::Int: dv = duckdb_create_int64(v.i); break;
        case Value::Kind::Float: dv = duckdb_create_double(v.d); break;
        case Value::Kind::Bytes: dv = duckdb_create_blob((const uint8_t *)v.s.data(), v.s.size()); break;
        default: dv = duckdb_create_varchar_length(v.s.data(), v.s.size()); break;
        }
        duckdb_state st = duckdb_bind_value(p, (idx_t)k + 1, dv);
        duckdb_destroy_value(&dv);
        if (st != DuckDBSuccess) throw DbError(ErrKind::Query, "could not bind parameter " + std::to_string(k + 1));
    }
}

class DuckConn : public Connector {
public:
    DuckConn(duckdb_database db, duckdb_connection con, bool ro) : db_(db), con_(con), ro_(ro) {}
    ~DuckConn() override { close(); }

    Capabilities caps() const override
    {
        Capabilities c;
        c.transactions = true;
        c.edit = !ro_;
        c.regex = true;
        c.schemas = true;
        c.sql = true;
        c.default_keyword = true;
        c.activity = true;
        return c;
    }

    std::string server_info() override { return std::string("DuckDB ") + duckdb_library_version(); }

    void close() override
    {
        std::lock_guard<std::mutex> g(m_);
        if (con_) duckdb_disconnect(&con_);
        if (db_) duckdb_close(&db_);
        con_ = nullptr;
        db_ = nullptr;
    }

    std::vector<ObjectInfo> list_databases() override
    {
        std::vector<ObjectInfo> out;
        for (auto &r : query("SELECT database_name FROM duckdb_databases() WHERE NOT internal ORDER BY database_name", {}))
            out.push_back({ r[0].s, ObjectKind::Database });
        return out;
    }

    std::vector<ObjectInfo> list_schemas(const std::string &db) override
    {
        std::vector<ObjectInfo> out;
        for (auto &r : query("SELECT schema_name FROM duckdb_schemas() WHERE database_name = ? AND schema_name NOT IN ('information_schema', 'pg_catalog') ORDER BY schema_name", { Value::text(db) }))
            out.push_back({ r[0].s, ObjectKind::Schema });
        return out;
    }

    std::vector<ObjectInfo> list_tables(const std::string &db, const std::string &schema) override
    {
        std::vector<ObjectInfo> out;
        auto rows = query("SELECT table_name, table_type FROM information_schema.tables WHERE table_catalog = ? AND table_schema = ? ORDER BY table_type, table_name",
                          { Value::text(db), Value::text(schema.empty() ? "main" : schema) });
        for (auto &r : rows) out.push_back({ r[0].s, r[1].s == "VIEW" ? ObjectKind::View : ObjectKind::Table });
        return out;
    }

    TableMeta table_meta(const TableRef &t) override
    {
        TableMeta m;
        m.table = t;
        if (m.table.schema.empty()) m.table.schema = "main";
        if (m.table.database.empty()) {
            auto r = query("SELECT current_database()", {});
            m.table.database = r.empty() ? "" : r[0][0].s;
        }
        std::vector<Value> key = { Value::text(m.table.database), Value::text(m.table.schema), Value::text(m.table.name) };

        auto kind = query("SELECT table_type FROM information_schema.tables WHERE table_catalog = ? AND table_schema = ? AND table_name = ?", key);
        if (kind.empty()) throw DbError(ErrKind::Query, "table not found: " + t.name);
        m.is_view = kind[0][0].s == "VIEW";

        for (auto &r : query("SELECT column_name, data_type, is_nullable, column_default FROM duckdb_columns() "
                             "WHERE database_name = ? AND schema_name = ? AND table_name = ? ORDER BY column_index", key)) {
            ColumnInfo c;
            c.name = r[0].s;
            c.native_type = r[1].s;
            c.cls = classify(c.native_type);
            c.nullable = r[2].kind == Value::Kind::Bool ? r[2].i != 0 : true;
            c.has_default = !r[3].is_null();
            m.columns.push_back(std::move(c));
        }

        std::vector<std::string> pk;
        for (auto &r : query("SELECT unnest(constraint_column_names) FROM duckdb_constraints() "
                             "WHERE database_name = ? AND schema_name = ? AND table_name = ? AND constraint_type = 'PRIMARY KEY'", key))
            pk.push_back(r[0].s);
        for (size_t k = 0; k < pk.size(); ++k)
            for (auto &c : m.columns)
                if (c.name == pk[k]) c.pk_position = (int)k + 1;

        for (auto &r : query("SELECT index_name, is_unique FROM duckdb_indexes() WHERE database_name = ? AND schema_name = ? AND table_name = ?", key)) {
            IndexInfo ix;
            ix.name = r[0].s;
            ix.unique = r[1].kind == Value::Kind::Bool && r[1].i != 0;
            m.indexes.push_back(ix);
        }

        auto est = query("SELECT estimated_size FROM duckdb_tables() WHERE database_name = ? AND schema_name = ? AND table_name = ?", key);
        if (!est.empty() && est[0][0].kind == Value::Kind::Int) m.estimated_rows = est[0][0].i;

        if (m.is_view) m.edit = { {}, false, "Views are read-only" };
        else if (ro_) m.edit = { {}, false, "Connection is read-only" };
        else if (pk.empty()) m.edit = { {}, false, "No primary key: rows cannot be identified unambiguously" };
        else {
            m.edit.key_columns = pk;
            m.edit.editable = true;
        }
        return m;
    }

    Page fetch_page(const TableMeta &m, const PageRequest &req, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        Built b = build_select(Dialect::DuckDb, m, req);
        Page p;
        p.generated = inline_params(Dialect::DuckDb, b.sql, b.params);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [con = con_] { duckdb_interrupt(con); });
        if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
        Result res;
        run(b.sql, b.params, res);
        auto cols = result_cols(m, req);
        walk(res, cols, [&](std::vector<Value> &&row) {
            if (p.rows.size() >= req.limit) {
                p.has_more = true;
                return false;
            }
            p.rows.push_back(std::move(row));
            return true;
        }, c);
        p.elapsed_ms = ms_since(t0);
        return p;
    }

    uint64_t count(const TableMeta &m, const FilterGroup &f, const CancelPtr &c) override
    {
        Built b = build_count(Dialect::DuckDb, m, f);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [con = con_] { duckdb_interrupt(con); });
        throw_if_cancelled(c);
        Result res;
        run(b.sql, b.params, res);
        uint64_t n = 0;
        walk(res, {}, [&](std::vector<Value> &&row) {
            n = row.empty() || row[0].kind != Value::Kind::Int ? 0 : (uint64_t)row[0].i;
            return false;
        }, c);
        return n;
    }

    std::vector<Value> distinct_values(const TableMeta &m, const DistinctRequest &r, const CancelPtr &c) override
    {
        Built b = build_distinct(Dialect::DuckDb, m, r);
        const ColumnInfo *ci = m.column(r.column);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [con = con_] { duckdb_interrupt(con); });
        throw_if_cancelled(c);
        Result res;
        run(b.sql, b.params, res);
        std::vector<Value> out;
        walk(res, { ci }, [&](std::vector<Value> &&row) {
            out.push_back(std::move(row[0]));
            return true;
        }, c);
        return out;
    }

    uint64_t stream_rows(const TableMeta &m, const PageRequest &req, size_t batch, const RowBatchFn &fn, const CancelPtr &c) override
    {
        PageRequest r = req;
        r.limit = 0x7fffffff;
        r.offset = 0;
        Built b = build_select(Dialect::DuckDb, m, r, false);
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [con = con_] { duckdb_interrupt(con); });
        throw_if_cancelled(c);
        Result res;
        run(b.sql, b.params, res);
        std::vector<std::vector<Value>> buf;
        uint64_t total = 0;
        bool stopped = false;
        walk(res, result_cols(m, req), [&](std::vector<Value> &&row) {
            buf.push_back(std::move(row));
            ++total;
            if (buf.size() >= batch) {
                if (!fn(buf)) {
                    stopped = true;
                    return false;
                }
                buf.clear();
            }
            return true;
        }, c);
        if (!stopped && !buf.empty()) fn(buf);
        return total;
    }

    Activity activity() override
    {
        // DuckDB is embedded, not a server with independent sessions. Report engine-provided
        // local memory usage only; do not synthesize server process/session metrics.
        auto rows = query("SELECT coalesce(sum(memory_usage_bytes), 0), coalesce(sum(temporary_storage_bytes), 0) FROM duckdb_memory()", {});
        Activity a;
        auto value = [](const Value &v) -> double {
            if (v.kind == Value::Kind::Int) return (double)v.i;
            if (v.kind == Value::Kind::Float) return v.d;
            char *end = nullptr;
            double n = std::strtod(v.s.c_str(), &end);
            return end == v.s.c_str() + v.s.size() ? n : 0;
        };
        if (!rows.empty() && rows[0].size() >= 2) {
            a.metrics.push_back({ "memory_used", "Engine memory", value(rows[0][0]), false, "B" });
            a.metrics.push_back({ "temporary_storage", "Temporary storage", value(rows[0][1]), false, "B" });
        }
        return a;
    }

    ApplyReport apply_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        if (ro_) throw DbError(ErrKind::ReadOnly, "Connection is read-only");
        if (!m.edit.editable) throw DbError(ErrKind::Validation, m.edit.blocked_reason);
        auto stmts = build_changes(Dialect::DuckDb, m, changes);
        std::lock_guard<std::mutex> g(m_);
        bool own_tx = !in_tx_;
        if (own_tx) exec_raw("BEGIN TRANSACTION");
        ApplyReport rep;
        try {
            for (auto &st : stmts) {
                Result res;
                run(st.q.sql, st.q.params, res);
                int64_t n = (int64_t)duckdb_rows_changed(&res.r);
                if (st.must_affect_one && n != 1)
                    throw DbError(ErrKind::Conflict, st.type == Change::Type::Delete
                        ? "A row to delete no longer exists or was changed by someone else"
                        : "A row was modified or deleted by someone else since it was loaded");
                if (st.type == Change::Type::Insert) rep.inserted += (int)n;
                else if (st.type == Change::Type::Update) rep.updated += (int)n;
                else rep.deleted += (int)n;
            }
            if (own_tx) exec_raw("COMMIT");
        } catch (...) {
            if (own_tx) {
                Result r;
                duckdb_query(con_, "ROLLBACK", &r.r);
            }
            throw;
        }
        return rep;
    }

    std::string preview_query(const TableMeta &m, const PageRequest &req) override
    {
        Built b = build_select(Dialect::DuckDb, m, req);
        return inline_params(Dialect::DuckDb, b.sql, b.params);
    }

    std::string preview_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        std::string out;
        for (auto &st : build_changes(Dialect::DuckDb, m, changes)) out += inline_params(Dialect::DuckDb, st.q.sql, st.q.params) + ";\n";
        return out;
    }

    QueryOutput execute(const std::string &text, size_t max_rows, const CancelPtr &c) override
    {
        auto t0 = Clock::now();
        std::lock_guard<std::mutex> g(m_);
        HookGuard hg(c, [con = con_] { duckdb_interrupt(con); });
        throw_if_cancelled(c);
        QueryOutput out;
        duckdb_extracted_statements ex = nullptr;
        idx_t n = duckdb_extract_statements(con_, text.c_str(), &ex);
        struct Fin { duckdb_extracted_statements *e; ~Fin() { duckdb_destroy_extracted(e); } } fin { &ex };
        if (n == 0) {
            const char *e = duckdb_extract_statements_error(ex);
            if (e && *e) throw DbError(ErrKind::Query, e);
        }
        for (idx_t k = 0; k < n; ++k) {
            Prep pr;
            if (duckdb_prepare_extracted_statement(con_, ex, k, &pr.p) != DuckDBSuccess) fail_query(duckdb_prepare_error(pr.p));
            Result res;
            if (duckdb_execute_prepared_streaming(pr.p, &res.r) != DuckDBSuccess) fail_query(duckdb_result_error(&res.r));
            out = QueryOutput();
            if (duckdb_result_return_type(res.r) == DUCKDB_RESULT_TYPE_CHANGED_ROWS || duckdb_result_return_type(res.r) == DUCKDB_RESULT_TYPE_NOTHING) {
                out.affected = duckdb_result_return_type(res.r) == DUCKDB_RESULT_TYPE_CHANGED_ROWS ? (int64_t)duckdb_rows_changed(&res.r) : 0;
                continue;
            }
            idx_t ncol = duckdb_column_count(&res.r);
            for (idx_t i = 0; i < ncol; ++i) out.columns.push_back(duckdb_column_name(&res.r, i));
            walk(res, {}, [&](std::vector<Value> &&row) {
                if (out.rows.size() >= max_rows) {
                    out.truncated = true;
                    return false;
                }
                out.rows.push_back(std::move(row));
                return true;
            }, c);
            if (out.truncated) break;
        }
        out.elapsed_ms = ms_since(t0);
        out.message = n == 0 ? "Nothing to execute" : out.columns.empty()
            ? std::to_string(out.affected < 0 ? 0 : out.affected) + " row(s) affected"
            : std::to_string(out.rows.size()) + " row(s)" + (out.truncated ? " (truncated)" : "");
        return out;
    }

    void begin() override
    {
        std::lock_guard<std::mutex> g(m_);
        if (in_tx_) throw DbError(ErrKind::Query, "a transaction is already open");
        exec_raw("BEGIN TRANSACTION");
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
        Result r;
        if (duckdb_query(con_, sql, &r.r) != DuckDBSuccess) fail_query(duckdb_result_error(&r.r));
    }

    // Prepares, binds and starts a streaming execution. The caller holds m_.
    void run(const std::string &sql, const std::vector<Value> &params, Result &res)
    {
        Prep pr;
        if (duckdb_prepare(con_, sql.c_str(), &pr.p) != DuckDBSuccess) fail_query(duckdb_prepare_error(pr.p));
        bind_params(pr.p, params);
        if (duckdb_execute_prepared_streaming(pr.p, &res.r) != DuckDBSuccess) fail_query(duckdb_result_error(&res.r));
    }

    // Reads rows chunk by chunk. `cols` supplies the type class of each column (empty = text).
    template <class F>
    void walk(Result &res, const std::vector<const ColumnInfo *> &cols, F &&on_row, const CancelPtr &c)
    {
        if (duckdb_result_return_type(res.r) != DUCKDB_RESULT_TYPE_QUERY_RESULT) return;
        idx_t ncol = duckdb_column_count(&res.r);
        bool go = true;
        while (go) {
            if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
            duckdb_data_chunk ch = duckdb_fetch_chunk(res.r);
            if (!ch) break;
            struct Free { duckdb_data_chunk *c; ~Free() { duckdb_destroy_data_chunk(c); } } fr { &ch };
            idx_t rows = duckdb_data_chunk_get_size(ch);
            std::vector<duckdb_vector> vecs(ncol);
            std::vector<std::unique_ptr<LType>> types;
            for (idx_t i = 0; i < ncol; ++i) {
                vecs[i] = duckdb_data_chunk_get_vector(ch, i);
                types.push_back(std::make_unique<LType>(duckdb_vector_get_column_type(vecs[i])));
            }
            for (idx_t r = 0; r < rows && go; ++r) {
                std::vector<Value> row;
                row.reserve(ncol);
                for (idx_t i = 0; i < ncol; ++i)
                    row.push_back(cell(vecs[i], types[i]->t, r, i < cols.size() && cols[i] ? cols[i]->cls : TypeClass::Text));
                go = on_row(std::move(row));
            }
        }
        const char *err = duckdb_result_error(&res.r);
        if (go && err && *err) fail_query(err);
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

    std::vector<std::vector<Value>> query(const std::string &sql, const std::vector<Value> &params)
    {
        std::lock_guard<std::mutex> g(m_);
        Result res;
        run(sql, params, res);
        std::vector<std::vector<Value>> rows;
        walk(res, {}, [&](std::vector<Value> &&row) {
            rows.push_back(std::move(row));
            return true;
        }, nullptr);
        return rows;
    }

    duckdb_database db_;
    duckdb_connection con_;
    bool ro_;
    bool in_tx_ = false;
    std::mutex m_;
};

} // namespace

std::unique_ptr<Connector> connect_duckdb(const std::string &path, bool read_only)
{
    if (path.empty()) throw DbError(ErrKind::Validation, "DuckDB file path is required");
    duckdb_config cfg = nullptr;
    if (duckdb_create_config(&cfg) != DuckDBSuccess) throw DbError(ErrKind::Connection, "could not create DuckDB configuration");
    struct Cfg { duckdb_config *c; ~Cfg() { duckdb_destroy_config(c); } } cg { &cfg };
    if (read_only && path != ":memory:") duckdb_set_config(cfg, "access_mode", "READ_ONLY");
    duckdb_set_config(cfg, "autoinstall_known_extensions", "false");
    duckdb_set_config(cfg, "autoload_known_extensions", "false");

    duckdb_database db = nullptr;
    char *err = nullptr;
    if (duckdb_open_ext(path == ":memory:" ? nullptr : path.c_str(), &db, cfg, &err) != DuckDBSuccess) {
        std::string e = err ? err : "cannot open database";
        if (err) duckdb_free(err);
        throw DbError(ErrKind::Connection, e);
    }
    duckdb_connection con = nullptr;
    if (duckdb_connect(db, &con) != DuckDBSuccess) {
        duckdb_close(&db);
        throw DbError(ErrKind::Connection, "cannot connect to the DuckDB database");
    }
    return std::make_unique<DuckConn>(db, con, read_only);
}

} // namespace dbp
