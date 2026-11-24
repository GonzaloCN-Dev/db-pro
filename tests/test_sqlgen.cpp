#include "core/sqlgen.h"
#include "test.h"

using namespace dbp;

static TableMeta people()
{
    TableMeta m;
    m.table = { "", "", "people" };
    ColumnInfo id; id.name = "id"; id.cls = TypeClass::Integer; id.pk_position = 1;
    ColumnInfo nm; nm.name = "name"; nm.cls = TypeClass::Text;
    ColumnInfo amt; amt.name = "amount"; amt.cls = TypeClass::Decimal;
    m.columns = { id, nm, amt };
    m.edit = { { "id" }, true, "" };
    return m;
}

TEST(injection_never_reaches_sql_text)
{
    TableMeta m = people();
    PageRequest r;
    Condition c; c.column = "name"; c.op = Op::Contains; c.values = { Value::text("'; DROP TABLE people; --") };
    r.filter.conds.push_back(c);
    for (Dialect d : { Dialect::Sqlite, Dialect::Postgres, Dialect::MySql, Dialect::MsSql }) {
        Built b = build_select(d, m, r);
        CHECK(b.sql.find("DROP") == std::string::npos);
        CHECK_EQ(b.params.size(), (size_t)1);
    }
}

TEST(unknown_column_is_rejected)
{
    TableMeta m = people();
    PageRequest r;
    Condition c; c.column = "name; DROP TABLE x"; c.op = Op::Equals; c.values = { Value::text("a") };
    r.filter.conds.push_back(c);
    bool threw = false;
    try { build_select(Dialect::Sqlite, m, r); } catch (const DbError &e) { threw = e.kind == ErrKind::Validation; }
    CHECK(threw);
}

TEST(like_wildcards_are_escaped)
{
    CHECK_EQ(like_escape("50%_off!"), std::string("50!%!_off!!"));
}

TEST(identifiers_are_delimited_per_dialect)
{
    CHECK_EQ(quote_ident(Dialect::MySql, "a`b"), std::string("`a``b`"));
    CHECK_EQ(quote_ident(Dialect::MsSql, "a]b"), std::string("[a]]b]"));
    CHECK_EQ(quote_ident(Dialect::Postgres, "a\"b"), std::string("\"a\"\"b\""));
}

TEST(postgres_uses_numbered_placeholders)
{
    TableMeta m = people();
    PageRequest r;
    Condition a; a.column = "name"; a.op = Op::Equals; a.values = { Value::text("x") };
    Condition b; b.column = "amount"; b.op = Op::Between; b.values = { Value::text("1"), Value::text("2") };
    r.filter.conds = { a, b };
    Built q = build_select(Dialect::Postgres, m, r);
    CHECK(q.sql.find("$1") != std::string::npos);
    CHECK(q.sql.find("$3") != std::string::npos);
    CHECK_EQ(q.params.size(), (size_t)3);
}

TEST(paging_is_deterministic_and_fetches_one_extra)
{
    TableMeta m = people();
    PageRequest r;
    r.limit = 100;
    r.sort = { { "name", false } };
    Built q = build_select(Dialect::Sqlite, m, r);
    CHECK(q.sql.find("ORDER BY \"name\" ASC, \"id\" ASC") != std::string::npos);
    CHECK(q.sql.find("LIMIT 101") != std::string::npos);
    Built ms = build_select(Dialect::MsSql, m, r);
    CHECK(ms.sql.find("OFFSET 0 ROWS FETCH NEXT 101 ROWS ONLY") != std::string::npos);
}

TEST(or_groups_are_parenthesised)
{
    TableMeta m = people();
    PageRequest r;
    Condition a; a.column = "name"; a.op = Op::Equals; a.values = { Value::text("x") };
    FilterGroup g; g.logic = Logic::Or; g.conds = { a, a };
    r.filter.conds = { a };
    r.filter.groups = { g };
    Built q = build_select(Dialect::Sqlite, m, r);
    CHECK(q.sql.find("AND (\"name\" = ? OR \"name\" = ?)") != std::string::npos);
}

