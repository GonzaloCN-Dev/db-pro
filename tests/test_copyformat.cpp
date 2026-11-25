#include <nlohmann/json.hpp>
#include "core/copyformat.h"
#include "test.h"

using namespace dbp;

static std::vector<ColumnInfo> two_cols()
{
    ColumnInfo a, b;
    a.name = "id";
    b.name = "na\"me";
    return { a, b };
}

TEST(copy_tsv_has_header_and_keeps_null_empty)
{
    std::vector<std::vector<Value>> rows = { { Value::integer(1), Value::text("Ana") }, { Value::integer(2), Value::null() } };
    auto r = format_rows(two_cols(), rows, CopyFormat::Tsv, "t", Dialect::Sqlite, true);
    CHECK(r.text == "id\tna\"\"me\n1\tAna\n2\t\n" || r.text == "id\t\"na\"\"me\"\n1\tAna\n2\t\n");
    CHECK_EQ(r.rows, (size_t)2);
    auto nh = format_rows(two_cols(), rows, CopyFormat::Tsv, "t", Dialect::Sqlite, false);
    CHECK(nh.text == "1\tAna\n2\t\n");
}

TEST(copy_csv_quotes_and_neutralises_formulas)
{
    std::vector<std::vector<Value>> rows = { { Value::integer(1), Value::text("a,b") }, { Value::integer(2), Value::text("=SUM(A1)") } };
    auto r = format_rows(two_cols(), rows, CopyFormat::Csv, "t", Dialect::Sqlite, false);
    CHECK(r.text == "1,\"a,b\"\r\n2,'=SUM(A1)\r\n");
}

TEST(copy_json_is_valid_and_keeps_types)
{
    std::vector<std::vector<Value>> rows = { { Value::integer(7), Value::null() } };
    auto r = format_rows(two_cols(), rows, CopyFormat::Json, "t", Dialect::Sqlite, true);
    auto j = nlohmann::json::parse(r.text);
    CHECK(j.is_array() && j.size() == 1 && j[0]["id"] == 7 && j[0]["na\"me"].is_null());
    CHECK(format_rows(two_cols(), {}, CopyFormat::Json, "t", Dialect::Sqlite, true).text == "[]");
}

TEST(copy_insert_quotes_per_dialect_and_escapes_values)
{
    std::vector<std::vector<Value>> rows = { { Value::integer(1), Value::text("O'Neil") } };
    ColumnInfo a, b;
    a.name = "id";
    b.name = "name";
    std::vector<ColumnInfo> cols = { a, b };
    CHECK(format_rows(cols, rows, CopyFormat::SqlInsert, "people", Dialect::MySql, true).text == "INSERT INTO `people` (`id`, `name`) VALUES (1, 'O''Neil');\n");
    CHECK(format_rows(cols, rows, CopyFormat::SqlInsert, "people", Dialect::MsSql, true).text == "INSERT INTO [people] ([id], [name]) VALUES (1, 'O''Neil');\n");
    CHECK(format_rows(cols, rows, CopyFormat::SqlInsert, "people", Dialect::Postgres, true).text == "INSERT INTO \"people\" (\"id\", \"name\") VALUES (1, 'O''Neil');\n");
}

TEST(copy_insert_rejects_bad_identifiers)
{
    ColumnInfo a;
    a.name = std::string("bad\0name", 8);
    bool threw = false;
    try { format_rows({ a }, { { Value::integer(1) } }, CopyFormat::SqlInsert, "t", Dialect::Sqlite, true); }
    catch (const std::exception &) { threw = true; }
    CHECK(threw);
}

TEST(copy_is_bounded_and_reports_truncation)
{
    std::vector<std::vector<Value>> rows(1000, { Value::integer(1), Value::text(std::string(100, 'x')) });
    auto r = format_rows(two_cols(), rows, CopyFormat::Tsv, "t", Dialect::Sqlite, false, 5000);
    CHECK(r.truncated);
    CHECK(r.text.size() <= 5000);
    CHECK(r.rows > 0 && r.rows < 1000);
}
