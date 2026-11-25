#include <filesystem>
#include <fstream>

#include "column_layouts.h"
#include "test.h"

using namespace dbp;
namespace fs = std::filesystem;

static const std::vector<std::string> cols = { "id", "name", "email", "created" };

TEST(layout_default_shows_all_in_natural_order)
{
    ColumnView v = resolve_layout({}, cols);
    CHECK_EQ(v.order.size(), (size_t)4);
    for (size_t k = 0; k < 4; ++k) { CHECK_EQ(v.order[k], k); CHECK(!v.hidden[k]); }
}

TEST(layout_applies_order_hidden_and_ignores_unknown_names)
{
    ColumnLayout l;
    l.order = { "email", "ghost", "id" };
    l.hidden = { "name", "ghost" };
    ColumnView v = resolve_layout(l, cols);
    CHECK_EQ(v.order.size(), (size_t)4);
    CHECK_EQ(v.order[0], (size_t)2); // email
    CHECK_EQ(v.order[1], (size_t)0); // id
    CHECK_EQ(v.order[2], (size_t)1); // unlisted ones follow in natural order
    CHECK_EQ(v.order[3], (size_t)3);
    CHECK(v.hidden[1] && !v.hidden[0] && !v.hidden[2] && !v.hidden[3]);
}

TEST(layout_never_hides_every_column)
{
    ColumnLayout l;
    l.order = { "email" };
    l.hidden = cols;
    ColumnView v = resolve_layout(l, cols);
    int shown = 0;
    for (bool h : v.hidden) shown += h ? 0 : 1;
    CHECK_EQ(shown, 1);
    CHECK(!v.hidden[2]); // the first one in display order
}

TEST(layout_roundtrips_through_view_and_natural_layout_is_empty)
{
    CHECK(layout_from_view(resolve_layout({}, cols), cols).order.empty());
    ColumnLayout l;
    l.order = { "created", "id" };
    l.hidden = { "email" };
    ColumnLayout back = layout_from_view(resolve_layout(l, cols), cols);
    CHECK_EQ(back.order.size(), (size_t)4);
    CHECK(back.order[0] == "created" && back.order[1] == "id");
    CHECK(back.hidden.size() == 1 && back.hidden[0] == "email");
}

TEST(layout_persists_per_table_and_survives_corrupt_file)
{
    fs::path d = fs::temp_directory_path() / "dbpro_layout_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    std::string a = ColumnLayouts::make_key("p1", "", "main", "customers");
    std::string b = ColumnLayouts::make_key("p2", "", "main", "customers");
    {
        ColumnLayouts s(d.string());
        ColumnLayout l;
        l.hidden = { "email" };
        s.set(a, l);
        CHECK(s.save());
    }
    ColumnLayouts s2(d.string());
    s2.load();
    CHECK_EQ(s2.get(a).hidden.size(), (size_t)1);
    CHECK(s2.get(b).hidden.empty()); // another connection is unaffected
    s2.set(a, {});
    CHECK(s2.get(a).hidden.empty());
    std::ofstream(d / "column_layouts.json", std::ios::trunc) << "{oops";
    ColumnLayouts s3(d.string());
    s3.load();
    CHECK(s3.get(a).hidden.empty());
}
