// Db Pro core model: values, metadata, typed filters, changes and the connector contract.
// Nothing in this header talks to a database.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace dbp {

// ---------------------------------------------------------------- errors
enum class ErrKind { Connection, Query, Cancelled, Timeout, Unsupported, ReadOnly, Validation, Conflict, Other };

struct DbError : std::runtime_error {
    ErrKind kind;
    DbError(ErrKind k, const std::string &m) : std::runtime_error(m), kind(k) {}
};

// ---------------------------------------------------------------- reconnection
constexpr int kReconnectAttempts = 5;

// Wait before reconnect attempt `attempt` (0-based): 1, 2, 4, 8, 16 seconds.
inline int reconnect_delay_seconds(int attempt)
{
    if (attempt < 0) attempt = 0;
    if (attempt > 4) attempt = 4;
    return 1 << attempt;
}

// Only connection-level failures justify reconnecting; query, conflict and validation errors never do.
inline bool is_connection_lost(ErrKind k) { return k == ErrKind::Connection; }

// ---------------------------------------------------------------- cancellation
// A token is shared between the UI thread and the worker running a query.
// The connector installs an engine-specific hook (sqlite3_interrupt, PQcancel, ...) while it blocks.
class CancelToken {
public:
    void cancel()
    {
        flag_ = true;
        std::function<void()> h;
        {
            std::lock_guard<std::mutex> g(m_);
            h = hook_;
        }
        if (h)
            h();
    }
    // Deadline reached: same effect as cancel(), but callers can tell the difference.
    void expire()
    {
        timed_out_ = true;
        cancel();
    }
    bool cancelled() const { return flag_; }
    bool timed_out() const { return timed_out_; }
    void set_hook(std::function<void()> h)
    {
        bool fire;
        {
            std::lock_guard<std::mutex> g(m_);
            hook_ = h;
            fire = flag_;
        }
        if (fire && h)
            h();
    }
    void clear_hook()
    {
        std::lock_guard<std::mutex> g(m_);
        hook_ = nullptr;
    }

private:
    std::atomic<bool> flag_ { false };
    std::atomic<bool> timed_out_ { false };
    std::mutex m_;
    std::function<void()> hook_;
};
using CancelPtr = std::shared_ptr<CancelToken>;

inline void throw_if_cancelled(const CancelPtr &token)
{
    if (token && token->cancelled()) throw DbError(token->timed_out() ? ErrKind::Timeout : ErrKind::Cancelled, token->timed_out() ? "the operation exceeded the time limit" : "operation cancelled");
}

// Expires a token after `seconds` unless destroyed first. seconds <= 0 or a null token = no limit.
// Works with every connector because it reuses the token's engine-specific cancel hook.
class Deadline {
public:
    Deadline(const CancelPtr &token, int seconds)
    {
        if (!token || seconds <= 0) return;
        st_ = std::make_shared<State>();
        std::thread([st = st_, token, seconds] {
            std::unique_lock<std::mutex> lk(st->m);
            if (!st->cv.wait_for(lk, std::chrono::seconds(seconds), [&] { return st->done; })) {
                lk.unlock();
                token->expire();
            }
        }).detach();
    }
    ~Deadline()
    {
        if (!st_) return;
        {
            std::lock_guard<std::mutex> g(st_->m);
            st_->done = true;
        }
        st_->cv.notify_all();
    }
    Deadline(const Deadline &) = delete;
    Deadline &operator=(const Deadline &) = delete;

private:
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
    };
    std::shared_ptr<State> st_;
};

struct HookGuard {
    CancelPtr t;
    HookGuard(const CancelPtr &tok, std::function<void()> h) : t(tok) { if (t) t->set_hook(std::move(h)); }
    ~HookGuard() { if (t) t->clear_hook(); }
    HookGuard(const HookGuard &) = delete;
    HookGuard &operator=(const HookGuard &) = delete;
};

enum class TypeClass { Integer, Decimal, Float, Text, LongText, Bool, Date, Time, DateTime, Uuid, Json, Binary, ObjectId, Other };

// ---------------------------------------------------------------- values
// NULL, a missing document field, DEFAULT, "" , 0 and false are all different things.
struct Value {
    enum class Kind : uint8_t { Null, Missing, Default, Bool, Int, Float, Decimal, Text, Bytes, Temporal, Uuid, Json, ObjectId };
    Kind kind = Kind::Null;
    int64_t i = 0;
    double d = 0;
    std::string s; // text payload (Decimal keeps its exact text; Bytes holds raw bytes)

    static Value null() { return {}; }
    static Value missing() { Value v; v.kind = Kind::Missing; return v; }
    static Value dflt() { Value v; v.kind = Kind::Default; return v; }
    static Value boolean(bool b) { Value v; v.kind = Kind::Bool; v.i = b; return v; }
    static Value integer(int64_t n) { Value v; v.kind = Kind::Int; v.i = n; return v; }
    static Value real(double x) { Value v; v.kind = Kind::Float; v.d = x; return v; }
    static Value text(std::string t) { Value v; v.kind = Kind::Text; v.s = std::move(t); return v; }
    static Value of(Kind k, std::string t) { Value v; v.kind = k; v.s = std::move(t); return v; }
    // Wrap server-rendered text in the variant matching the column class.
    static Value from_text_for(TypeClass c, std::string t);

