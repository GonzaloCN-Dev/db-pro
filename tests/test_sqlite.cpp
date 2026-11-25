#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "connectors/sqlite_conn.h"
#include "core/sqlgen.h"
#include "test.h"

using namespace dbp;
namespace fs = std::filesystem;

static std::string tmp_db(const char *name)
{
    fs::path p = fs::temp_directory_path() / (std::string("dbpro_") + name + ".db");
    std::error_code ec;
    fs::remove(p, ec);
    return p.string();
}

static void seed(Connector &c, int rows)
{
    c.execute("CREATE TABLE expedientes (id INTEGER PRIMARY KEY, numero TEXT NOT NULL, entidad TEXT, provincia TEXT, importe DECIMAL(12,2), estado TEXT, nota TEXT)", 10, nullptr);
    c.begin();
    c.execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < " + std::to_string(rows) + ") "
              "INSERT INTO expedientes SELECT i, printf('EXP-%07d', i), CASE i % 3 WHEN 0 THEN 'Santander' WHEN 1 THEN 'BBVA' ELSE 'Caixa' END, "
              "CASE i % 2 WHEN 0 THEN 'Madrid' ELSE 'Sevilla' END, i * 1.5, CASE i % 5 WHEN 0 THEN 'Cancelado' ELSE 'Activo' END, "
              "CASE WHEN i % 10 = 0 THEN NULL ELSE '' END FROM n", 10, nullptr);
    c.commit();
}

static TableMeta meta_of(Connector &c) { return c.table_meta({ "main", "", "expedientes" }); }

TEST(sqlite_pre_cancelled_sql_does_not_execute)
{
    auto c = connect_sqlite(tmp_db("precancel"), false);
    c->execute("CREATE TABLE t (id INTEGER)", 10, nullptr);
    auto token = std::make_shared<CancelToken>();
    token->cancel();
    bool cancelled = false;
    try { c->execute("INSERT INTO t VALUES (1)", 10, token); }
    catch (const DbError &e) { cancelled = e.kind == ErrKind::Cancelled; }
    CHECK(cancelled);
    CHECK_EQ(c->execute("SELECT count(*) FROM t", 10, nullptr).rows[0][0].i, 0);
}

TEST(sqlite_pre_cancelled_read_operations_do_not_return_data)
{
    auto c = connect_sqlite(tmp_db("precancel_reads"), false);
    seed(*c, 10);
    TableMeta m = meta_of(*c);
    auto token = std::make_shared<CancelToken>();
    token->cancel();
    int cancelled = 0;
    try { c->count(m, {}, token); } catch (const DbError &e) { if (e.kind == ErrKind::Cancelled) ++cancelled; }
    DistinctRequest d; d.column = "entidad";
    try { c->distinct_values(m, d, token); } catch (const DbError &e) { if (e.kind == ErrKind::Cancelled) ++cancelled; }
    int batches = 0;
    try { c->stream_rows(m, {}, 2, [&](auto &) { ++batches; return true; }, token); }
    catch (const DbError &e) { if (e.kind == ErrKind::Cancelled) ++cancelled; }
    CHECK_EQ(cancelled, 3);
    CHECK_EQ(batches, 0);
}

TEST(sqlite_stream_cancelled_in_callback_stops_before_next_batch)
{
    auto c = connect_sqlite(tmp_db("cancel_stream"), false);
    seed(*c, 10);
    TableMeta m = meta_of(*c);
    auto token = std::make_shared<CancelToken>();
    int batches = 0;
    bool cancelled = false;
    try {
        c->stream_rows(m, {}, 2, [&](auto &) { ++batches; token->cancel(); return true; }, token);
    } catch (const DbError &e) { cancelled = e.kind == ErrKind::Cancelled; }
    CHECK(cancelled);
    CHECK_EQ(batches, 1);
}

