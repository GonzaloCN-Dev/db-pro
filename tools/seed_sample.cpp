// Creates a sample SQLite database for manual testing: dbpro_seed <file> [rows]
#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: dbpro_seed <file> [rows]\n");
        return 2;
    }
    long rows = argc > 2 ? std::atol(argv[2]) : 70000;
    sqlite3 *db = nullptr;
    if (sqlite3_open(argv[1], &db) != SQLITE_OK) return 1;
    const char *schema =
        "DROP TABLE IF EXISTS customers; DROP TABLE IF EXISTS orders; DROP TABLE IF EXISTS notes_no_key;"
        "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT, country TEXT, balance REAL, active INTEGER, created TEXT, note TEXT);"
        "CREATE TABLE orders(order_id INTEGER PRIMARY KEY, customer_id INTEGER, total NUMERIC, status TEXT, placed_at TEXT);"
        "CREATE TABLE notes_no_key(text TEXT);"
        "CREATE VIEW big_customers AS SELECT id, name, balance FROM customers WHERE balance > 9000;";
    char *err = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK) { std::fprintf(stderr, "%s\n", err); return 1; }
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO customers(name,email,country,balance,active,created,note) VALUES(?,?,?,?,?,?,?)", -1, &st, nullptr);
    const char *countries[] = { "Spain", "Chile", "Mexico", "Argentina", "Peru", "Colombia", "Uruguay" };
    for (long i = 1; i <= rows; ++i) {
        std::string name = "Customer " + std::to_string(i);
        std::string mail = "user" + std::to_string(i) + "@example.com";
        char date[32];
        std::snprintf(date, sizeof date, "20%02ld-%02ld-%02ld 10:%02ld:00", 10 + i % 15, 1 + i % 12, 1 + i % 28, i % 60);
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (i % 97 == 0) sqlite3_bind_null(st, 2); else sqlite3_bind_text(st, 2, mail.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, countries[i % 7], -1, SQLITE_STATIC);
        sqlite3_bind_double(st, 4, (double)((i * 7919) % 1000000) / 100.0);
        sqlite3_bind_int(st, 5, i % 3 != 0);
        sqlite3_bind_text(st, 6, date, -1, SQLITE_TRANSIENT);
        if (i % 5 == 0) sqlite3_bind_text(st, 7, "Línea uno\nLínea dos, con \"comillas\", coma y =formula", -1, SQLITE_STATIC); else sqlite3_bind_null(st, 7);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    sqlite3_exec(db, "INSERT INTO orders(customer_id,total,status,placed_at) SELECT id, balance/10, CASE id%3 WHEN 0 THEN 'paid' WHEN 1 THEN 'pending' ELSE 'shipped' END, created FROM customers WHERE id%4=0", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO notes_no_key VALUES('a'),('b'),('c')", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    std::printf("created %s with %ld customers\n", argv[1], rows);
    return 0;
}
