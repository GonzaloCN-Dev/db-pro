// Independent check tool for tests: dbpro_sqlq <file> "<sql>"  (opens read-only, prints rows tab-separated)
#include <sqlite3.h>

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: dbpro_sqlq <file> <sql>\n");
        return 2;
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return 1;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, argv[2], -1, &st, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "%s\n", sqlite3_errmsg(db));
        return 1;
    }
    int n = sqlite3_column_count(st);
    while (sqlite3_step(st) == SQLITE_ROW) {
        for (int i = 0; i < n; ++i) {
            const unsigned char *t = sqlite3_column_text(st, i);
            std::printf("%s%s", i ? "\t" : "", t ? (const char *)t : "NULL");
        }
        std::printf("\n");
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}