TEST(sqlite_metadata_and_row_identity)
{
    auto c = connect_sqlite(tmp_db("meta"), false);
    seed(*c, 10);
    TableMeta m = meta_of(*c);
    CHECK_EQ(m.columns.size(), (size_t)7);
    CHECK(m.edit.editable);
    CHECK_EQ(m.edit.key_columns.size(), (size_t)1);
    CHECK(m.columns[0].is_pk());
    CHECK(m.columns[0].identity);
    CHECK(m.columns[4].cls == TypeClass::Decimal);
}

TEST(sqlite_local_activity_reports_connection_cache_metrics)
{
    auto c = connect_sqlite(tmp_db("activity"), false);
    CHECK(c->caps().activity);
    CHECK(!c->caps().kill_session);
    Activity a = c->activity();
    CHECK(a.rows.empty());
    bool has_cache = false, has_hits = false;
    for (const Metric &m : a.metrics) {
        if (m.key == "cache_used" && m.unit == "B") has_cache = true;
        if (m.key == "cache_hits" && m.counter) has_hits = true;
    }
    CHECK(has_cache);
    CHECK(has_hits);
}

TEST(table_without_primary_key_is_not_editable)
{
    auto c = connect_sqlite(tmp_db("nopk"), false);
    c->execute("CREATE TABLE t (a TEXT, b TEXT)", 1, nullptr);
    TableMeta m = c->table_meta({ "main", "", "t" });
    CHECK(!m.edit.editable);
    CHECK(!m.edit.blocked_reason.empty());
    Change ch; ch.type = Change::Type::Delete; ch.key = { { "a", Value::text("x") } };
    bool threw = false;
    try { c->apply_changes(m, { ch }); } catch (const DbError &) { threw = true; }
    CHECK(threw);
}

TEST(filter_searches_whole_table_not_just_loaded_page)
{
    auto c = connect_sqlite(tmp_db("big"), false);
    seed(*c, 500000);
    TableMeta m = meta_of(*c);

    PageRequest first; first.limit = 100;
    Page p0 = c->fetch_page(m, first, nullptr);
    CHECK_EQ(p0.rows.size(), (size_t)100);
    CHECK(p0.has_more);

    // A record far beyond the first page.
    PageRequest r; r.limit = 100;
    Condition k; k.column = "numero"; k.op = Op::Equals; k.values = { Value::text("EXP-0432109") };
    r.filter.conds.push_back(k);
    auto t0 = std::chrono::steady_clock::now();
    Page p = c->fetch_page(m, r, nullptr);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(p.rows.size(), (size_t)1);
    CHECK_EQ(p.rows[0][0].i, (int64_t)432109);
    std::printf("    (equals lookup over 500k rows: %lld ms)\n", (long long)ms);

    // Several columns at once.
    PageRequest multi; multi.limit = 100;
    Condition e; e.column = "entidad"; e.op = Op::Contains; e.values = { Value::text("santan") };
    Condition pr; pr.column = "provincia"; pr.op = Op::Equals; pr.values = { Value::text("Madrid") };
    Condition im; im.column = "importe"; im.op = Op::Gt; im.values = { Value::integer(150000) };
    Condition st; st.column = "estado"; st.op = Op::NotEquals; st.values = { Value::text("Cancelado") };
    multi.filter.conds = { e, pr, im, st };
    Page pm = c->fetch_page(m, multi, nullptr);
    CHECK(!pm.rows.empty());
    for (auto &row : pm.rows) {
        CHECK(row[2].s == "Santander");
        CHECK(row[3].s == "Madrid");
        CHECK(row[5].s != "Cancelado");
    }
    uint64_t n = c->count(m, multi.filter, nullptr);
    uint64_t manual = 0;
    for (int i = 1; i <= 500000; ++i)
        if (i % 3 == 0 && i % 2 == 0 && i * 1.5 > 150000 && i % 5 != 0) ++manual;
    CHECK_EQ(n, manual);
}

