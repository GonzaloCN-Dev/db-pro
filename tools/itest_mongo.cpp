// Integration test of the MongoDB driver against a REAL server (throwaway instance). Not part of ctest.
//   DBPRO_DRIVERS_DIR=build\drivers  ITEST_HOST ITEST_PORT ITEST_DB [ITEST_USER ITEST_PASS]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "core/core.h"
#include "drivers/driver_manager.h"

using namespace dbp;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what) \
    do { \
        if (cond) { ++g_pass; std::printf("  ok    %s\n", what); } \
        else { ++g_fail; std::printf("  FAIL  %s   (line %d)\n", what, __LINE__); } \
    } while (0)

static std::string env(const char *k, const char *d = "")
{
    const char *v = std::getenv(k);
    return v ? v : d;
}

static bool throws(ErrKind want, const std::function<void()> &f, std::string *msg = nullptr)
{
    try {
        f();
    } catch (const DbError &e) {
        if (msg) *msg = e.what();
        return e.kind == want;
    } catch (const std::exception &e) {
        if (msg) *msg = e.what();
    }
    return false;
}

static Condition cond(const char *col, Op op, std::vector<Value> v = {})
{
    Condition c;
    c.column = col;
    c.op = op;
    c.values = std::move(v);
    return c;
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Profile p;
    p.engine = "mongodb";
    p.host = env("ITEST_HOST", "127.0.0.1");
    p.port = std::atoi(env("ITEST_PORT", "27017").c_str());
    p.user = env("ITEST_USER");
    p.database = env("ITEST_DB", "dbpro_it");
    p.tls = false;
    std::string pass = env("ITEST_PASS");
    std::unique_ptr<Connector> c;
    try {
        c = drivers::connect(p, pass);
    } catch (const DbError &e) {
        std::printf("CONNECT FAILED: %s\n", e.what());
        return 2;
    }
    std::printf("server: %s\n", c->server_info().c_str());
    Capabilities caps = c->caps();
    CHECK(caps.documents && !caps.sql, "declares document capabilities");

    const std::string cname = "dbpro_it_people";
    TableRef ref { p.database, "", cname };
    auto cmd = [&](const std::string &json) { return c->execute(json, 100, nullptr); };
    try { cmd("{\"drop\":\"" + cname + "\"}"); } catch (...) {}

    std::printf("[insert 5000 documents through apply_changes]\n");
    cmd("{\"create\":\"" + cname + "\"}");
    TableMeta m0 = c->table_meta(ref);
    const char *countries[] = { "Chile", "Argentina", "Peru", "Colombia", "Uruguay" };
    for (int base = 0; base < 5000; base += 500) {
        std::vector<Change> chg;
        for (int i = base + 1; i <= base + 500; ++i) {
            Change ch;
            ch.type = Change::Type::Insert;
            ch.values = { { "_id", Value::integer(i) }, { "name", Value::text("Cliente " + std::to_string(i)) }, { "country", Value::text(countries[i % 5]) },
                          { "age", Value::integer(18 + i % 60) }, { "active", Value::boolean(i % 2 == 0) } };
            if (i % 10 != 0) ch.values.push_back({ "note", Value::text("n" + std::to_string(i)) });
            if (i % 100 == 0) ch.values.push_back({ "address", Value::of(Value::Kind::Json, "{\"city\":\"Santiago\",\"zip\":" + std::to_string(i) + "}") });
            chg.push_back(std::move(ch));
        }
        try {
            c->apply_changes(m0, chg);
        } catch (const DbError &e) {
            std::printf("  insert failed: %s\n", e.what());
            ++g_fail;
            break;
        }
    }

    std::printf("[metadata]\n");
    bool fdb = false, fcol = false;
    for (auto &d : c->list_databases()) fdb |= d.name == p.database;
    CHECK(fdb, "list_databases contains the database");
    for (auto &t : c->list_tables(p.database, "")) fcol |= t.name == cname && t.kind == ObjectKind::Collection;
    CHECK(fcol, "list_tables contains the collection");
    TableMeta m = c->table_meta(ref);
    CHECK(m.columns.size() >= 6 && m.columns[0].name == "_id", "fields discovered by sampling, _id first");
    CHECK(m.edit.editable && m.edit.key_columns.size() == 1 && m.edit.key_columns[0] == "_id", "_id is the row identity");
    const ColumnInfo *age = m.column("age"), *addr = m.column("address"), *act = m.column("active");
    CHECK(age && age->cls == TypeClass::Integer, "age discovered as integer");
    CHECK(act && act->cls == TypeClass::Bool, "active discovered as bool");
    CHECK(addr && addr->cls == TypeClass::Json, "nested object discovered as JSON");
    CHECK(m.estimated_rows == 5000, "estimated count from collection metadata");

    std::printf("[paging, filters, sort]\n");
    PageRequest req;
    req.limit = 100;
    Page pg = c->fetch_page(m, req, nullptr);
    CHECK(pg.rows.size() == 100 && pg.has_more, "first page: 100 documents and has_more");
    CHECK(pg.rows[0][0].kind == Value::Kind::Int && pg.rows[0][0].i == 1, "ordered by _id");
    CHECK(!pg.generated.empty(), "generated query text is available");
    req.offset = 4950;
    pg = c->fetch_page(m, req, nullptr);
    CHECK(pg.rows.size() == 50 && !pg.has_more, "last page: 50 documents");
    int ni = m.column_index("note");
    CHECK(ni >= 0 && pg.rows[9][ni].kind == Value::Kind::Missing, "absent field is Missing, not NULL");

    FilterGroup fg;
    fg.conds = { cond("country", Op::Equals, { Value::text("Chile") }) };
    CHECK(c->count(m, fg, nullptr) == 1000, "count country = Chile is 1000");
    fg.conds.push_back(cond("age", Op::Gte, { Value::integer(50) }));
    uint64_t both = c->count(m, fg, nullptr);
    CHECK(both > 0 && both < 1000, "combined filters (AND)");
    std::printf("        (combined = %llu)\n", (unsigned long long)both);
    fg.conds = { cond("note", Op::NotExists) };
    CHECK(c->count(m, fg, nullptr) == 500, "NotExists finds the 500 documents without 'note'");
    fg.conds = { cond("note", Op::Exists) };
    CHECK(c->count(m, fg, nullptr) == 4500, "Exists");
    fg.conds = { cond("_id", Op::Between, { Value::integer(10), Value::integer(19) }) };
    CHECK(c->count(m, fg, nullptr) == 10, "BETWEEN 10 and 19");
    fg.conds = { cond("_id", Op::In, { Value::integer(1), Value::integer(2), Value::integer(3) }) };
    CHECK(c->count(m, fg, nullptr) == 3, "IN (1,2,3)");
    fg.conds = { cond("country", Op::Contains, { Value::text("CHILE") }) };
    CHECK(c->count(m, fg, nullptr) == 1000, "case-insensitive contains");
    fg.conds[0].case_sensitive = true;
    CHECK(c->count(m, fg, nullptr) == 0, "case-sensitive contains with the wrong case finds nothing");
    fg.conds = { cond("name", Op::Contains, { Value::text(".*") }) };
    CHECK(c->count(m, fg, nullptr) == 0, "regex metacharacters in 'contains' are literal");
    fg.conds = { cond("country", Op::Regex, { Value::text("^(Chile|Peru)$") }) };
    CHECK(c->count(m, fg, nullptr) == 2000, "regex ^(Chile|Peru)$");
    fg.conds = { cond("active", Op::IsTrue) };
    CHECK(c->count(m, fg, nullptr) == 2500, "boolean true");
    fg.conds = { cond("address.zip", Op::Equals, { Value::integer(200) }) };
    CHECK(c->count(m, fg, nullptr) == 1, "filter on a nested path (address.zip = 200)");
    fg.conds = { cond("$where", Op::Equals, { Value::text("1") }) };
    CHECK(throws(ErrKind::Validation, [&] { c->count(m, fg, nullptr); }), "an operator-looking field name is rejected");
    fg.conds = { cond("name", Op::Equals, { Value::text("{\"$ne\":null}") }) };
    CHECK(c->count(m, fg, nullptr) == 0, "operator-looking VALUE is only text (no injection)");
    fg.conds.clear();
    fg.logic = Logic::Or;
    fg.conds = { cond("_id", Op::Equals, { Value::integer(1) }), cond("_id", Op::Equals, { Value::integer(2) }) };
    CHECK(c->count(m, fg, nullptr) == 2, "OR group");

    PageRequest so;
    so.limit = 5;
    so.sort = { { "age", true }, { "_id", false } };
    Page ps = c->fetch_page(m, so, nullptr);
    CHECK(ps.rows.size() == 5 && ps.rows[0][m.column_index("age")].i == 77, "remote sort by age DESC");
    DistinctRequest dr;
    dr.column = "country";
    CHECK(c->distinct_values(m, dr, nullptr).size() == 5, "distinct countries = 5");

    std::printf("[edit, conflict]\n");
    Change up;
    up.type = Change::Type::Update;
    up.key = { { "_id", Value::integer(7) } };
    up.set = { { "name", Value::text("Renombrado"), Value::text("Cliente 7") } };
    CHECK(c->apply_changes(m, { up }).updated == 1, "update one document");
    PageRequest one;
    one.limit = 1;
    one.filter.conds = { cond("_id", Op::Equals, { Value::integer(7) }) };
    Page r7 = c->fetch_page(m, one, nullptr);
    CHECK(r7.rows.size() == 1 && r7.rows[0][m.column_index("name")].s == "Renombrado", "value verified on the server");

    Change stale = up;
    stale.set = { { "name", Value::text("Otro"), Value::text("Cliente 7") } };
    Change good;
    good.type = Change::Type::Update;
    good.key = { { "_id", Value::integer(8) } };
    good.set = { { "name", Value::text("NoDebeGuardarse"), Value::text("Cliente 8") } };
    CHECK(throws(ErrKind::Conflict, [&] { c->apply_changes(m, { good, stale }); }), "stale original => Conflict");
    one.filter.conds = { cond("_id", Op::Equals, { Value::integer(8) }) };
    r7 = c->fetch_page(m, one, nullptr);
    CHECK(r7.rows[0][m.column_index("name")].s == "Cliente 8", "a conflict leaves earlier changes of the batch unapplied");

    Change setmissing;
    setmissing.type = Change::Type::Update;
    setmissing.key = { { "_id", Value::integer(10) } };
    setmissing.set = { { "note", Value::text("ahora existe"), Value::missing() } };
    CHECK(c->apply_changes(m, { setmissing }).updated == 1, "set a field that was absent");
    Change tonull;
    tonull.type = Change::Type::Update;
    tonull.key = { { "_id", Value::integer(9) } };
    tonull.set = { { "country", Value::null(), Value::text(countries[9 % 5]) } };
    CHECK(c->apply_changes(m, { tonull }).updated == 1, "set a field to null");
    one.filter.conds = { cond("_id", Op::Equals, { Value::integer(9) }) };
    r7 = c->fetch_page(m, one, nullptr);
    CHECK(r7.rows[0][m.column_index("country")].is_null() && r7.rows[0][m.column_index("country")].kind != Value::Kind::Missing, "null is stored as null, not as absent");
    Change keepint;
    keepint.type = Change::Type::Update;
    keepint.key = { { "_id", Value::integer(11) } };
    keepint.set = { { "age", Value::real(40.0), Value::integer(18 + 11 % 60) } };
    c->apply_changes(m, { keepint });
    one.filter.conds = { cond("_id", Op::Equals, { Value::integer(11) }) };
    r7 = c->fetch_page(m, one, nullptr);
    CHECK(r7.rows[0][m.column_index("age")].kind == Value::Kind::Int, "editing an integer keeps it an integer");

    Change del;
    del.type = Change::Type::Delete;
    del.key = { { "_id", Value::integer(5000) } };
    CHECK(c->apply_changes(m, { del }).deleted == 1, "delete by _id");
    CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "count after delete = 4999");
    CHECK(throws(ErrKind::Conflict, [&] { c->apply_changes(m, { del }); }), "deleting a vanished document => Conflict");
    Change dup;
    dup.type = Change::Type::Insert;
    dup.values = { { "_id", Value::integer(1) }, { "name", Value::text("dup") } };
    std::string dmsg;
    CHECK(throws(ErrKind::Query, [&] { c->apply_changes(m, { dup }); }, &dmsg), "duplicate _id is an error");
    std::printf("        (message: %s)\n", dmsg.c_str());

    std::printf("[export streaming]\n");
    PageRequest all;
    uint64_t seen = 0;
    size_t max_batch = 0;
    uint64_t got = c->stream_rows(m, all, 700, [&](std::vector<std::vector<Value>> &b) {
        seen += b.size();
        max_batch = std::max(max_batch, b.size());
        return true;
    }, nullptr);
    CHECK(got == 4999 && seen == 4999, "stream_rows delivers every document");
    CHECK(max_batch <= 700, "batches are bounded");
    CHECK(c->stream_rows(m, all, 100, [&](std::vector<std::vector<Value>> &) { return false; }, nullptr) == 100, "stopping from the callback ends the stream");
    CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "connection usable after an abandoned stream");

    std::printf("[command editor]\n");
    QueryOutput q = cmd("{\"find\":\"" + cname + "\",\"filter\":{\"country\":\"Chile\"},\"limit\":20,\"sort\":{\"_id\":1}}");
    CHECK(q.rows.size() == 20 && !q.columns.empty(), "find command returns documents and columns");
    QueryOutput q2 = c->execute("{\"find\":\"" + cname + "\",\"filter\":{}}", 50, nullptr);
    CHECK(q2.rows.size() == 50 && q2.truncated, "max_rows truncates");
    QueryOutput q3 = cmd("{\"ping\":1}");
    CHECK(q3.rows.size() == 1, "non-cursor command returns its reply as one row");
    CHECK(throws(ErrKind::Validation, [&] { cmd("{}"); }), "empty command is a validation error");
    std::string bad;
    CHECK(throws(ErrKind::Query, [&] { cmd("{\"find\":1}"); }, &bad) || !bad.empty(), "a server-side error carries the server's message");
    std::printf("        (message: %s)\n", bad.c_str());

    std::printf("[cancel]\n");
    {
        auto tok = std::make_shared<CancelToken>();
        std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(1200)); tok->cancel(); });
        auto t0 = std::chrono::steady_clock::now();
        bool cancelled = false;
        std::string msg;
        try {
            c->execute("{\"find\":\"" + cname + "\",\"filter\":{\"$where\":\"sleep(20000) || true\"}}", 10, tok);
        } catch (const DbError &e) {
            cancelled = e.kind == ErrKind::Cancelled;
            msg = e.what();
        }
        t.join();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("        (stopped after %lld ms: %s)\n", (long long)ms, msg.c_str());
        CHECK(ms < 8000, "a 20 s command stops within 8 s of Cancel");
        CHECK(cancelled, "reported as Cancelled");
        CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "connection usable after cancel");
    }

    std::printf("[read-only]\n");
    {
        Profile ro = p;
        ro.read_only = true;
        auto rc = drivers::connect(ro, pass);
        TableMeta rm = rc->table_meta(ref);
        CHECK(!rm.edit.editable, "read-only connection blocks editing");
        CHECK(throws(ErrKind::ReadOnly, [&] { rc->apply_changes(rm, { del }); }), "apply_changes refused with ReadOnly");
        CHECK(throws(ErrKind::ReadOnly, [&] { rc->execute("{\"delete\":\"" + cname + "\",\"deletes\":[{\"q\":{},\"limit\":0}]}", 10, nullptr); }), "a write command is refused in read-only mode");
        CHECK(rc->execute("{\"find\":\"" + cname + "\",\"limit\":1}", 10, nullptr).rows.size() == 1, "reads still work");
        CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "nothing was deleted");
    }

    std::printf("[connection errors]\n");
    {
        Profile bad_p = p;
        bad_p.port = 1;
        std::string em;
        CHECK(throws(ErrKind::Connection, [&] { drivers::connect(bad_p, pass); }, &em), "closed port => Connection error");
        std::printf("        (message: %s)\n", em.substr(0, 160).c_str());
    }

    try { cmd("{\"drop\":\"" + cname + "\"}"); } catch (...) {}
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
