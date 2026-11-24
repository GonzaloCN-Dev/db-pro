#include "core/sqlsafety.h"
#include "test.h"

using namespace dbp;

TEST(safety_selects_are_read_only)
{
    CHECK(statement_is_read_only("SELECT * FROM t"));
    CHECK(statement_is_read_only("  select 1; select 2;"));
    CHECK(statement_is_read_only("-- note\nSELECT 1"));
    CHECK(statement_is_read_only("EXPLAIN SELECT 1"));
    CHECK(statement_is_read_only("WITH a AS (SELECT 1) SELECT * FROM a"));
}

TEST(safety_writes_and_unknown_need_confirmation)
{
    CHECK(!statement_is_read_only("DELETE FROM t"));
    CHECK(!statement_is_read_only("select 1; drop table t"));
    CHECK(!statement_is_read_only("UPDATE t SET a=1"));
    CHECK(!statement_is_read_only("CREATE TABLE x(a)"));
    CHECK(!statement_is_read_only("WITH a AS (SELECT 1) DELETE FROM t"));
    CHECK(!statement_is_read_only("SELECT * INTO copy FROM t"));
    CHECK(!statement_is_read_only("EXPLAIN ANALYZE DELETE FROM t"));
    CHECK(!statement_is_read_only("PRAGMA writable_schema=1"));
    CHECK(!statement_is_read_only(""));
}

TEST(safety_ignores_keywords_in_strings_and_comments)
{
    CHECK(statement_is_read_only("SELECT 'delete from t; drop table x'"));
    CHECK(statement_is_read_only("SELECT 1 /* drop table t */"));
    CHECK(!statement_is_read_only("/* select */ DROP TABLE t"));
}