TEST(remote_sort_and_deterministic_paging)
{
    auto c = connect_sqlite(tmp_db("sort"), false);
    seed(*c, 1000);
    TableMeta m = meta_of(*c);
    PageRequest r; r.limit = 50; r.sort = { { "provincia", true } };
    Page a = c->fetch_page(m, r, nullptr);
    r.offset = 50;
    Page b = c->fetch_page(m, r, nullptr);
    CHECK_EQ(a.rows.size(), (size_t)50);
    CHECK(a.rows[0][3].s == "Sevilla");
    // No row repeats across pages.
    for (auto &x : a.rows)
        for (auto &y : b.rows) CHECK(x[0].i != y[0].i);
}

TEST(null_empty_string_and_missing_are_distinguished)
{
    auto c = connect_sqlite(tmp_db("nulls"), false);
    seed(*c, 100);
    TableMeta m = meta_of(*c);
    PageRequest r; r.limit = 200;
    Condition isnull; isnull.column = "nota"; isnull.op = Op::IsNull;
    r.filter.conds = { isnull };
    CHECK_EQ(c->count(m, r.filter, nullptr), (uint64_t)10);
    PageRequest e; e.limit = 200;
    Condition eq; eq.column = "nota"; eq.op = Op::Equals; eq.values = { Value::text("") };
    e.filter.conds = { eq };
    CHECK_EQ(c->count(m, e.filter, nullptr), (uint64_t)90);
}

TEST(edit_insert_delete_roundtrip_verified_on_server)
{
    auto c = connect_sqlite(tmp_db("edit"), false);
    seed(*c, 100);
    TableMeta m = meta_of(*c);

    Change up; up.type = Change::Type::Update;
    up.key = { { "id", Value::integer(43) } }; // 43 % 3 == 1 -> BBVA
    up.set = { { "entidad", Value::text("Nueva"), Value::text("BBVA") } };
    Change ins; ins.type = Change::Type::Insert;
    ins.values = { { "numero", Value::text("EXP-NEW") }, { "entidad", Value::text("Ibercaja") } };
    Change del; del.type = Change::Type::Delete; del.key = { { "id", Value::integer(7) } };
    ApplyReport rep = c->apply_changes(m, { up, ins, del });
    CHECK_EQ(rep.updated, 1);
    CHECK_EQ(rep.inserted, 1);
    CHECK_EQ(rep.deleted, 1);

    auto o = c->execute("SELECT entidad FROM expedientes WHERE id = 43", 10, nullptr);
    CHECK_EQ(o.rows[0][0].s, std::string("Nueva"));
    CHECK_EQ(c->execute("SELECT count(*) FROM expedientes WHERE id = 7", 10, nullptr).rows[0][0].i, (int64_t)0);
    CHECK_EQ(c->execute("SELECT count(*) FROM expedientes WHERE numero = 'EXP-NEW'", 10, nullptr).rows[0][0].i, (int64_t)1);
}

TEST(concurrent_modification_is_detected_never_overwritten)
{
    auto path = tmp_db("conflict");
    auto a = connect_sqlite(path, false);
    seed(*a, 10);
    auto b = connect_sqlite(path, false); // second "user"
    TableMeta ma = meta_of(*a);
    TableMeta mb = meta_of(*b);

    Change ub; ub.type = Change::Type::Update; ub.key = { { "id", Value::integer(3) } };
    ub.set = { { "estado", Value::text("Pagado"), Value::text("Activo") } };
    b->apply_changes(mb, { ub });

    Change ua; ua.type = Change::Type::Update; ua.key = { { "id", Value::integer(3) } };
    ua.set = { { "estado", Value::text("Rechazado"), Value::text("Activo") } };
    bool conflict = false;
    try { a->apply_changes(ma, { ua }); } catch (const DbError &e) { conflict = e.kind == ErrKind::Conflict; }
    CHECK(conflict);
    CHECK_EQ(a->execute("SELECT estado FROM expedientes WHERE id = 3", 1, nullptr).rows[0][0].s, std::string("Pagado"));
}

