#include "mongodb_conn.h"

#include <winsock2.h>
#include <windows.h>

#include <cmath>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/make_value.hpp>
#include <bsoncxx/types/bson_value/value.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <cstring>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/aggregate.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace dbp {

namespace {

using Clock = std::chrono::steady_clock;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
namespace bb = bsoncxx::builder::basic;
namespace bt = bsoncxx::types;
using BVal = bsoncxx::types::bson_value::value;

uint64_t ms_since(Clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

[[noreturn]] void fail(ErrKind k, const std::string &m) { throw DbError(k, m); }

mongocxx::instance &instance()
{
    static mongocxx::instance *inst = new mongocxx::instance(); // intentionally leaked: the DLL may unload after the app's static destructors
    return *inst;
}

// -------------------------------------------------------------- errors
bool looks_like_connection_problem(const std::string &m)
{
    static const char *keys[] = { "No suitable servers", "Failed to resolve", "connection", "Connection", "socket", "SSL", "TLS", "certificate", "Authentication failed", "authentication", "timed out" };
    for (auto k : keys)
        if (m.find(k) != std::string::npos) return true;
    return false;
}

[[noreturn]] void translate(const std::exception &e, bool connecting = false)
{
    std::string m = e.what();
    int code = 0;
    bool server = false;
    if (auto *me = dynamic_cast<const mongocxx::operation_exception *>(&e)) {
        if (me->raw_server_error()) {
            auto c = me->raw_server_error()->view()["code"];
            if (c) code = c.type() == bsoncxx::type::k_int64 ? (int)c.get_int64().value : c.type() == bsoncxx::type::k_int32 ? c.get_int32().value : 0;
            server = code != 0;
        }
    }
    for (const char *suffix : { ": generic server error", ": generic server error." }) {
        size_t n = std::strlen(suffix);
        if (m.size() > n && m.compare(m.size() - n, n, suffix) == 0) m.erase(m.size() - n);
    }
    if (server) {
        if (code == 11601 || code == 11602 || code == 237) throw DbError(ErrKind::Cancelled, "operation cancelled");
        if (code == 50) throw DbError(ErrKind::Timeout, "the operation exceeded its time limit");
        if (code == 18) throw DbError(ErrKind::Connection, "authentication failed: " + m);
        throw DbError(ErrKind::Query, m);
    }
    if (connecting || looks_like_connection_problem(m)) throw DbError(ErrKind::Connection, m);
    throw DbError(ErrKind::Query, m);
}

template <class F> auto guard(F &&f) -> decltype(f())
{
    try {
        return f();
    } catch (const DbError &) {
        throw;
    } catch (const std::exception &e) {
        translate(e);
    }
}

// -------------------------------------------------------------- time
int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_days(int64_t z, int64_t &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

std::string format_date(int64_t ms)
{
    int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    int64_t rem = ms - days * 86400000;
    int64_t y;
    unsigned mo, d;
    civil_from_days(days, y, mo, d);
    char buf[48];
    int h = (int)(rem / 3600000), mi = (int)(rem / 60000 % 60), s = (int)(rem / 1000 % 60), f = (int)(rem % 1000);
    if (f) std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02d:%02d:%02d.%03d", (long long)y, mo, d, h, mi, s, f);
    else std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02d:%02d:%02d", (long long)y, mo, d, h, mi, s);
    return buf;
}

int64_t parse_date(const std::string &t)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, n = 0;
    double frac = 0;
    int got = std::sscanf(t.c_str(), "%d-%d-%d%n", &y, &mo, &d, &n);
    if (got < 3 || mo < 1 || mo > 12 || d < 1 || d > 31) fail(ErrKind::Validation, "invalid date: " + t);
    const char *p = t.c_str() + n;
    if (*p == ' ' || *p == 'T') {
        int n2 = 0;
        int g = std::sscanf(p + 1, "%d:%d%n", &h, &mi, &n2);
        if (g < 2 || h > 23 || mi > 59) fail(ErrKind::Validation, "invalid time in date: " + t);
        p += 1 + n2;
        if (*p == ':') {
            int n3 = 0;
            if (std::sscanf(p + 1, "%d%n", &s, &n3) < 1 || s > 59) fail(ErrKind::Validation, "invalid seconds in date: " + t);
            p += 1 + n3;
            if (*p == '.') {
                char *end = nullptr;
                frac = std::strtod(p, &end);
                p = end;
            }
        }
    }
    if (*p == 'Z') ++p;
    if (*p) fail(ErrKind::Validation, "invalid date: " + t);
    int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)d);
    return days * 86400000 + (int64_t)h * 3600000 + (int64_t)mi * 60000 + (int64_t)s * 1000 + (int64_t)(frac * 1000 + 0.5);
}

// -------------------------------------------------------------- BSON <-> Value
using bsoncxx::type;

const char *type_name(type t)
{
    switch (t) {
    case type::k_double: return "double";
    case type::k_string: return "string";
    case type::k_document: return "object";
    case type::k_array: return "array";
    case type::k_binary: return "binData";
    case type::k_undefined: return "undefined";
    case type::k_oid: return "objectId";
    case type::k_bool: return "bool";
    case type::k_date: return "date";
    case type::k_null: return "null";
    case type::k_regex: return "regex";
    case type::k_dbpointer: return "dbPointer";
    case type::k_code: return "javascript";
    case type::k_symbol: return "symbol";
    case type::k_codewscope: return "javascriptWithScope";
    case type::k_int32: return "int";
    case type::k_timestamp: return "timestamp";
    case type::k_int64: return "long";
    case type::k_decimal128: return "decimal";
    case type::k_minkey: return "minKey";
    case type::k_maxkey: return "maxKey";
    }
    return "unknown";
}