    bool is_null() const { return kind == Kind::Null; }
    bool operator==(const Value &o) const { return kind == o.kind && i == o.i && d == o.d && s == o.s; }
    bool operator!=(const Value &o) const { return !(*this == o); }

    // Text shown in a grid cell. NULL is never shown as an empty string.
    std::string display() const;
    // Text placed in the in-cell editor.
    std::string edit_text() const;
};

std::string hex_preview(const std::string &bytes, size_t max = 24);

// ---------------------------------------------------------------- metadata
inline bool is_numeric(TypeClass c) { return c == TypeClass::Integer || c == TypeClass::Decimal || c == TypeClass::Float; }
inline bool is_textual_class(TypeClass c) { return c == TypeClass::Text || c == TypeClass::LongText || c == TypeClass::Other; }
inline bool is_temporal(TypeClass c) { return c == TypeClass::Date || c == TypeClass::Time || c == TypeClass::DateTime; }

struct ColumnInfo {
    std::string name;
    std::string native_type;
    TypeClass cls = TypeClass::Text;
    bool nullable = true;
    int pk_position = 0; // 1-based, 0 = not part of the primary key
    std::string foreign_key; // "table.column" when known
    bool has_default = false;
    bool identity = false;
    bool generated = false;

    bool is_pk() const { return pk_position > 0; }
    // Columns the server computes or that the grid cannot represent as text.
    bool read_only() const { return generated || cls == TypeClass::Binary; }
};

struct IndexInfo {
    std::string name;
    std::vector<std::string> columns;
    bool unique = false;
};

struct TableRef {
    std::string database, schema, name;
    std::string label() const
    {
        std::string r;
        if (!database.empty()) r += database + ".";
        if (!schema.empty()) r += schema + ".";
        return r + name;
    }
    bool operator==(const TableRef &o) const { return database == o.database && schema == o.schema && name == o.name; }
};

struct EditInfo {
    std::vector<std::string> key_columns;
    bool editable = false;
    std::string blocked_reason;
};

struct TableMeta {
    TableRef table;
    std::vector<ColumnInfo> columns;
    std::vector<IndexInfo> indexes;
    EditInfo edit;
    int64_t estimated_rows = -1; // -1 = unknown; always displayed as "≈"
    bool is_view = false;

    int column_index(const std::string &n) const
    {
        for (size_t k = 0; k < columns.size(); ++k)
            if (columns[k].name == n) return (int)k;
        return -1;
    }
    const ColumnInfo *column(const std::string &n) const
    {
        int k = column_index(n);
        return k < 0 ? nullptr : &columns[k];
    }
};

enum class ObjectKind { Database, Schema, Table, View, Collection };

struct ObjectInfo {
    std::string name;
    ObjectKind kind = ObjectKind::Table;
};

// ---------------------------------------------------------------- filters
enum class Op {
    Contains, NotContains, Equals, NotEquals, StartsWith, EndsWith, IsEmpty, NotEmpty, Regex,
    Gt, Lt, Gte, Lte, Between, In, NotIn,
    IsNull, NotNull, IsTrue, IsFalse, Exists, NotExists
};

// Expected number of values: >=0 fixed, -1 = one or more.
int op_arity(Op op);
const char *op_label(Op op);

struct Condition {
    std::string column; // column name, or dotted field path for documents
    Op op = Op::Contains;
    std::vector<Value> values;
    bool case_sensitive = false;
    bool well_formed() const
    {
        int a = op_arity(op);
        return a >= 0 ? (int)values.size() == a : !values.empty();
    }
};

enum class Logic { And, Or };

struct FilterGroup {
    Logic logic = Logic::And;
    std::vector<Condition> conds;
    std::vector<FilterGroup> groups;

    bool empty() const { return conds.empty() && groups.empty(); }
    int count() const
    {
        int n = (int)conds.size();
        for (auto &g : groups) n += g.count();
        return n;
    }
    const Condition *for_column(const std::string &c) const
    {
        for (auto &x : conds)
            if (x.column == c) return &x;
        return nullptr;
    }
    // Replace the top-level conditions for a column; an empty list clears it.
    void set_column(const std::string &c, const std::vector<Condition> &list)
    {
        for (size_t k = 0; k < conds.size();)
            if (conds[k].column == c) conds.erase(conds.begin() + k); else ++k;
        for (auto &x : list) conds.push_back(x);
    }
};

struct SortKey {
    std::string column;
    bool desc = false;
};

// ---------------------------------------------------------------- requests / results
struct PageRequest {
    std::vector<std::string> columns; // empty = all
    FilterGroup filter;
    std::vector<SortKey> sort;
    uint64_t offset = 0;
    uint32_t limit = 100;
};

