#include <filesystem>
#include <fstream>

#include "query_history.h"
#include "test.h"

using namespace dbp;
namespace fs = std::filesystem;

static fs::path hist_dir(const char *n)
{
    fs::path d = fs::temp_directory_path() / (std::string("dbpro_hist_") + n);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

TEST(history_records_and_moves_repeats_to_the_top)
{
    QueryHistory h(hist_dir("repeat").string());
    CHECK(h.record("SELECT 1", 100));
    CHECK(h.record("SELECT 2", 200));
    CHECK(h.record("  SELECT 1;\n", 300) == true); // different text (semicolon), kept separate
    CHECK(h.record("SELECT 1", 400));              // exact repeat
    CHECK_EQ(h.entries().size(), (size_t)3);
    CHECK(h.entries()[0].text == "SELECT 1");
    CHECK_EQ(h.entries()[0].runs, 2);
    CHECK_EQ(h.entries()[0].last_used, (long long)400);
}

TEST(history_skips_blank_oversized_and_secret_statements)
{
    QueryHistory h(hist_dir("skip").string());
    CHECK(!h.record("   \n\t ", 1));
    CHECK(!h.record(std::string(QueryHistory::kMaxChars + 1, 'x'), 1));
    CHECK(!h.record("CREATE USER bob IDENTIFIED BY 'hunter2'", 1));
    CHECK(!h.record("ALTER ROLE x WITH PASSWORD 'abc'", 1));
    CHECK(!h.record("select * from tokens", 1)); // conservative: a word match is enough
    CHECK(h.entries().empty());
}

TEST(history_is_capped_but_favorites_survive)
{
    QueryHistory h(hist_dir("cap").string());
    h.record("KEEP ME", 1);
    CHECK(h.set_favorite(0, true));
    for (int i = 0; i < 500; ++i) h.record("SELECT " + std::to_string(i), 10 + i);
    size_t plain = 0, fav = 0;
    for (auto &e : h.entries()) (e.favorite ? fav : plain)++;
    CHECK_EQ(fav, (size_t)1);
    CHECK_EQ(plain, QueryHistory::kMaxPlain);
    CHECK(h.entries().back().favorite || h.view("KEEP", true).size() == 1);
    CHECK_EQ(h.entries().front().text, std::string("SELECT 499"));
}

TEST(history_search_and_favorite_filter)
{
    QueryHistory h(hist_dir("view").string());
    h.record("SELECT name FROM Customers", 1);
    h.record("DELETE FROM logs", 2);
    h.record("select count(*) from customers", 3);
    CHECK_EQ(h.view("CUSTOMERS", false).size(), (size_t)2);
    CHECK_EQ(h.view("", false).size(), (size_t)3);
    CHECK_EQ(h.view("zzz", false).size(), (size_t)0);
    size_t idx = h.view("logs", false)[0];
    h.set_favorite(idx, true);
    auto favs = h.view("", true);
    CHECK_EQ(favs.size(), (size_t)1);
    CHECK(h.entries()[favs[0]].text == "DELETE FROM logs");
}

TEST(history_persists_and_survives_a_corrupt_file)
{
    auto d = hist_dir("persist");
    {
        QueryHistory h(d.string());
        h.record("SELECT 'ñ'", 5);
        h.record("SELECT 2", 6);
        h.set_favorite(1, true);
        CHECK(h.save());
    }
    QueryHistory h2(d.string());
    h2.load();
    CHECK_EQ(h2.entries().size(), (size_t)2);
    CHECK(h2.entries()[0].text == "SELECT 2");
    bool any_fav = false;
    for (auto &e : h2.entries()) if (e.favorite && e.text == "SELECT 'ñ'") any_fav = true;
    CHECK(any_fav);

    std::ofstream(d / "query_history.json", std::ios::trunc) << "{not json";
    QueryHistory h3(d.string());
    h3.record("x", 1);
    h3.load();
    CHECK(h3.entries().empty());
}

TEST(history_load_drops_secrets_written_by_hand)
{
    auto d = hist_dir("secrets");
    std::ofstream(d / "query_history.json", std::ios::trunc)
        << R"({"version":1,"entries":[{"text":"SELECT 1","last_used":1},{"text":"SET PASSWORD = 'x'","last_used":2}]})";
    QueryHistory h(d.string());
    h.load();
    CHECK_EQ(h.entries().size(), (size_t)1);
    CHECK(h.entries()[0].text == "SELECT 1");
}

TEST(history_clear_keeps_favorites_and_remove_checks_bounds)
{
    QueryHistory h(hist_dir("clear").string());
    h.record("A1", 1);
    h.record("B2", 2);
    h.set_favorite(h.view("A1", false)[0], true);
    h.clear_plain();
    CHECK_EQ(h.entries().size(), (size_t)1);
    CHECK(!h.remove(5));
    CHECK(!h.set_favorite(9, true));
    CHECK(h.remove(0));
    CHECK(h.entries().empty());
}

TEST(history_preview_is_one_line_and_utf8_safe)
{
    CHECK(QueryHistory::preview("SELECT  a,\n   b\tFROM   t") == "SELECT a, b FROM t");
    std::string long_text(300, 'a');
    std::string p = QueryHistory::preview(long_text, 50);
    CHECK(p.size() <= 53);
    CHECK(p.substr(p.size() - 3) == "...");
    std::string accents;
    for (int i = 0; i < 100; ++i) accents += "ñ";
    std::string q = QueryHistory::preview(accents, 51);
    CHECK(q.substr(q.size() - 3) == "...");
    std::string body = q.substr(0, q.size() - 3);
    CHECK(body.size() % 2 == 0); // never a lone continuation byte
}