std::string relaxed_json(const bsoncxx::document::view &v) { return bsoncxx::to_json(v, bsoncxx::ExtendedJsonMode::k_relaxed); }

// JSON text of a single element of any type: wrap it in a document and strip the wrapper.
std::string element_json(const bsoncxx::document::element &e)
{
    std::string j = bsoncxx::to_json(make_document(kvp("v", e.get_value())).view(), bsoncxx::ExtendedJsonMode::k_relaxed);
    size_t colon = j.find(':');
    size_t end = j.rfind('}');
    if (colon == std::string::npos || end == std::string::npos) return j;
    std::string r = j.substr(colon + 1, end - colon - 1);
    size_t a = r.find_first_not_of(' '), b = r.find_last_not_of(' ');
    return a == std::string::npos ? r : r.substr(a, b - a + 1);
}

Value to_value(const bsoncxx::document::element &e)
{
    switch (e.type()) {
    case type::k_int32: return Value::integer(e.get_int32().value);
    case type::k_int64: return Value::integer(e.get_int64().value);
    case type::k_double: return Value::real(e.get_double().value);
    case type::k_decimal128: return Value::of(Value::Kind::Decimal, e.get_decimal128().value.to_string());
    case type::k_string: return Value::text(std::string(e.get_string().value));
    case type::k_bool: return Value::boolean(e.get_bool().value);
    case type::k_date: return Value::of(Value::Kind::Temporal, format_date(e.get_date().to_int64()));
    case type::k_oid: return Value::of(Value::Kind::ObjectId, e.get_oid().value.to_string());
    case type::k_null: return Value::null();
    case type::k_document: return Value::of(Value::Kind::Json, relaxed_json(e.get_document().value));
    case type::k_array: return Value::of(Value::Kind::Json, bsoncxx::to_json(e.get_array().value, bsoncxx::ExtendedJsonMode::k_relaxed));
    case type::k_binary: {
        auto b = e.get_binary();
        return Value::of(Value::Kind::Bytes, std::string(reinterpret_cast<const char *>(b.bytes), b.size));
    }
    default: return Value::text(element_json(e));
    }
}

BVal bson_of(const Value &v)
{
    using namespace bsoncxx::types;
    switch (v.kind) {
    case Value::Kind::Null:
    case Value::Kind::Missing:
    case Value::Kind::Default: return BVal(b_null {});
    case Value::Kind::Bool: return BVal(b_bool { v.i != 0 });
    case Value::Kind::Int:
        if (v.i >= INT32_MIN && v.i <= INT32_MAX) return BVal(b_int32 { (int32_t)v.i });
        return BVal(b_int64 { v.i });
    case Value::Kind::Float: return BVal(b_double { v.d });
    case Value::Kind::Decimal:
        try {
            return BVal(b_decimal128 { bsoncxx::decimal128(v.s) });
        } catch (const std::exception &) {
            fail(ErrKind::Validation, "invalid decimal number: " + v.s);
        }
    case Value::Kind::Text:
    case Value::Kind::Uuid: return BVal(b_string { v.s });
    case Value::Kind::Bytes: return BVal(b_binary { bsoncxx::binary_sub_type::k_binary, (uint32_t)v.s.size(), reinterpret_cast<const uint8_t *>(v.s.data()) });
    case Value::Kind::Temporal: return BVal(b_date { std::chrono::milliseconds(parse_date(v.s)) });
    case Value::Kind::ObjectId:
        try {
            return BVal(b_oid { bsoncxx::oid(v.s) });
        } catch (const std::exception &) {
            fail(ErrKind::Validation, "invalid ObjectId (24 hexadecimal characters expected): " + v.s);
        }
    case Value::Kind::Json:
        try {
            auto d = bsoncxx::from_json("{\"v\":" + v.s + "}");
            return BVal(d.view()["v"].get_value());
        } catch (const std::exception &) {
            fail(ErrKind::Validation, "invalid JSON: " + v.s);
        }
    }
    return BVal(b_null {});
}

// A value typed in the grid takes the numeric type the field already had, so editing never silently changes int to double.
BVal bson_for_set(const Value &v, const Value &original)
{
    if (v.kind == Value::Kind::Float && original.kind == Value::Kind::Int && v.d == (double)(int64_t)v.d && std::abs(v.d) < 9e15) return bson_of(Value::integer((int64_t)v.d));
    return bson_of(v);
}

// -------------------------------------------------------------- filters
std::string escape_regex(const std::string &s)
{
    std::string r;
    for (char c : s) {
        if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) r += '\\';
        r += c;
    }
    return r;
}

// Names written into a new document: any valid key, since a collection has no schema. Never an operator.
std::string checked_new_field(const std::string &name)
{
    if (name.empty() || name[0] == '$' || name.find('.') != std::string::npos || name.find('\0') != std::string::npos) fail(ErrKind::Validation, "invalid field name: " + name);
    return name;
}

