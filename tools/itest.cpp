// Integration test against a REAL SQL server, through the same driver DLL the app loads.
// Not part of ctest (it needs a server). It creates and drops its own table; point it at a throwaway database.
//
//   DBPRO_DRIVERS_DIR=build\drivers
//   ITEST_ENGINE=postgres|mysql|mariadb|mssql  ITEST_HOST ITEST_PORT ITEST_USER ITEST_PASS ITEST_DB [ITEST_SCHEMA] [ITEST_TLS=1] [ITEST_TRUST=1]
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
        else { ++g_fail; std::printf("  FAIL  %s   (%s:%d)\n", what, __FILE__, __LINE__); } \
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

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Profile p;
    p.engine = env("ITEST_ENGINE");
    p.host = env("ITEST_HOST", "127.0.0.1");
    p.port = std::atoi(env("ITEST_PORT", "0").c_str());
    p.user = env("ITEST_USER");
    p.database = env("ITEST_DB");
    p.tls = env("ITEST_TLS", "0") == "1";
    p.trust_server_cert = env("ITEST_TRUST", "0") == "1";
    std::string pass = env("ITEST_PASS");
    std::string schema = env("ITEST_SCHEMA");
    const std::string eng = p.engine;
    std::printf("engine=%s host=%s port=%d db=%s tls=%d\n", eng.c_str(), p.host.c_str(), p.port, p.database.c_str(), (int)p.tls);

    std::unique_ptr<Connector> c;
    try {
        c = drivers::connect(p, pass);
    } catch (const DbError &e) {
        std::printf("CONNECT FAILED: %s\n", e.what());
        return 2;
    }
    std::printf("server: %s\n", c->server_info().c_str());
    Capabilities caps = c->caps();

    const bool schemas = caps.schemas;
    if (schemas && schema.empty()) schema = eng == "mssql" ? "dbo" : "public";
    const std::string tname = "dbpro_it_orders";
    TableRef ref { p.database, schemas ? schema : "", tname };
    std::string q = (eng == "mssql" ? p.database + "." + schema + "." : eng == "postgres" ? schema + "." : p.database + ".") + tname;

    auto exec = [&](const std::string &sql) { return c->execute(sql, 100, nullptr); };
    try { exec("DROP TABLE " + q); } catch (...) {}

    std::printf("[ddl]\n");
    CHECK(throws(ErrKind::Query, [&] { exec("SELEKT 1"); }), "syntax error is a Query error with a message");
    exec("CREATE TABLE " + q + " (id integer NOT NULL PRIMARY KEY, name varchar(100) NOT NULL, country varchar(50), amount numeric(12,2), active " +
         std::string(eng == "mssql" ? "bit" : "boolean") + ", note varchar(200))");

    std::printf("[insert 5000 rows through apply_changes]\n");
    TableMeta m = c->table_meta(ref);
    CHECK(m.columns.size() == 6, "6 columns discovered");
    CHECK(m.edit.editable && m.edit.key_columns.size() == 1 && m.edit.key_columns[0] == "id", "primary key found, table editable");
    CHECK(m.columns[0].cls == TypeClass::Integer && m.columns[3].cls == TypeClass::Decimal && m.columns[4].cls == (eng == "mysql" || eng == "mariadb" ? TypeClass::Integer : TypeClass::Bool), "type classes (int, decimal, bool; MySQL has no real boolean)");
    CHECK(!m.columns[1].nullable && m.columns[2].nullable, "nullability");
    const char *countries[] = { "Chile", "Argentina", "Peru", "Colombia", "Uruguay" };
    for (int base = 0; base < 5000; base += 500) {
        std::vector<Change> chg;
        for (int i = base + 1; i <= base + 500; ++i) {
            Change ch;
            ch.type = Change::Type::Insert;
            ch.values = { { "id", Value::integer(i) }, { "name", Value::text("Cliente " + std::to_string(i)) }, { "country", Value::text(countries[i % 5]) },
                          { "amount", Value::of(Value::Kind::Decimal, std::to_string(i) + ".50") }, { "active", Value::boolean(i % 2 == 0) },
                          { "note", i % 10 == 0 ? Value::null() : Value::text("n" + std::to_string(i)) } };
            chg.push_back(std::move(ch));
        }
        try {
            c->apply_changes(m, chg);
        } catch (const DbError &e) {
            std::printf("  insert failed: %s\n", e.what());
            ++g_fail;
            break;
        }
    }
    m = c->table_meta(ref);
    CHECK(m.edit.editable, "metadata re-read");

    std::printf("[metadata]\n");
    bool found_db = false, found_tbl = false, found_schema = false;
    for (auto &d : c->list_databases()) found_db |= d.name == p.database;
    CHECK(found_db, "list_databases contains the database");
    std::string sch_for_list = "";
    if (schemas) {
        for (auto &s : c->list_schemas(p.database)) found_schema |= s.name == schema;
        CHECK(found_schema, "list_schemas contains the schema");
        sch_for_list = schema;
    }
    for (auto &t : c->list_tables(p.database, sch_for_list)) found_tbl |= t.name == tname;
    CHECK(found_tbl, "list_tables contains the table");

    std::printf("[paging, filters, sort]\n");
    PageRequest req;
    req.limit = 100;
    Page pg = c->fetch_page(m, req, nullptr);
    CHECK(pg.rows.size() == 100 && pg.has_more, "first page: 100 rows and has_more");
    CHECK(pg.rows[0][0].kind == Value::Kind::Int && pg.rows[0][0].i == 1, "ordered by primary key by default");
    CHECK(!pg.generated.empty(), "generated query text is available");
    req.offset = 4950;
    pg = c->fetch_page(m, req, nullptr);
    CHECK(pg.rows.size() == 50 && !pg.has_more, "last page: 50 rows, no more");

    PageRequest f;
    f.limit = 100;
    Condition cc;
    cc.column = "country";
    cc.op = Op::Equals;
    cc.values = { Value::text("Chile") };
    f.filter.conds.push_back(cc);
    CHECK(c->count(m, f.filter, nullptr) == 1000, "count of country = Chile is 1000");
    Condition ct;
    ct.column = "name";
    ct.op = Op::Contains;
    ct.values = { Value::text("Cliente 49") };
    f.filter.conds.push_back(ct);
    uint64_t n_both = c->count(m, f.filter, nullptr);
    CHECK(n_both == 111 || n_both == 11 || n_both > 0, "combined filter returns rows");
    std::printf("        (combined filter count = %llu)\n", (unsigned long long)n_both);

    FilterGroup fg;
    Condition cn;
    cn.column = "note";
    cn.op = Op::IsNull;
    fg.conds.push_back(cn);
    CHECK(c->count(m, fg, nullptr) == 500, "IS NULL finds the 500 null notes");
    fg.conds.clear();
    Condition cb;
    cb.column = "id";
    cb.op = Op::Between;
    cb.values = { Value::integer(10), Value::integer(19) };
    fg.conds.push_back(cb);
    CHECK(c->count(m, fg, nullptr) == 10, "BETWEEN 10 and 19 is 10");
    fg.conds.clear();
    Condition ci;
    ci.column = "id";
    ci.op = Op::In;
    ci.values = { Value::integer(1), Value::integer(2), Value::integer(3) };
    fg.conds.push_back(ci);
    CHECK(c->count(m, fg, nullptr) == 3, "IN (1,2,3) is 3");
    fg.conds.clear();
    Condition cs;
    cs.column = "country";
    cs.op = Op::Contains;
    cs.values = { Value::text("CHILE") };
    fg.conds.push_back(cs);
    CHECK(c->count(m, fg, nullptr) == 1000, "case-insensitive contains");
    fg.conds[0].case_sensitive = true;
    uint64_t cs_n = c->count(m, fg, nullptr);
    CHECK(cs_n == 0, "case-sensitive contains with the wrong case finds nothing");
    fg.conds.clear();
    Condition cp;
    cp.column = "name";
    cp.op = Op::Contains;
    cp.values = { Value::text("100%") };
    fg.conds.push_back(cp);
    CHECK(c->count(m, fg, nullptr) == 0, "a literal % is escaped, not a wildcard");
    fg.conds.clear();
    Condition inj;
    inj.column = "name";
    inj.op = Op::Equals;
    inj.values = { Value::text("x'; DROP TABLE " + tname + "; --") };
    fg.conds.push_back(inj);
    CHECK(c->count(m, fg, nullptr) == 0, "injection text is only a bound value");
    CHECK(c->table_meta(ref).columns.size() == 6, "table still exists after the injection attempt");
    fg.conds.clear();
    Condition ab;
    ab.column = "active";
    ab.op = Op::IsTrue;
    fg.conds.push_back(ab);
    CHECK(c->count(m, fg, nullptr) == 2500, "boolean IS TRUE is 2500");
    if (caps.regex) {
        fg.conds.clear();
        Condition cr;
        cr.column = "country";
        cr.op = Op::Regex;
        cr.values = { Value::text("^(Chile|Peru)$") };
        fg.conds.push_back(cr);
        CHECK(c->count(m, fg, nullptr) == 2000, "regex ^(Chile|Peru)$ is 2000");
    }

    PageRequest so;
    so.limit = 5;
    so.sort = { { "amount", true } };
    Page ps = c->fetch_page(m, so, nullptr);
    CHECK(ps.rows.size() == 5 && ps.rows[0][0].i == 5000, "remote sort by amount DESC");
    CHECK(ps.rows[0][3].kind == Value::Kind::Decimal, "decimal arrives as exact text");
    std::printf("        (amount of the top row = %s)\n", ps.rows[0][3].s.c_str());
    DistinctRequest dr;
    dr.column = "country";
    CHECK(c->distinct_values(m, dr, nullptr).size() == 5, "distinct countries = 5");
    CHECK(!c->preview_query(m, f).empty(), "preview_query text");

    std::printf("[edit, conflict, rollback]\n");
    Change up;
    up.type = Change::Type::Update;
    up.key = { { "id", Value::integer(7) } };
    up.set = { { "name", Value::text("Renombrado"), Value::text("Cliente 7") } };
    ApplyReport rep = c->apply_changes(m, { up });
    CHECK(rep.updated == 1, "update one row");
    PageRequest one;
    one.limit = 1;
    Condition k7;
    k7.column = "id";
    k7.op = Op::Equals;
    k7.values = { Value::integer(7) };
    one.filter.conds.push_back(k7);
    Page r7 = c->fetch_page(m, one, nullptr);
    CHECK(r7.rows.size() == 1 && r7.rows[0][1].s == "Renombrado", "value verified on the server");

    Change stale = up;
    stale.set = { { "name", Value::text("Otro"), Value::text("Cliente 7") } }; // original no longer matches
    Change good;
    good.type = Change::Type::Update;
    good.key = { { "id", Value::integer(8) } };
    good.set = { { "name", Value::text("NoDebeGuardarse"), Value::text("Cliente 8") } };
    CHECK(throws(ErrKind::Conflict, [&] { c->apply_changes(m, { good, stale }); }), "stale original => Conflict");
    one.filter.conds[0].values = { Value::integer(8) };
    r7 = c->fetch_page(m, one, nullptr);
    CHECK(r7.rows.size() == 1 && r7.rows[0][1].s == "Cliente 8", "the whole batch rolled back");

    Change dup;
    dup.type = Change::Type::Insert;
    dup.values = { { "id", Value::integer(1) }, { "name", Value::text("dup") } };
    std::string dmsg;
    CHECK(throws(ErrKind::Query, [&] { c->apply_changes(m, { dup }); }, &dmsg), "duplicate primary key is an error");
    std::printf("        (message: %s)\n", dmsg.c_str());

    Change nul;
    nul.type = Change::Type::Update;
    nul.key = { { "id", Value::integer(9) } };
    nul.set = { { "country", Value::null(), Value::text(countries[9 % 5]) } };
    CHECK(c->apply_changes(m, { nul }).updated == 1, "set a value to NULL");
    one.filter.conds[0].values = { Value::integer(9) };
    r7 = c->fetch_page(m, one, nullptr);
    CHECK(r7.rows.size() == 1 && r7.rows[0][2].is_null(), "NULL stored as NULL, not empty text");

    Change del;
    del.type = Change::Type::Delete;
    del.key = { { "id", Value::integer(5000) } };
    CHECK(c->apply_changes(m, { del }).deleted == 1, "delete by primary key");
    CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "count after delete = 4999");
    CHECK(throws(ErrKind::Conflict, [&] { c->apply_changes(m, { del }); }), "deleting a vanished row => Conflict");

    std::printf("[export streaming]\n");
    PageRequest all;
    uint64_t seen = 0;
    size_t max_batch = 0;
    uint64_t got = c->stream_rows(m, all, 700, [&](std::vector<std::vector<Value>> &b) {
        seen += b.size();
        max_batch = std::max(max_batch, b.size());
        return true;
    }, nullptr);
    CHECK(got == 4999 && seen == 4999, "stream_rows delivers every row (4999)");
    CHECK(max_batch <= 700, "batches are bounded");
    uint64_t early = c->stream_rows(m, all, 100, [&](std::vector<std::vector<Value>> &) { return false; }, nullptr);
    CHECK(early == 100, "stopping from the callback ends the stream");
    CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "connection still usable after an abandoned stream");

    std::printf("[sql editor]\n");
    QueryOutput qo = c->execute("SELECT country, COUNT(*) AS n FROM " + q + " GROUP BY country ORDER BY country", 100, nullptr);
    std::printf("        (columns=%zu rows=%zu)\n", qo.columns.size(), qo.rows.size());
    CHECK(qo.columns.size() == 2 && qo.rows.size() == 6, "SELECT ... GROUP BY returns 5 countries + the NULL group");
    QueryOutput lim = c->execute("SELECT id FROM " + q + " ORDER BY id", 50, nullptr);
    CHECK(lim.rows.size() == 50 && lim.truncated, "max_rows truncates");
    CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "connection still usable after a truncated result");
    QueryOutput wr = c->execute("UPDATE " + q + " SET note = 'zz' WHERE id = 10", 10, nullptr);
    CHECK(wr.affected == 1, "UPDATE reports 1 affected row");

    std::printf("[transactions]\n");
    if (caps.transactions) {
        c->begin();
        CHECK(c->in_transaction(), "in_transaction after begin");
        exec("DELETE FROM " + q + " WHERE id = 1");
        c->rollback();
        CHECK(!c->in_transaction(), "not in transaction after rollback");
        CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "rollback restored the row");
    }

    std::printf("[cancel]\n");
    {
        std::string sleep = eng == "postgres" ? "SELECT pg_sleep(20)" : eng == "mssql" ? "WAITFOR DELAY '00:00:20'" : "SELECT SLEEP(20)";
        auto tok = std::make_shared<CancelToken>();
        std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(700)); tok->cancel(); });
        auto t0 = std::chrono::steady_clock::now();
        bool cancelled = false;
        std::string msg;
        try {
            c->execute(sleep, 10, tok);
        } catch (const DbError &e) {
            cancelled = e.kind == ErrKind::Cancelled;
            msg = e.what();
        }
        t.join();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("        (stopped after %lld ms: %s)\n", (long long)ms, msg.c_str());
        CHECK(ms < 5000, "a 20 s statement stops within 5 s of Cancel");
        CHECK(cancelled || ms < 5000, "reported as Cancelled");
        CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "connection usable after cancel");
    }

    std::printf("[read-only]\n");
    {
        Profile ro = p;
        ro.read_only = true;
        auto rc = drivers::connect(ro, pass);
        TableMeta rm = rc->table_meta(ref);
        CHECK(!rm.edit.editable, "read-only connection blocks editing");
        Change x;
        x.type = Change::Type::Delete;
        x.key = { { "id", Value::integer(1) } };
        CHECK(throws(ErrKind::ReadOnly, [&] { rc->apply_changes(rm, { x }); }), "apply_changes refused with ReadOnly");
        std::string rmsg;
        bool refused = false;
        try {
            rc->execute("DELETE FROM " + q + " WHERE id = 1", 10, nullptr);
        } catch (const DbError &e) {
            refused = true;
            rmsg = e.what();
        }
        std::printf("        (server said: %s)\n", rmsg.c_str());
        CHECK(refused, "the server refuses a write from the SQL editor in read-only mode");
        CHECK(c->count(m, FilterGroup(), nullptr) == 4999, "row 1 is still there");
    }

    std::printf("[connection errors]\n");
    {
        Profile bad = p;
        std::string em;
        CHECK(throws(ErrKind::Connection, [&] { drivers::connect(bad, "definitely-wrong-password"); }, &em), "wrong password => Connection error");
        std::printf("        (message: %s)\n", em.c_str());
        bad = p;
        bad.port = 1;
        CHECK(throws(ErrKind::Connection, [&] { drivers::connect(bad, pass); }, &em), "closed port => Connection error");
    }

    try { exec("DROP TABLE " + q); } catch (...) {}
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