struct Page {
    std::vector<std::vector<Value>> rows;
    bool has_more = false;
    uint64_t elapsed_ms = 0;
    std::string generated; // SQL text or BSON for the "show query" panel
    std::vector<ColumnInfo> columns; // actual returned columns (documents discover fields)
};

struct DistinctRequest {
    std::string column;
    std::string search;
    uint32_t limit = 200; // hard cap, never an unbounded DISTINCT
};

struct QueryOutput {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    int64_t affected = -1;
    uint64_t elapsed_ms = 0;
    bool truncated = false;
    std::string message;
};

struct Change {
    enum class Type { Insert, Update, Delete } type = Type::Update;
    std::vector<std::pair<std::string, Value>> key; // identifies the row (never a visual position)
    struct Set {
        std::string column;
        Value value;
        Value original; // drives conflict detection
    };
    std::vector<Set> set;                                  // Update
    std::vector<std::pair<std::string, Value>> values;     // Insert
};

struct ApplyReport {
    int inserted = 0, updated = 0, deleted = 0;
};

struct Capabilities {
    bool transactions = false;
    bool edit = false;
    bool regex = false;
    bool schemas = false;
    bool sql = false;
    bool documents = false;
    bool default_keyword = false;
    bool activity = false;     // activity() works: live sessions, blocked sessions and server counters
    bool kill_session = false; // kill_session() works (and the profile is not read-only)
};

// ---------------------------------------------------------------- live server activity
// One sample of what the server is doing right now. Read-only; produced by Connector::activity().
struct Metric {
    std::string key;   // stable id: "sessions", "active" and "blocked" are understood by the UI; others are engine specific
    std::string label; // English label; the UI translates known keys
    double value = 0;
    bool counter = false; // cumulative counter: the UI shows the rate per second between two samples
    std::string unit;     // "" (plain number) or "B" (bytes: the UI formats KB/MB/GB)
};

struct Activity {
    std::vector<Metric> metrics;
    std::vector<std::string> columns; // column keys; the UI translates "mon.col.<key>"
    std::vector<std::vector<Value>> rows; // one row per session / operation
    int id_col = -1;      // column holding the identifier kill_session() needs
    int blocked_col = -1; // column listing the ids this session waits for ("" = not blocked, "?" = blocked by an unknown session)
    int query_col = -1;   // column holding the statement text (shown in full for the selected row)
};

// ---------------------------------------------------------------- connector contract
using RowBatchFn = std::function<bool(std::vector<std::vector<Value>> &)>; // return false to stop

class Connector {
public:
    virtual ~Connector() = default;
    virtual Capabilities caps() const = 0;
    virtual std::string server_info() = 0;
    virtual void close() = 0;

    virtual std::vector<ObjectInfo> list_databases() = 0;
    virtual std::vector<ObjectInfo> list_schemas(const std::string &database) = 0;
    virtual std::vector<ObjectInfo> list_tables(const std::string &database, const std::string &schema) = 0;
    virtual TableMeta table_meta(const TableRef &t) = 0;

    virtual Page fetch_page(const TableMeta &m, const PageRequest &req, const CancelPtr &c) = 0;
    virtual uint64_t count(const TableMeta &m, const FilterGroup &f, const CancelPtr &c) = 0;
    virtual std::vector<Value> distinct_values(const TableMeta &m, const DistinctRequest &r, const CancelPtr &c) = 0;
    // Streams every matching row (no limit). Returns rows delivered.
    virtual uint64_t stream_rows(const TableMeta &m, const PageRequest &req, size_t batch, const RowBatchFn &fn, const CancelPtr &c) = 0;
    // All-or-nothing: throws DbError(Conflict) when a row changed on the server.
    virtual ApplyReport apply_changes(const TableMeta &m, const std::vector<Change> &changes) = 0;
    virtual std::string preview_query(const TableMeta &m, const PageRequest &req) = 0;
    virtual std::string preview_changes(const TableMeta &m, const std::vector<Change> &changes) = 0;

    virtual QueryOutput execute(const std::string &text, size_t max_rows, const CancelPtr &c) = 0;

    virtual void begin() { throw DbError(ErrKind::Unsupported, "transactions"); }
    virtual void commit() { throw DbError(ErrKind::Unsupported, "transactions"); }
    virtual void rollback() { throw DbError(ErrKind::Unsupported, "transactions"); }
    virtual bool in_transaction() const { return false; }

    // Live monitoring. Uses its own connection so a long query on the main one never blocks the monitor.
    virtual Activity activity() { throw DbError(ErrKind::Unsupported, "activity monitor"); }
    // Terminates a session (id as listed in Activity::id_col). Refuses Db Pro's own sessions and read-only profiles.
    virtual void kill_session(const std::string &) { throw DbError(ErrKind::Unsupported, "kill session"); }
};

} // namespace dbp