// Field paths become BSON keys. A key beginning with '$' would be an operator, so it is never accepted.
std::string checked_path(const TableMeta &m, const std::string &path)
{
    if (path.empty()) fail(ErrKind::Validation, "empty field name");
    size_t dot = path.find('.');
    std::string top = path.substr(0, dot);
    if (!m.column(top)) fail(ErrKind::Validation, "unknown field: " + top);
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t e = path.find('.', pos);
        std::string seg = path.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (seg.empty() || seg[0] == '$' || seg.find('\0') != std::string::npos) fail(ErrKind::Validation, "invalid field path: " + path);
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return path;
}

bool is_text_col(const ColumnInfo *c) { return c && (c->cls == TypeClass::Text || c->cls == TypeClass::LongText); }

bsoncxx::document::value regex_expr(const std::string &pattern, bool case_sensitive)
{
    return make_document(kvp("$regex", pattern), kvp("$options", case_sensitive ? "" : "i"));
}

bsoncxx::document::value cond_doc(const TableMeta &m, const Condition &c)
{
    if (!c.well_formed()) fail(ErrKind::Validation, "malformed condition on " + c.column);
    std::string f = checked_path(m, c.column);
    const ColumnInfo *col = m.column(f.substr(0, f.find('.')));
    bb::document d;
    auto text0 = [&] { return c.values[0].display(); };
    switch (c.op) {
    case Op::Equals: d.append(kvp(f, bson_of(c.values[0]).view())); break;
    case Op::NotEquals: d.append(kvp(f, make_document(kvp("$ne", bson_of(c.values[0]).view())))); break;
    case Op::Contains: d.append(kvp(f, regex_expr(escape_regex(text0()), c.case_sensitive))); break;
    case Op::StartsWith: d.append(kvp(f, regex_expr("^" + escape_regex(text0()), c.case_sensitive))); break;
    case Op::EndsWith: d.append(kvp(f, regex_expr(escape_regex(text0()) + "$", c.case_sensitive))); break;
    case Op::NotContains: d.append(kvp(f, make_document(kvp("$not", regex_expr(escape_regex(text0()), c.case_sensitive))))); break;
    case Op::Regex: d.append(kvp(f, regex_expr(text0(), c.case_sensitive))); break;
    case Op::IsEmpty:
        if (is_text_col(col)) {
            bb::array a;
            a.append(make_document(kvp(f, BVal(bt::b_null {}).view())));
            a.append(make_document(kvp(f, "")));
            d.append(kvp("$or", bt::b_array { a.view() }));
        } else d.append(kvp(f, BVal(bt::b_null {}).view()));
        break;
    case Op::NotEmpty: {
        bb::array a;
        a.append(BVal(bt::b_null {}).view());
        if (is_text_col(col)) a.append("");
        bb::document inner;
        inner.append(kvp(std::string("$nin"), bt::b_array { a.view() }));
        d.append(kvp(f, inner.extract()));
        break;
    }
    case Op::Gt: d.append(kvp(f, make_document(kvp("$gt", bson_of(c.values[0]).view())))); break;
    case Op::Lt: d.append(kvp(f, make_document(kvp("$lt", bson_of(c.values[0]).view())))); break;
    case Op::Gte: d.append(kvp(f, make_document(kvp("$gte", bson_of(c.values[0]).view())))); break;
    case Op::Lte: d.append(kvp(f, make_document(kvp("$lte", bson_of(c.values[0]).view())))); break;
    case Op::Between: d.append(kvp(f, make_document(kvp("$gte", bson_of(c.values[0]).view()), kvp("$lte", bson_of(c.values[1]).view())))); break;
    case Op::In:
    case Op::NotIn: {
        if (c.values.size() > 1000) fail(ErrKind::Validation, "too many values in list (max 1000)");
        bb::array a;
        for (auto &v : c.values) a.append(bson_of(v).view());
        bb::document inner;
        inner.append(kvp(std::string(c.op == Op::In ? "$in" : "$nin"), bt::b_array { a.view() }));
        d.append(kvp(f, inner.extract()));
        break;
    }
    case Op::IsNull: d.append(kvp(f, make_document(kvp("$type", "null")))); break;
    case Op::NotNull: d.append(kvp(f, make_document(kvp("$exists", true), kvp("$not", make_document(kvp("$type", "null")))))); break;
    case Op::IsTrue: d.append(kvp(f, true)); break;
    case Op::IsFalse: d.append(kvp(f, false)); break;
    case Op::Exists: d.append(kvp(f, make_document(kvp("$exists", true)))); break;
    case Op::NotExists: d.append(kvp(f, make_document(kvp("$exists", false)))); break;
    }
    return d.extract();
}

bsoncxx::document::value group_doc(const TableMeta &m, const FilterGroup &g)
{
    std::vector<bsoncxx::document::value> parts;
    for (auto &c : g.conds) parts.push_back(cond_doc(m, c));
    for (auto &s : g.groups) {
        auto d = group_doc(m, s);
        if (!d.view().empty()) parts.push_back(std::move(d));
    }
    if (parts.empty()) return make_document();
    if (parts.size() == 1) return std::move(parts[0]);
    bb::array a;
    for (auto &p : parts) a.append(p.view());
    bb::document out;
    out.append(kvp(std::string(g.logic == Logic::And ? "$and" : "$or"), bt::b_array { a.view() }));
    return out.extract();
}

