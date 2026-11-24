#include <set>

#include "core/engines.h"
#include "test.h"

using namespace dbp;

TEST(engine_ids_are_unique_and_lowercase)
{
    std::set<std::string> seen;
    for (auto &e : engine_catalog()) {
        CHECK(seen.insert(e.id).second);
        std::string id = e.id;
        for (char c : id) CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
        CHECK(*e.label != 0);
    }
}

TEST(every_engine_has_a_real_route)
{
    std::set<std::string> known = { "", "postgres", "mysql", "mssql", "mongodb", "duckdb" };
    for (auto &e : engine_catalog()) {
        CHECK(known.count(e.driver) == 1);
        CHECK((std::string(e.id) == "sqlite") == (*e.driver == 0)); // only SQLite is built in
    }
}

TEST(only_engines_tested_against_a_real_server_are_marked_tested)
{
    // Keep this list in sync with docs/PROGRESS.md: adding an id here requires integration results.
    std::set<std::string> tested = { "sqlite", "postgres", "mariadb", "mongodb", "cockroachdb" };
    for (auto &e : engine_catalog()) {
        bool t = e.status == EngineStatus::Tested;
        CHECK(t == (tested.count(e.id) == 1));
        if (!t) CHECK(*e.note != 0); // an experimental engine always explains why
    }
}

TEST(engine_lookup_and_filter)
{
    CHECK(find_engine("postgres") != nullptr);
    CHECK(find_engine("nonsense") == nullptr);
    CHECK(filter_engines("").size() == engine_catalog().size());
    auto r = filter_engines("POSTGRES");
    CHECK(!r.empty());
    for (auto *e : r) CHECK(std::string(e->id).find("postgres") != std::string::npos || std::string(e->label).find("Postgres") != std::string::npos || std::string(e->label).find("PostgreSQL") != std::string::npos);
    CHECK(filter_engines("zzzz").empty());
}
