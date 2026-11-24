#include "test.h"
#include "core/activity_view.h"

using namespace dbp;

static Activity sample_activity()
{
    Activity a;
    a.columns = { "id", "user", "state", "blocked_by", "seconds" };
    a.id_col = 0;
    a.blocked_col = 3;
    a.rows = {
        { Value::integer(8), Value::text("Worker"), Value::text("active"), Value::text(""), Value::text("12.5") },
        { Value::integer(2), Value::text("alice"), Value::text("waiting"), Value::text("8"), Value::text("3") },
        { Value::integer(5), Value::text("BOB"), Value::text("idle"), Value::text("?"), Value::text("3") },
        { Value::integer(10), Value::text("Carol"), Value::text("active"), Value::null(), Value::text("1") },
    };
    return a;
}

TEST(activity_view_search_is_case_insensitive_and_empty_search_keeps_all)
{
    Activity a = sample_activity();
    ActivityViewOptions o;
    CHECK_EQ(activity_visible_rows(a, o).size(), size_t(4));
    o.search = "ALICE";
    auto rows = activity_visible_rows(a, o);
    CHECK_EQ(rows.size(), size_t(1));
    CHECK_EQ(rows[0], size_t(1));
}

TEST(activity_view_blocked_filter_includes_blockers_and_unknown_waits)
{
    Activity a = sample_activity();
    ActivityViewOptions o;
    o.blocked_only = true;
    auto rows = activity_visible_rows(a, o);
    CHECK_EQ(rows.size(), size_t(2));
    CHECK_EQ(rows[0], size_t(1));
    CHECK_EQ(rows[1], size_t(2));
}

TEST(activity_view_numeric_sort_is_stable_and_missing_last)
{
    Activity a = sample_activity();
    a.rows[3][4] = Value::null();
    ActivityViewOptions o;
    o.sort_column = 4;
    auto rows = activity_visible_rows(a, o);
    CHECK_EQ(rows.size(), size_t(4));
    CHECK_EQ(rows[0], size_t(1));
    CHECK_EQ(rows[1], size_t(2));
    CHECK_EQ(rows[3], size_t(3));
}

TEST(activity_view_descending_sort_preserves_nulls_last)
{
    Activity a = sample_activity();
    a.rows[3][4] = Value::null();
    ActivityViewOptions o;
    o.sort_column = 4;
    o.ascending = false;
    auto rows = activity_visible_rows(a, o);
    CHECK_EQ(rows[0], size_t(0));
    CHECK_EQ(rows[3], size_t(3));
}