bsoncxx::document::value sort_doc(const TableMeta &m, const std::vector<SortKey> &sort)
{
    bb::document d;
    bool has_id = false;
    for (auto &s : sort) {
        d.append(kvp(checked_path(m, s.column), s.desc ? -1 : 1));
        if (s.column == "_id") has_id = true;
    }
    if (!has_id) d.append(kvp("_id", 1)); // deterministic paging
    return d.extract();
}

std::vector<std::string> column_names(const TableMeta &m, const PageRequest &req)
{
    std::vector<std::string> v;
    if (req.columns.empty())
        for (auto &c : m.columns) v.push_back(c.name);
    else
        for (auto &n : req.columns) {
            if (!m.column(n)) fail(ErrKind::Validation, "unknown field: " + n);
            v.push_back(n);
        }
    return v;
}

bsoncxx::document::value projection_doc(const std::vector<std::string> &names)
{
    bb::document d;
    bool id = false;
    for (auto &n : names) {
        d.append(kvp(n, 1));
        if (n == "_id") id = true;
    }
    if (!id) d.append(kvp("_id", 0));
    return d.extract();
}

std::vector<Value> row_from(const bsoncxx::document::view &doc, const std::vector<std::string> &names)
{
    std::vector<Value> row;
    row.reserve(names.size());
    for (auto &n : names) {
        auto e = doc[n];
        row.push_back(e ? to_value(e) : Value::missing());
    }
    return row;
}

std::string shell_text(const TableMeta &m, const PageRequest &req, bool with_paging)
{
    std::string t = "db." + m.table.name + ".find(" + relaxed_json(group_doc(m, req.filter).view()) + ")";
    t += ".sort(" + relaxed_json(sort_doc(m, req.sort).view()) + ")";
    if (with_paging) t += ".skip(" + std::to_string(req.offset) + ").limit(" + std::to_string(req.limit) + ")";
    return t;
}

// -------------------------------------------------------------- schema discovery
struct FieldStat {
    std::set<type> types;
    size_t max_len = 0;
};

TypeClass class_of(const FieldStat &f, bool &exotic, std::string &native)
{
    std::set<type> t = f.types;
    bool has_null = t.erase(type::k_null) > 0;
    exotic = false;
    auto name_of = [&] {
        std::string s;
        for (auto x : t) s += (s.empty() ? "" : " | ") + std::string(type_name(x));
        return s.empty() ? std::string("null") : s;
    };
    native = name_of();
    (void)has_null;
    if (t.empty()) return TypeClass::Text;
    bool all_num = true, any_double = false, any_dec = false;
    for (auto x : t) {
        if (x == type::k_int32 || x == type::k_int64) continue;
        if (x == type::k_double) { any_double = true; continue; }
        if (x == type::k_decimal128) { any_dec = true; continue; }
        all_num = false;
    }
    if (all_num) return any_dec ? TypeClass::Decimal : any_double ? TypeClass::Float : TypeClass::Integer;
    if (t.size() == 1) {
        switch (*t.begin()) {
        case type::k_string: return f.max_len > 200 ? TypeClass::LongText : TypeClass::Text;
        case type::k_bool: return TypeClass::Bool;
        case type::k_date: return TypeClass::DateTime;
        case type::k_oid: return TypeClass::ObjectId;
        case type::k_document:
        case type::k_array: return TypeClass::Json;
        case type::k_binary: return TypeClass::Binary;
        default: break;
        }
    } else if (std::all_of(t.begin(), t.end(), [](type x) { return x == type::k_document || x == type::k_array; })) {
        return TypeClass::Json;
    }
    exotic = true; // mixed or unrepresentable: shown, not editable
    native = t.size() > 1 ? "mixed (" + name_of() + ")" : name_of();
    return TypeClass::Other;
}

std::string url_encode(const std::string &s)
{
    static const char *H = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
        else {
            r += '%';
            r += H[c >> 4];
            r += H[c & 15];
        }
    }
    return r;
}

std::atomic<unsigned> g_tag { 0 };

class MongoConn : public Connector {
public:
    explicit MongoConn(MongoParams p) : p_(std::move(p)) {}
    ~MongoConn() override { close(); }

    void open()
    {
        instance();
        std::string uri = build_uri();
        try {
            pool_ = std::make_shared<mongocxx::pool>(mongocxx::uri(uri));
            auto c = pool_->acquire();
            (*c)["admin"].run_command(make_document(kvp("ping", 1)));
            auto hello = (*c)["admin"].run_command(make_document(kvp("hello", 1)));
            auto v = hello.view();
            clustered_ = v["setName"] || (v["msg"] && std::string(v["msg"].get_string().value) == "isdbgrid");
            auto bi = (*c)["admin"].run_command(make_document(kvp("buildInfo", 1)));
            if (bi.view()["version"]) version_ = std::string(bi.view()["version"].get_string().value);
        } catch (const std::exception &e) {
            pool_.reset();
            std::string m = e.what();
            if (p_.tls && !p_.trust_server_cert && (m.find("certificate") != std::string::npos || m.find("TLS") != std::string::npos || m.find("SSL") != std::string::npos))
                m += " (TLS is required and the certificate is validated. For a trusted server you may change the TLS options in the connection settings.)";
            translate(std::runtime_error(m), true);
        }
    }

    Capabilities caps() const override
    {
        Capabilities c;
        c.edit = !p_.read_only;
        c.documents = true;
        c.sql = false;
        c.regex = true;
        return c;
    }