TEST(update_carries_concurrency_check_and_key)
{
    TableMeta m = people();
    Change ch; ch.type = Change::Type::Update;
    ch.key = { { "id", Value::integer(7) } };
    ch.set = { { "name", Value::text("new"), Value::text("old") } };
    auto st = build_changes(Dialect::Sqlite, m, { ch });
    CHECK_EQ(st.size(), (size_t)1);
    CHECK(st[0].must_affect_one);
    CHECK(st[0].q.sql.find("WHERE \"id\" = ? AND \"name\" = ?") != std::string::npos);
    CHECK_EQ(st[0].q.params.size(), (size_t)3);
}

TEST(update_original_null_checks_is_null)
{
    TableMeta m = people();
    Change ch; ch.type = Change::Type::Update;
    ch.key = { { "id", Value::integer(1) } };
    ch.set = { { "name", Value::text("x"), Value::null() } };
    auto st = build_changes(Dialect::Sqlite, m, { ch });
    CHECK(st[0].q.sql.find("\"name\" IS NULL") != std::string::npos);
}

TEST(row_without_key_cannot_be_deleted)
{
    TableMeta m = people();
    Change ch; ch.type = Change::Type::Delete;
    bool threw = false;
    try { build_changes(Dialect::Sqlite, m, { ch }); } catch (const DbError &e) { threw = e.kind == ErrKind::Validation; }
    CHECK(threw);
}

TEST(regex_unsupported_on_sqlite_and_mssql)
{
    TableMeta m = people();
    PageRequest r;
    Condition c; c.column = "name"; c.op = Op::Regex; c.values = { Value::text("^a") };
    r.filter.conds = { c };
    for (Dialect d : { Dialect::Sqlite, Dialect::MsSql }) {
        bool threw = false;
        try { build_select(d, m, r); } catch (const DbError &e) { threw = e.kind == ErrKind::Unsupported; }
        CHECK(threw);
    }
    CHECK(build_select(Dialect::Postgres, m, r).sql.find("~*") != std::string::npos);
}

TEST(distinct_is_always_bounded)
{
    TableMeta m = people();
    DistinctRequest r; r.column = "name"; r.limit = 5000000;
    CHECK(build_distinct(Dialect::Sqlite, m, r).sql.find("LIMIT 1000") != std::string::npos);
    CHECK(build_distinct(Dialect::MsSql, m, r).sql.find("TOP (1000)") != std::string::npos);
}

TEST(qualified_names_per_dialect)
{
    TableRef t { "shop", "sales", "orders" };
    CHECK_EQ(qualified_table(Dialect::Postgres, t), std::string("\"sales\".\"orders\""));
    CHECK_EQ(qualified_table(Dialect::MySql, t), std::string("`shop`.`orders`"));
    CHECK_EQ(qualified_table(Dialect::MsSql, t), std::string("[shop].[sales].[orders]"));
    TableRef d { "shop", "", "orders" };
    CHECK_EQ(qualified_table(Dialect::MsSql, d), std::string("[shop].[dbo].[orders]"));
}

TEST(count_and_distinct_per_dialect)
{
    TableMeta m = people();
    CHECK(build_count(Dialect::MsSql, m, FilterGroup()).sql.find("COUNT_BIG(*)") != std::string::npos);
    CHECK(build_count(Dialect::Postgres, m, FilterGroup()).sql.find("COUNT(*)") != std::string::npos);
    Condition c; c.column = "name"; c.op = Op::Equals; c.values = { Value::text("x") };
    FilterGroup g; g.conds = { c };
    Built q = build_count(Dialect::Postgres, m, g);
    CHECK(q.sql.find("$1") != std::string::npos && q.params.size() == 1);
}
