#include <filesystem>
#include <fstream>
#include <sstream>

#include "connectors/sqlite_conn.h"
#include "core/exporter.h"
#include "platform.h"
#include "store.h"
#include "test.h"

using namespace dbp;
namespace fs = std::filesystem;

static std::string slurp(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static fs::path tmp(const char *n) { return fs::temp_directory_path() / (std::string("dbpro_") + n); }

TEST(csv_quotes_delimiters_newlines_and_neutralises_formulas)
{
    CHECK_EQ(csv_escape("a,b", ',', true), std::string("\"a,b\""));
    CHECK_EQ(csv_escape("say \"hi\"", ',', true), std::string("\"say \"\"hi\"\"\""));
    CHECK_EQ(csv_escape("l1\nl2", ',', true), std::string("\"l1\nl2\""));
    CHECK_EQ(csv_escape("=HYPERLINK(\"x\")", ',', true).substr(0, 2), std::string("\"'"));
    CHECK_EQ(csv_escape("-5", ',', true), std::string("-5")); // a plain negative number is not a formula
    CHECK_EQ(csv_escape("a;b", ';', false), std::string("\"a;b\""));
}

TEST(csv_writer_distinguishes_null_from_empty_when_asked)
{
    auto p = tmp("w.csv");
    ExportOptions o; o.bom = false; o.null_text = "NULL";
    auto w = make_writer(p, o);
    ColumnInfo a; a.name = "a"; ColumnInfo b; b.name = "b";
    w->begin({ a, b });
    w->rows({ { Value::null(), Value::text("") }, { Value::integer(5), Value::text("x") } });
    w->end();
    CHECK_EQ(slurp(p), std::string("a,b\r\nNULL,\r\n5,x\r\n"));
}

TEST(json_export_is_valid_and_keeps_decimals_exact)
{
    auto p = tmp("w.json");
    ExportOptions o; o.format = ExportFormat::Json;
    auto w = make_writer(p, o);
    ColumnInfo a; a.name = "amt";
    w->begin({ a });
    w->rows({ { Value::of(Value::Kind::Decimal, "12345678901234567890.123456789") } });
    w->end();
    std::string s = slurp(p);
    CHECK(s.find("\"12345678901234567890.123456789\"") != std::string::npos);
    CHECK(s.front() == '[');
}

TEST(sql_export_escapes_quotes)
{
    auto p = tmp("w.sql");
    ExportOptions o; o.format = ExportFormat::SqlInsert; o.table_name = "t";
    auto w = make_writer(p, o);
    ColumnInfo a; a.name = "n";
    w->begin({ a });
    w->rows({ { Value::text("O'Brien") } });
    w->end();
    CHECK(slurp(p).find("'O''Brien'") != std::string::npos);
}

// Scenario: export ALL filtered rows from a large table through bounded batches.
TEST(export_all_filtered_rows_end_to_end)
{
    fs::path db = tmp("exp.db");
    std::error_code ec; fs::remove(db, ec);
    auto c = connect_sqlite(db.string(), false);
    c->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, g TEXT, v TEXT)", 1, nullptr);
    c->begin();
    c->execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 70000) INSERT INTO t SELECT i, CASE i % 2 WHEN 0 THEN 'a' ELSE 'b' END, 'row,'||i FROM n", 1, nullptr);
    c->commit();
    TableMeta m = c->table_meta({ "main", "", "t" });
    PageRequest r;
    Condition k; k.column = "g"; k.op = Op::Equals; k.values = { Value::text("a") };
    r.filter.conds = { k };
    fs::path out = tmp("exp.csv");
    ExportOptions o; o.bom = false;
    auto w = make_writer(out, o);
    w->begin(m.columns);
    uint64_t n = c->stream_rows(m, r, 500, [&](auto &b) { w->rows(b); return true; }, nullptr);
    w->end();
    CHECK_EQ(n, (uint64_t)35000);
    std::ifstream f(out);
    std::string line; size_t lines = 0;
    while (std::getline(f, line)) ++lines;
    CHECK_EQ(lines, (size_t)35001); // header + every filtered row, none from the "b" group
    CHECK(slurp(out).find(",b,") == std::string::npos);
}

TEST(store_roundtrip_never_writes_passwords)
{
    fs::path dir = tmp("store");
    std::error_code ec; fs::remove_all(dir, ec);
    Store s(dir.string());
    s.settings.page_size = 250;
    s.settings.monitor_interval = 10;
    Profile p; p.id = Store::new_id(); p.name = "Prod"; p.engine = "postgres"; p.host = "h"; p.port = 5432; p.user = "u"; p.env = 3;
    s.upsert(p);
    CHECK(s.save());
    Store s2(dir.string());
    s2.load();
    CHECK_EQ(s2.profiles.size(), (size_t)1);
    CHECK_EQ(s2.profiles[0].env, 3);
    CHECK(s2.profiles[0].tls); // TLS validation defaults on, trust-cert defaults off
    CHECK(!s2.profiles[0].trust_server_cert);
    CHECK_EQ(s2.settings.page_size, 250);
    CHECK_EQ(s2.settings.monitor_interval, 10);
    std::string all = slurp(dir / "connections.json") + slurp(dir / "settings.json");
    CHECK(all.find("password") == std::string::npos);
    CHECK(s2.export_profiles_json().find("password") == std::string::npos);
}

TEST(store_survives_a_corrupt_file)
{
    fs::path dir = tmp("store_bad");
    std::error_code ec; fs::remove_all(dir, ec);
    fs::create_directories(dir);
    { std::ofstream f(dir / "connections.json"); f << "{ not json"; }
    Store s(dir.string());
    s.load();
    CHECK(s.profiles.empty());
}

TEST(imported_profiles_get_new_ids)
{
    Store a(tmp("store_a").string());
    Profile p; p.id = "fixed"; p.name = "n"; p.engine = "sqlite";
    a.upsert(p);
    Store b(tmp("store_b").string());
    CHECK_EQ(b.import_profiles_json(a.export_profiles_json()), 1);
    CHECK(b.profiles[0].id != "fixed");
}

TEST(credential_vault_roundtrip_and_delete)
{
    std::string key = "unit-test-" + Store::new_id();
    CHECK(platform::cred_write(key, "user", "pässword-ñ-123"));
    auto got = platform::cred_read(key);
    CHECK(got.has_value());
    if (got) CHECK_EQ(*got, std::string("pässword-ñ-123"));
    platform::cred_delete(key);
    CHECK(!platform::cred_read(key).has_value());
}