    std::string server_info() override { return "MongoDB " + version_ + (clustered_ ? "" : " (standalone)"); }

    void close() override { pool_.reset(); }

    std::vector<ObjectInfo> list_databases() override
    {
        if (!p_.database.empty()) return { { p_.database, ObjectKind::Database } };
        return guard([&] {
            auto c = pool().acquire();
            std::vector<std::string> names;
            for (auto &n : c->list_database_names()) names.push_back(n);
            std::stable_sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
                auto sys = [](const std::string &s) { return s == "admin" || s == "config" || s == "local"; };
                return sys(a) < sys(b);
            });
            std::vector<ObjectInfo> out;
            for (auto &n : names) out.push_back({ n, ObjectKind::Database });
            return out;
        });
    }

    std::vector<ObjectInfo> list_schemas(const std::string &) override { return {}; }

    std::vector<ObjectInfo> list_tables(const std::string &db, const std::string &) override
    {
        return guard([&] {
            auto c = pool().acquire();
            std::vector<ObjectInfo> out;
            auto cur = (*c)[home(db)].list_collections();
            for (auto &d : cur) {
                std::string n(d["name"].get_string().value);
                if (n.rfind("system.", 0) == 0) continue;
                bool view = d["type"] && std::string(d["type"].get_string().value) == "view";
                out.push_back({ n, view ? ObjectKind::View : ObjectKind::Collection });
            }
            std::sort(out.begin(), out.end(), [](const ObjectInfo &a, const ObjectInfo &b) { return a.name < b.name; });
            return out;
        });
    }

    TableMeta table_meta(const TableRef &t) override
    {
        return guard([&] {
            TableMeta m;
            m.table = t;
            m.table.database = home(t.database);
            m.table.schema.clear();
            auto c = pool().acquire();
            auto db = (*c)[m.table.database];
            auto coll = db[t.name];

            for (auto &d : db.list_collections(make_document(kvp("name", t.name))))
                m.is_view = d["type"] && std::string(d["type"].get_string().value) == "view";

            std::vector<std::string> order;
            std::map<std::string, FieldStat> stats;
            mongocxx::options::find fo;
            fo.limit(kSample);
            fo.max_time(std::chrono::seconds(30));
            for (auto &doc : coll.find({}, fo)) {
                for (auto &e : doc) {
                    std::string k(e.key());
                    if (k.empty() || k[0] == '$' || k.find('.') != std::string::npos) continue; // not addressable by path
                    auto it = stats.find(k);
                    if (it == stats.end()) {
                        order.push_back(k);
                        it = stats.emplace(k, FieldStat()).first;
                    }
                    it->second.types.insert(e.type());
                    if (e.type() == type::k_string) it->second.max_len = std::max<size_t>(it->second.max_len, e.get_string().value.size());
                }
            }
            if (!stats.count("_id")) {
                order.insert(order.begin(), "_id");
                stats["_id"].types.insert(type::k_oid);
            } else {
                order.erase(std::find(order.begin(), order.end(), "_id"));
                order.insert(order.begin(), "_id");
            }
            for (auto &k : order) {
                ColumnInfo ci;
                ci.name = k;
                bool exotic = false;
                ci.cls = class_of(stats[k], exotic, ci.native_type);
                ci.generated = exotic;
                if (k == "_id") {
                    ci.pk_position = 1;
                    ci.nullable = false;
                    ci.has_default = true;
                }
                m.columns.push_back(std::move(ci));
            }

            if (!m.is_view) {
                try {
                    m.estimated_rows = coll.estimated_document_count();
                } catch (const std::exception &) {}
                try {
                    for (auto &ix : coll.list_indexes()) {
                        IndexInfo i;
                        i.name = std::string(ix["name"].get_string().value);
                        i.unique = ix["unique"] && ix["unique"].get_bool().value;
                        if (ix["key"])
                            for (auto &k : ix["key"].get_document().value) i.columns.push_back(std::string(k.key()));
                        m.indexes.push_back(std::move(i));
                    }
                } catch (const std::exception &) {}
            }

            if (m.is_view) m.edit = { {}, false, "Views are read-only" };
            else if (p_.read_only) m.edit = { {}, false, "Connection is read-only" };
            else {
                m.edit.key_columns = { "_id" };
                m.edit.editable = true;
            }
            return m;
        });
    }

    Page fetch_page(const TableMeta &m, const PageRequest &req, const CancelPtr &c) override
    {
        return guard([&] {
            auto t0 = Clock::now();
            Page p;
            p.generated = shell_text(m, req, true);
            auto names = column_names(m, req);
            auto filter = group_doc(m, req.filter);
            std::string tag = new_tag();
            auto sort_v = sort_doc(m, req.sort);
            auto proj_v = projection_doc(names);
            mongocxx::options::find fo;
            fo.sort(sort_v.view());
            fo.skip((int64_t)req.offset);
            fo.limit((int64_t)req.limit + 1);
            fo.projection(proj_v.view());
            fo.allow_disk_use(true);
            fo.comment(tag);
            auto client = pool().acquire();
            if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
            HookGuard hg(c, canceller(tag));
            auto cur = (*client)[m.table.database][m.table.name].find(filter.view(), fo);
            for (auto &doc : cur) {
                if (p.rows.size() >= req.limit) {
                    p.has_more = true;
                    break;
                }
                p.rows.push_back(row_from(doc, names));
            }
            p.elapsed_ms = ms_since(t0);
            return p;
        });
    }

    uint64_t count(const TableMeta &m, const FilterGroup &f, const CancelPtr &c) override
    {
        return guard([&] {
            auto filter = group_doc(m, f);
            std::string tag = new_tag();
            mongocxx::pipeline pl;
            pl.match(filter.view());
            pl.count("n");
            mongocxx::options::aggregate ao;
            ao.comment(BVal(bt::b_string { tag }));
            auto client = pool().acquire();
            HookGuard hg(c, canceller(tag));
            uint64_t n = 0;
            for (auto &d : (*client)[m.table.database][m.table.name].aggregate(pl, ao)) {
                auto e = d["n"];
                if (e) n = (uint64_t)(e.type() == type::k_int64 ? e.get_int64().value : e.get_int32().value);
            }
            return n;
        });
    }

    std::vector<Value> distinct_values(const TableMeta &m, const DistinctRequest &r, const CancelPtr &c) override
    {
        return guard([&] {
            std::string f = checked_path(m, r.column);
            const ColumnInfo *ci = m.column(f.substr(0, f.find('.')));
            if (ci->cls == TypeClass::Binary) fail(ErrKind::Validation, "distinct values are not available for binary fields");
            uint32_t lim = std::clamp<uint32_t>(r.limit, 1, 1000);
            std::string tag = new_tag();
            mongocxx::pipeline pl;
            if (!r.search.empty()) pl.match(make_document(kvp(f, regex_expr(escape_regex(r.search), false))).view());
            pl.group(make_document(kvp("_id", "$" + f)).view());
            pl.sort(make_document(kvp("_id", 1)).view());
            pl.limit((int32_t)lim);
            mongocxx::options::aggregate ao;
            ao.comment(BVal(bt::b_string { tag }));
            ao.allow_disk_use(true);
            auto client = pool().acquire();
            HookGuard hg(c, canceller(tag));
            std::vector<Value> out;
            for (auto &d : (*client)[m.table.database][m.table.name].aggregate(pl, ao)) out.push_back(to_value(d["_id"]));
            return out;
        });
    }

    uint64_t stream_rows(const TableMeta &m, const PageRequest &req, size_t batch, const RowBatchFn &fn, const CancelPtr &c) override
    {
        return guard([&] {
            auto names = column_names(m, req);
            auto filter = group_doc(m, req.filter);
            std::string tag = new_tag();
            auto sort_v = sort_doc(m, req.sort);
            auto proj_v = projection_doc(names);
            mongocxx::options::find fo;
            fo.sort(sort_v.view());
            fo.projection(proj_v.view());
            fo.allow_disk_use(true);
            fo.batch_size((int32_t)std::min<size_t>(std::max<size_t>(batch, 1), 10000));
            fo.comment(tag);
            auto client = pool().acquire();
            HookGuard hg(c, canceller(tag));
            auto cur = (*client)[m.table.database][m.table.name].find(filter.view(), fo);
            std::vector<std::vector<Value>> buf;
            uint64_t total = 0;
            for (auto &doc : cur) {
                buf.push_back(row_from(doc, names));
                ++total;
                if (buf.size() >= batch) {
                    bool more = fn(buf);
                    buf.clear();
                    if (!more) return total; // leaving the scope closes the cursor on the server
                }
            }
            if (!buf.empty()) fn(buf);
            return total;
        });
    }

    ApplyReport apply_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        if (p_.read_only) throw DbError(ErrKind::ReadOnly, "Connection is read-only");
        if (!m.edit.editable) throw DbError(ErrKind::Validation, m.edit.blocked_reason);
        return guard([&] {
            struct Op {
                Change::Type type;
                bsoncxx::document::value filter = make_document();
                bsoncxx::document::value body = make_document();
                bool empty = false;
            };
            std::vector<Op> ops;
            for (auto &ch : changes) {
                Op op { ch.type };
                switch (ch.type) {
                case Change::Type::Insert: {
                    bb::document d;
                    for (auto &kv : ch.values) {
                        checked_new_field(kv.first);
                        if (kv.second.kind == Value::Kind::Default || kv.second.kind == Value::Kind::Missing || kv.second.is_null()) continue;
                        d.append(kvp(kv.first, bson_of(kv.second).view()));
                    }
                    op.body = d.extract();
                    break;
                }
                case Change::Type::Update: {
                    if (ch.set.empty()) continue;
                    bb::document f, set, unset;
                    bool any_set = false, any_unset = false;
                    key_into(m, ch.key, f);
                    for (auto &s : ch.set) {
                        checked_path(m, s.column);
                        if (s.column == "_id") fail(ErrKind::Validation, "_id cannot be changed in MongoDB");
                        if (s.original.kind == Value::Kind::Missing) f.append(kvp(s.column, make_document(kvp("$exists", false))));
                        else if (s.original.kind != Value::Kind::Default) f.append(kvp(s.column, bson_of(s.original).view()));
                        if (s.value.kind == Value::Kind::Missing) {
                            unset.append(kvp(s.column, ""));
                            any_unset = true;
                        } else {
                            set.append(kvp(s.column, bson_for_set(s.value, s.original).view()));
                            any_set = true;
                        }
                    }
                    bb::document body;
                    if (any_set) body.append(kvp("$set", set.extract()));
                    if (any_unset) body.append(kvp("$unset", unset.extract()));
                    op.filter = f.extract();
                    op.body = body.extract();
                    break;
                }
                case Change::Type::Delete: {
                    bb::document f;
                    key_into(m, ch.key, f);
                    op.filter = f.extract();
                    break;
                }
                }
                ops.push_back(std::move(op));
            }

            auto client = pool().acquire();
            auto coll = (*client)[m.table.database][m.table.name];
            ApplyReport rep;
            auto run_one = [&](Op &op, const mongocxx::client_session *session, size_t done) {
                auto conflict = [&](const char *msg) {
                    std::string t = msg;
                    if (!clustered_ && done) t += " (" + std::to_string(done) + " earlier change(s) were already applied: a standalone MongoDB server has no transactions)";
                    throw DbError(ErrKind::Conflict, t);
                };
                if (op.type == Change::Type::Insert) {
                    if (session) coll.insert_one(*session, op.body.view());
                    else coll.insert_one(op.body.view());
                    ++rep.inserted;
                } else if (op.type == Change::Type::Update) {
                    auto r = session ? coll.update_one(*session, op.filter.view(), op.body.view()) : coll.update_one(op.filter.view(), op.body.view());
                    if (!r || r->matched_count() != 1) conflict("A document was modified or deleted by someone else since it was loaded");
                    ++rep.updated;
                } else {
                    auto r = session ? coll.delete_one(*session, op.filter.view()) : coll.delete_one(op.filter.view());
                    if (!r || r->deleted_count() != 1) conflict("A document to delete no longer exists or was changed by someone else");
                    ++rep.deleted;
                }
            };

            if (clustered_) {
                auto session = client->start_session();
                session.start_transaction();
                try {
                    size_t n = 0;
                    for (auto &op : ops) run_one(op, &session, n++);
                    session.commit_transaction();
                } catch (...) {
                    try {
                        session.abort_transaction();
                    } catch (...) {}
                    throw;
                }
            } else {
                // No transactions here: prove every update/delete still matches before touching anything, then apply in order.
                for (auto &op : ops) {
                    if (op.type == Change::Type::Insert) continue;
                    if (coll.count_documents(op.filter.view()) != 1)
                        throw DbError(ErrKind::Conflict, op.type == Change::Type::Delete ? "A document to delete no longer exists or was changed by someone else"
                                                                                           : "A document was modified or deleted by someone else since it was loaded");
                }
                size_t n = 0;
                for (auto &op : ops) run_one(op, nullptr, n++);
            }
            return rep;
        });
    }

    std::string preview_query(const TableMeta &m, const PageRequest &req) override { return guard([&] { return shell_text(m, req, true); }); }

    std::string preview_changes(const TableMeta &m, const std::vector<Change> &changes) override
    {
        return guard([&] {
            std::string out;
            std::string coll = "db." + m.table.name;
            for (auto &ch : changes) {
                if (ch.type == Change::Type::Insert) {
                    bb::document d;
                    for (auto &kv : ch.values)
                        if (kv.second.kind != Value::Kind::Default && kv.second.kind != Value::Kind::Missing && !kv.second.is_null()) d.append(kvp(checked_new_field(kv.first), bson_of(kv.second).view()));
                    out += coll + ".insertOne(" + relaxed_json(d.extract().view()) + ");\n";
                } else {
                    bb::document f;
                    key_into(m, ch.key, f);
                    if (ch.type == Change::Type::Delete) {
                        out += coll + ".deleteOne(" + relaxed_json(f.extract().view()) + ");\n";
                    } else {
                        bb::document set;
                        for (auto &s : ch.set) {
                            checked_path(m, s.column);
                            set.append(kvp(s.column, bson_for_set(s.value, s.original).view()));
                        }
                        out += coll + ".updateOne(" + relaxed_json(f.extract().view()) + ", { $set: " + relaxed_json(set.extract().view()) + " });\n";
                    }
                }
            }
            return out;
        });
    }

    // The editor takes one database command as JSON, e.g. {"find":"people","filter":{"age":{"$gt":30}},"limit":20}.
    // An optional "$db" key selects the database.
    QueryOutput execute(const std::string &text, size_t max_rows, const CancelPtr &c) override
    {
        return guard([&] {
            auto t0 = Clock::now();
            bsoncxx::document::value cmd = bsoncxx::from_json(text.empty() ? "{}" : text);
            auto view = cmd.view();
            if (view.empty()) fail(ErrKind::Validation, "Write a database command as JSON, for example {\"ping\":1} or {\"find\":\"collection\",\"filter\":{},\"limit\":20}");
            std::string first(view.begin()->key());
            if (p_.read_only) {
                static const std::set<std::string> ok = { "find", "count", "distinct", "listCollections", "listIndexes", "listDatabases", "ping", "buildInfo", "hello", "isMaster", "dbStats", "collStats", "serverStatus" };
                bool allowed = ok.count(first) > 0;
                if (first == "aggregate") {
                    std::string j = text;
                    allowed = j.find("$out") == std::string::npos && j.find("$merge") == std::string::npos;
                }
                if (!allowed) throw DbError(ErrKind::ReadOnly, "Connection is read-only: command '" + first + "' is not allowed");
            }
            std::string db = home("");
            std::string tag = new_tag();
            bb::document nd;
            bool has_comment = false;
            for (auto &e : view) {
                std::string k(e.key());
                if (k == "$db") {
                    db = std::string(e.get_string().value);
                    continue;
                }
                if (k == "comment") has_comment = true;
                nd.append(kvp(k, e.get_value()));
            }
            if (!has_comment) nd.append(kvp("comment", tag));
            if (db.empty()) db = "admin";
            auto run = nd.extract();
            auto client = pool().acquire();
            if (c && c->cancelled()) throw DbError(ErrKind::Cancelled, "operation cancelled");
            HookGuard hg(c, canceller(has_comment ? std::string() : tag));
            auto res = (*client)[db].run_command(run.view());
            auto rv = res.view();

            QueryOutput out;
            auto cursor = rv["cursor"];
            if (cursor && cursor.type() == type::k_document && cursor.get_document().value["firstBatch"]) {
                auto cv = cursor.get_document().value;
                std::vector<std::string> cols;
                std::set<std::string> seen;
                std::vector<bsoncxx::document::view> docs;
                for (auto &d : cv["firstBatch"].get_array().value) {
                    if (d.type() != type::k_document) continue;
                    if (docs.size() >= max_rows) {
                        out.truncated = true;
                        break;
                    }
                    docs.push_back(d.get_document().value);
                    for (auto &e : docs.back())
                        if (seen.insert(std::string(e.key())).second) cols.push_back(std::string(e.key()));
                }
                auto id = cv["id"];
                if (id && id.type() == type::k_int64 && id.get_int64().value != 0) out.truncated = true; // more results exist on the server
                out.columns = cols;
                for (auto &d : docs) out.rows.push_back(row_from(d, cols));
            } else {
                std::vector<std::string> cols;
                for (auto &e : rv) cols.push_back(std::string(e.key()));
                out.columns = cols;
                out.rows.push_back(row_from(rv, cols));
            }
            out.elapsed_ms = ms_since(t0);
            out.message = std::to_string(out.rows.size()) + " document(s)" + (out.truncated ? " (truncated)" : "");
            return out;
        });
    }

