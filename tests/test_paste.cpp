#include "core/paste.h"
#include "test.h"

using namespace dbp;

static ColumnInfo col(const char *n, TypeClass c, bool nullable = true)
{
    ColumnInfo k;
    k.name = n;
    k.cls = c;
    k.nullable = nullable;
    return k;
}

TEST(tsv_parses_rows_trailing_newline_and_crlf)
{
    std::vector<std::vector<std::string>> r;
    std::string err;
    CHECK(parse_tsv("a\tb\r\nc\td\r\n", r, &err));
    CHECK_EQ(r.size(), (size_t)2);
    CHECK(r[0][0] == "a" && r[1][1] == "d");
    CHECK(parse_tsv("", r, &err) && r.empty());
    CHECK(parse_tsv("x", r, &err) && r.size() == 1);
}

TEST(tsv_handles_excel_quotes_tabs_and_newlines)
{
    std::vector<std::vector<std::string>> r;
    std::string err;
    CHECK(parse_tsv("\"line1\nline2\"\t\"say \"\"hi\"\"\"\t\"a\tb\"\n", r, &err));
    CHECK_EQ(r.size(), (size_t)1);
    CHECK_EQ(r[0].size(), (size_t)3);
    CHECK(r[0][0] == "line1\nline2" && r[0][1] == "say \"hi\"" && r[0][2] == "a\tb");
}

TEST(tsv_keeps_empty_cells_and_rejects_bad_input)
{
    std::vector<std::vector<std::string>> r;
    std::string err;
    CHECK(parse_tsv("a\t\tc\n", r, &err));
    CHECK(r[0].size() == 3 && r[0][1].empty());
    CHECK(!parse_tsv("\"open\tnever closed", r, &err) && err == "paste.bad-quote");
    std::string many;
    for (size_t i = 0; i < kMaxPasteRows + 5; ++i) many += "x\n";
    CHECK(!parse_tsv(many, r, &err) && err == "paste.too-many");
    CHECK(!parse_tsv(std::string(kMaxPasteBytes + 1, 'x'), r, &err) && err == "paste.too-big");
}

TEST(paste_plan_validates_each_cell_like_typing)
{
    std::vector<ColumnInfo> cols = { col("id", TypeClass::Integer, false), col("name", TypeClass::Text), col("when", TypeClass::Integer) };
    std::vector<size_t> targets = { 0, 1, 2 };
    auto ok = plan_paste(cols, targets, { { "1", "Ana", "" }, { "2", "", "7" } });
    CHECK(ok.ok());
    CHECK(ok.values[0][2].is_null()); // empty non-text cell on a nullable column -> NULL
    CHECK(ok.values[1][1].kind == Value::Kind::Text && ok.values[1][1].s.empty());

    auto bad = plan_paste(cols, targets, { { "x", "Ana", "1" }, { "", "B", "2" } });
    CHECK(!bad.ok());
    CHECK_EQ(bad.issues.size(), (size_t)2);
    CHECK(bad.issues[0].row == 0 && bad.issues[0].col == 0);
    CHECK(bad.issues[1].row == 1 && bad.issues[1].message == "paste.empty-not-null");
}

TEST(paste_plan_flags_extra_cells_and_readonly_columns)
{
    ColumnInfo ro = col("calc", TypeClass::Integer);
    ro.generated = true;
    std::vector<ColumnInfo> cols = { col("a", TypeClass::Text), ro };
    auto p = plan_paste(cols, { 0 }, { { "x", "y" } });
    CHECK(!p.ok());
    CHECK(p.issues[0].message == "paste.extra-cells");
    auto q = plan_paste(cols, { 0, 1 }, { { "x", "5" } });
    CHECK(!q.ok() && q.issues[0].message == "paste.readonly-col");
    CHECK(!plan_paste(cols, { 0 }, {}).ok());
}