TEST(failed_batch_rolls_back_atomically)
{
    auto c = connect_sqlite(tmp_db("atomic"), false);
    seed(*c, 10);
    TableMeta m = meta_of(*c);
    Change ok; ok.type = Change::Type::Update; ok.key = { { "id", Value::integer(1) } };
    ok.set = { { "estado", Value::text("X"), Value::text("Activo") } };
    Change bad; bad.type = Change::Type::Update; bad.key = { { "id", Value::integer(2) } };
    bad.set = { { "estado", Value::text("Y"), Value::text("WRONG-ORIGINAL") } };
    bool threw = false;
    try { c->apply_changes(m, { ok, bad }); } catch (const DbError &) { threw = true; }
    CHECK(threw);
    CHECK_EQ(c->execute("SELECT estado FROM expedientes WHERE id = 1", 1, nullptr).rows[0][0].s, std::string("Activo"));
}

TEST(read_only_connection_blocks_writes)
{
    auto path = tmp_db("ro");
    { auto w = connect_sqlite(path, false); seed(*w, 5); }
    auto c = connect_sqlite(path, true);
    TableMeta m = meta_of(*c);
    CHECK(!m.edit.editable);
    bool threw = false;
    try { c->execute("DELETE FROM expedientes", 10, nullptr); } catch (const DbError &e) { threw = e.kind == ErrKind::ReadOnly; }
    CHECK(threw);
    CHECK_EQ(c->execute("SELECT count(*) FROM expedientes", 1, nullptr).rows[0][0].i, (int64_t)5);
}

TEST(export_streams_all_filtered_rows_in_batches)
{
    auto c = connect_sqlite(tmp_db("stream"), false);
    seed(*c, 200000);
    TableMeta m = meta_of(*c);
    PageRequest r;
    Condition e; e.column = "entidad"; e.op = Op::Equals; e.values = { Value::text("Santander") };
    r.filter.conds = { e };
    uint64_t expected = c->count(m, r.filter, nullptr);
    size_t max_batch = 0, batches = 0;
    uint64_t seen = 0;
    uint64_t got = c->stream_rows(m, r, 1000, [&](std::vector<std::vector<Value>> &b) {
        max_batch = std::max(max_batch, b.size());
        ++batches;
        seen += b.size();
        return true;
    }, nullptr);
    CHECK_EQ(got, expected);
    CHECK_EQ(seen, expected);
    CHECK(expected > 60000);
    CHECK(max_batch <= 1000); // memory stays bounded by the batch size
    CHECK(batches > 60);
}

TEST(long_query_can_be_cancelled)
{
    auto c = connect_sqlite(tmp_db("cancel"), false);
    seed(*c, 10);
    auto tok = std::make_shared<CancelToken>();
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); tok->cancel(); });
    bool cancelled = false;
    auto t0 = std::chrono::steady_clock::now();
    try {
        c->execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n) SELECT count(*) FROM n", 10, tok);
    } catch (const DbError &e) { cancelled = e.kind == ErrKind::Cancelled; }
    t.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(cancelled);
    CHECK(ms < 3000);
    // The connection is still usable afterwards.
    CHECK_EQ(c->execute("SELECT 1", 1, nullptr).rows[0][0].i, (int64_t)1);
}

TEST(query_deadline_stops_long_query_and_is_flagged_as_timeout)
{
    auto c = connect_sqlite(tmp_db("deadline"), false);
    seed(*c, 10);
    auto tok = std::make_shared<CancelToken>();
    bool stopped = false;
    auto t0 = std::chrono::steady_clock::now();
    try {
        Deadline dl(tok, 1);
        c->execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n) SELECT count(*) FROM n", 10, tok);
    } catch (const DbError &) { stopped = true; }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(stopped);
    CHECK(tok->timed_out());
    CHECK(ms < 4000);
    CHECK_EQ(c->execute("SELECT 1", 1, nullptr).rows[0][0].i, (int64_t)1);
}