private:
    static constexpr int kSample = 200; // fields are discovered from the first documents of a collection

    mongocxx::pool &pool()
    {
        if (!pool_) throw DbError(ErrKind::Connection, "connection closed");
        return *pool_;
    }

    std::string home(const std::string &db) const { return db.empty() ? p_.database : db; }

    static std::string new_tag() { return "dbpro-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++g_tag); }

    // Cancels the running command by finding it through its comment tag and killing it from another pooled connection.
    std::function<void()> canceller(const std::string &tag)
    {
        auto pool = pool_;
        return [pool, tag] {
            if (!pool || tag.empty()) return;
            try {
                auto c = pool->acquire();
                auto admin = (*c)["admin"];
                bb::array pl;
                pl.append(make_document(kvp("$currentOp", make_document())));
                pl.append(make_document(kvp("$match", make_document(kvp("command.comment", tag)))));
                auto r = admin.run_command(make_document(kvp("aggregate", 1), kvp("pipeline", bt::b_array { pl.view() }), kvp("cursor", make_document())));
                for (auto &op : r.view()["cursor"]["firstBatch"].get_array().value) {
                    auto opid = op["opid"].get_value();
                    admin.run_command(make_document(kvp("killOp", 1), kvp("op", opid)));
                }
            } catch (const std::exception &) {}
        };
    }

    static void key_into(const TableMeta &m, const std::vector<std::pair<std::string, Value>> &key, bb::document &f)
    {
        if (key.empty()) fail(ErrKind::Validation, "document cannot be identified: no key");
        for (auto &kv : key) {
            checked_path(m, kv.first);
            if (kv.second.is_null() || kv.second.kind == Value::Kind::Missing) fail(ErrKind::Validation, "document cannot be identified: key " + kv.first + " is empty");
            f.append(kvp(kv.first, bson_of(kv.second).view()));
        }
    }

    std::string build_uri() const
    {
        std::string host = p_.host;
        std::string scheme = "mongodb://";
        if (host.rfind("mongodb+srv://", 0) == 0) {
            scheme = "mongodb+srv://";
            host = host.substr(14);
        } else if (host.rfind("mongodb://", 0) == 0) {
            host = host.substr(10);
        }
        if (host.empty() || host.find_first_of("/@?# \t") != std::string::npos) fail(ErrKind::Validation, "invalid host name");
        if (scheme == "mongodb://" && host.find(':') == std::string::npos && host.find(',') == std::string::npos && p_.port) host += ":" + std::to_string(p_.port);
        std::string u = scheme;
        if (!p_.user.empty()) u += url_encode(p_.user) + ":" + url_encode(p_.password) + "@";
        u += host + "/";
        u += "?appName=Db%20Pro&serverSelectionTimeoutMS=8000&connectTimeoutMS=8000";
        u += p_.tls ? "&tls=true" : "&tls=false";
        if (p_.tls && p_.trust_server_cert) u += "&tlsAllowInvalidCertificates=true";
        if (!p_.user.empty()) u += "&authSource=admin";
        return u;
    }

    MongoParams p_;
    std::shared_ptr<mongocxx::pool> pool_;
    bool clustered_ = false;
    std::string version_;
};

} // namespace

std::unique_ptr<Connector> connect_mongodb(const MongoParams &p)
{
    auto c = std::make_unique<MongoConn>(p);
    c->open(); // fail here, with the server's message, not on first use
    return c;
}

} // namespace dbp
