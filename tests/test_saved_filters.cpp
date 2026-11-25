#include <filesystem>
#include <fstream>

#include "saved_filters.h"
#include "test.h"

using namespace dbp;
namespace fs = std::filesystem;

static SavedFilter make(const char *name)
{
    SavedFilter f;
    f.name = name;
    f.items.push_back({ "country", "contains", "Chile" });
    return f;
}

TEST(saved_filters_put_replaces_same_name_and_trims)
{
    SavedFilters s(fs::temp_directory_path().string());
    std::string k = SavedFilters::make_key("p", "", "main", "t");
    CHECK(s.put(k, make("  Chile  ")));
    CHECK(s.put(k, make("Chile")));
    CHECK_EQ(s.list(k).size(), (size_t)1);
    CHECK(s.list(k)[0].name == "Chile");
    CHECK(!s.put(k, make("   ")));
}

TEST(saved_filters_limit_and_remove)
{
    SavedFilters s(fs::temp_directory_path().string());
    std::string k = SavedFilters::make_key("p", "", "main", "t");
    for (size_t i = 0; i < kMaxSavedFilters; ++i) CHECK(s.put(k, make(("f" + std::to_string(i)).c_str())));
    CHECK(!s.put(k, make("one more")));
    CHECK(s.put(k, make("f3"))); // replacing is still allowed at the limit
    s.remove(k, 0);
    CHECK_EQ(s.list(k).size(), kMaxSavedFilters - 1);
    s.remove(k, 999); // out of range is ignored
}

TEST(saved_filters_persist_per_table_and_survive_corrupt_file)
{
    fs::path d = fs::temp_directory_path() / "dbpro_savedfilters_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    std::string a = SavedFilters::make_key("p1", "", "main", "t");
    std::string b = SavedFilters::make_key("p2", "", "main", "t");
    {
        SavedFilters s(d.string());
        s.put(a, make("Chile"));
        CHECK(s.save());
    }
    SavedFilters s2(d.string());
    s2.load();
    CHECK_EQ(s2.list(a).size(), (size_t)1);
    CHECK(s2.list(a)[0].items[0].text == "Chile");
    CHECK(s2.list(b).empty());
    std::ofstream(d / "saved_filters.json", std::ios::trunc) << "{oops";
    SavedFilters s3(d.string());
    s3.load();
    CHECK(s3.list(a).empty());
}