TEST(reconnect_backoff_grows_and_is_capped)
{
    CHECK_EQ(reconnect_delay_seconds(0), 1);
    CHECK_EQ(reconnect_delay_seconds(1), 2);
    CHECK_EQ(reconnect_delay_seconds(2), 4);
    CHECK_EQ(reconnect_delay_seconds(4), 16);
    CHECK_EQ(reconnect_delay_seconds(50), 16);
    CHECK_EQ(reconnect_delay_seconds(-3), 1);
}

TEST(only_connection_errors_trigger_reconnect)
{
    CHECK(is_connection_lost(ErrKind::Connection));
    CHECK(!is_connection_lost(ErrKind::Query));
    CHECK(!is_connection_lost(ErrKind::Conflict));
    CHECK(!is_connection_lost(ErrKind::Cancelled));
    CHECK(!is_connection_lost(ErrKind::Timeout));
    CHECK(!is_connection_lost(ErrKind::Validation));
    CHECK(!is_connection_lost(ErrKind::ReadOnly));
}

TEST(closed_connection_reports_connection_error_and_new_one_works)
{
    std::string path = tmp_db("reconnect");
    auto c = connect_sqlite(path, false);
    seed(*c, 5);
    c->close();
    ErrKind k = ErrKind::Other;
    try { c->activity(); } catch (const DbError &e) { k = e.kind; }
    CHECK(is_connection_lost(k));
    auto c2 = connect_sqlite(path, false);
    CHECK_EQ(c2->execute("SELECT count(*) FROM expedientes", 1, nullptr).rows[0][0].i, (int64_t)5);
}

TEST(query_deadline_does_not_fire_after_finishing)
{
    auto tok = std::make_shared<CancelToken>();
    {
        Deadline dl(tok, 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    CHECK(!tok->cancelled());
    CHECK(!tok->timed_out());
    auto none = std::make_shared<CancelToken>();
    {
        Deadline dl(none, 0); // no limit
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(!none->cancelled());
}

TEST(distinct_values_remote_search_is_bounded)
{
    auto c = connect_sqlite(tmp_db("distinct"), false);
    seed(*c, 5000);
    TableMeta m = meta_of(*c);
    DistinctRequest r; r.column = "numero"; r.limit = 25;
    CHECK_EQ(c->distinct_values(m, r, nullptr).size(), (size_t)25);
    DistinctRequest s; s.column = "entidad"; s.search = "tand";
    auto v = c->distinct_values(m, s, nullptr);
    CHECK_EQ(v.size(), (size_t)1);
    if (!v.empty()) CHECK_EQ(v[0].s, std::string("Santander"));
}

TEST(sql_editor_executes_multiple_statements)
{
    auto c = connect_sqlite(tmp_db("script"), false);
    auto o = c->execute("CREATE TABLE a(x); INSERT INTO a VALUES (1),(2),(3); SELECT sum(x) FROM a;", 100, nullptr);
    CHECK_EQ(o.rows.size(), (size_t)1);
    CHECK_EQ(o.rows[0][0].i, (int64_t)6);
    bool threw = false;
    try { c->execute("SELEC nonsense", 10, nullptr); } catch (const DbError &e) { threw = e.kind == ErrKind::Query; }
    CHECK(threw);
}

TEST(explicit_transaction_can_be_rolled_back)
{
    auto c = connect_sqlite(tmp_db("tx"), false);
    seed(*c, 10);
    c->begin();
    CHECK(c->in_transaction());
    c->execute("DELETE FROM expedientes", 1, nullptr);
    c->rollback();
    CHECK(!c->in_transaction());
    CHECK_EQ(c->execute("SELECT count(*) FROM expedientes", 1, nullptr).rows[0][0].i, (int64_t)10);
}
