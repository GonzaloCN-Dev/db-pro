#include "core/changes.h"
#include "core/filtertext.h"
#include "test.h"

using namespace dbp;

static TableMeta tbl()
{
    TableMeta m;
    m.table = { "main", "", "t" };
    ColumnInfo id; id.name = "id"; id.cls = TypeClass::Integer; id.pk_position = 1; id.identity = true;
    ColumnInfo a; a.name = "a"; a.cls = TypeClass::Text;
    ColumnInfo n; n.name = "n"; n.cls = TypeClass::Integer;
    m.columns = { id, a, n };
    m.edit = { { "id" }, true, "" };
    return m;
}

static ChangeSet::Rows rows()
{
    return { { Value::integer(1), Value::text("x"), Value::integer(10) },
             { Value::integer(2), Value::text("y"), Value::null() } };
}

// Scenario A: edit, then put the original back -> no pending change.
TEST(editing_back_to_original_clears_the_change)
{
    auto r = rows();
    ChangeSet cs;
    cs.set_cell(r, 0, 1, Value::text("changed"));
    CHECK(cs.is_edited(0, 1));
    CHECK_EQ(cs.edit_cells(), (size_t)1);
    cs.set_cell(r, 0, 1, Value::text("x"));
    CHECK(!cs.is_edited(0, 1));
    CHECK(cs.empty());
}

TEST(null_and_empty_string_are_different_edits)
{
    auto r = rows();
    ChangeSet cs;
    cs.set_cell(r, 1, 2, Value::text(""));
    CHECK(cs.is_edited(1, 2));
    cs.set_cell(r, 1, 2, Value::null());
    CHECK(cs.empty());
}

TEST(update_is_keyed_by_original_primary_key_with_original_values)
{
    auto r = rows();
    TableMeta m = tbl();
    ChangeSet cs;
    cs.set_cell(r, 1, 1, Value::text("new"));
    auto ch = cs.build(m, r);
    CHECK_EQ(ch.size(), (size_t)1);
    CHECK(ch[0].type == Change::Type::Update);
    CHECK_EQ(ch[0].key[0].second.i, (int64_t)2);
    CHECK_EQ(ch[0].set[0].original.s, std::string("y"));
}

TEST(delete_insert_and_edit_are_all_emitted)
{
    auto r = rows();
    TableMeta m = tbl();
    ChangeSet cs;
    cs.set_cell(r, 0, 1, Value::text("e"));
    cs.mark_delete(r, 1);
    cs.add_insert(ChangeSet::blank_row(m));
    CHECK_EQ(cs.affected_rows(), (size_t)3);
    CHECK_EQ(cs.build(m, r).size(), (size_t)3);
}

TEST(deleting_an_edited_row_drops_its_update)
{
    auto r = rows();
    TableMeta m = tbl();
    ChangeSet cs;
    cs.set_cell(r, 0, 1, Value::text("e"));
    cs.mark_delete(r, 0);
    auto ch = cs.build(m, r);
    CHECK_EQ(ch.size(), (size_t)1);
    CHECK(ch[0].type == Change::Type::Delete);
}

TEST(undo_and_redo_restore_state)
{
    auto r = rows();
    ChangeSet cs;
    cs.set_cell(r, 0, 1, Value::text("e"));
    cs.mark_delete(r, 1);
    CHECK(cs.undo());
    CHECK(!cs.is_deleted(1));
    CHECK(cs.is_edited(0, 1));
    CHECK(cs.undo());
    CHECK(cs.empty());
    CHECK(cs.redo());
    CHECK(cs.is_edited(0, 1));
}

TEST(duplicate_resets_identity_to_default)
{
    auto r = rows();
    TableMeta m = tbl();
    ChangeSet cs;
    auto d = cs.duplicate_of(m, r, 0);
    CHECK(d[0].kind == Value::Kind::Default);
    CHECK_EQ(d[1].s, std::string("x"));
}

TEST(table_without_key_blocks_build)
{
    auto r = rows();
    TableMeta m = tbl();
    m.edit = { {}, false, "No primary key" };
    ChangeSet cs;
    cs.set_cell(r, 0, 1, Value::text("z"));
    bool threw = false;
    try { cs.build(m, r); } catch (const DbError &e) { threw = e.kind == ErrKind::Validation; }
    CHECK(threw);
}

// ----- filter text parsing
static ColumnInfo col(TypeClass c) { ColumnInfo ci; ci.name = "c"; ci.cls = c; return ci; }

TEST(filter_blank_text_means_no_filter)
{
    std::string err;
    CHECK(!make_condition(col(TypeClass::Text), Op::Contains, "", &err).has_value());
    CHECK(err.empty());
}

TEST(filter_invalid_number_reports_error_instead_of_guessing)
{
    std::string err;
    CHECK(!make_condition(col(TypeClass::Integer), Op::Equals, "12abc", &err).has_value());
    CHECK(!err.empty());
}

TEST(filter_between_and_in_parse_lists)
{
    std::string err;
    auto b = make_condition(col(TypeClass::Integer), Op::Between, "5..10", &err);
    CHECK(b.has_value());
    if (b) CHECK_EQ(b->values.size(), (size_t)2);
    auto in = make_condition(col(TypeClass::Integer), Op::In, "1, 2, 3", &err);
    CHECK(in.has_value());
    if (in) CHECK_EQ(in->values.size(), (size_t)3);
}

TEST(filter_european_date_is_normalised)
{
    std::string err;
    auto c = make_condition(col(TypeClass::Date), Op::Equals, "05/03/2026", &err);
    CHECK(c.has_value());
    if (c) CHECK_EQ(c->values[0].s, std::string("2026-03-05"));
}

TEST(filter_null_operators_need_no_text)
{
    std::string err;
    auto c = make_condition(col(TypeClass::Text), Op::IsNull, "", &err);
    CHECK(c.has_value());
}

TEST(local_matching_is_case_insensitive_by_default)
{
    Condition c; c.column = "c"; c.op = Op::Contains; c.values = { Value::text("SANT") };
    CHECK(matches_local(c, Value::text("Santander")));
    CHECK(!matches_local(c, Value::null()));
}
