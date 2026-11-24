#include "engines.h"

#include <algorithm>
#include <cctype>

namespace dbp {

namespace {

constexpr auto T = EngineStatus::Tested;
constexpr auto X = EngineStatus::Experimental;

const char *kPgNote = "note.pg";
const char *kMyNote = "note.my";
const char *kMongoNote = "note.mongo";
const char *kTdsNote = "note.tds";

} // namespace

const std::vector<EngineDef> &engine_catalog()
{
    static const std::vector<EngineDef> c = {
        // Built in and fully tested first, then the rest alphabetically by label within each driver family.
        { "sqlite", "SQLite", "", 0, T, "" },
        { "postgres", "PostgreSQL", "postgres", 5432, T, "" },
        { "mariadb", "MariaDB", "mysql", 3306, T, "" },
        { "mysql", "MySQL", "mysql", 3306, X, "note.mysql" },
        { "mssql", "SQL Server", "mssql", 1433, X, "note.mssql" },
        { "mongodb", "MongoDB", "mongodb", 27017, T, "note.mongodb" },
        { "duckdb", "DuckDB", "duckdb", 0, T, "note.duckdb" },

        // PostgreSQL wire protocol
        { "cloudberry", "Apache Cloudberry", "postgres", 5432, X, kPgNote },
        { "cockroachdb", "CockroachDB", "postgres", 26257, T, "note.crdb" },
        { "cratedb", "CrateDB", "postgres", 5432, X, kPgNote },
        { "edb", "EDB Postgres", "postgres", 5444, X, kPgNote },
        { "fujitsupg", "Fujitsu Enterprise Postgres", "postgres", 27500, X, kPgNote },
        { "gaussdb", "GaussDB", "postgres", 8000, X, kPgNote },
        { "greengage", "Greengage", "postgres", 5432, X, kPgNote },
        { "greenplum", "Greenplum", "postgres", 5432, X, kPgNote },
        { "greptimedb", "GreptimeDB", "postgres", 4003, X, kPgNote },
        { "kingbase", "Kingbase", "postgres", 54321, X, kPgNote },
        { "materialize", "Materialize", "postgres", 6875, X, kPgNote },
        { "risingwave", "RisingWave", "postgres", 4566, X, kPgNote },
        { "timescaledb", "TimescaleDB", "postgres", 5432, X, kPgNote },
        { "yellowbrick", "Yellowbrick", "postgres", 5432, X, kPgNote },
        { "yugabytedb", "YugabyteDB (YSQL)", "postgres", 5433, X, kPgNote },

        // MySQL wire protocol
        { "databend", "Databend", "mysql", 3307, X, kMyNote },
        { "doris", "Apache Doris", "mysql", 9030, X, kMyNote },
        { "mysqlndb", "MySQL NDB Cluster", "mysql", 3306, X, kMyNote },
        { "oceanbase", "OceanBase (MySQL mode)", "mysql", 2881, X, kMyNote },
        { "singlestore", "SingleStore", "mysql", 3306, X, kMyNote },
        { "starrocks", "StarRocks", "mysql", 9030, X, kMyNote },
        { "tidb", "TiDB", "mysql", 4000, X, kMyNote },
        { "velodb", "VeloDB", "mysql", 9030, X, kMyNote },

        // TDS protocol
        { "azuresql", "Azure SQL Database", "mssql", 1433, X, kTdsNote },
        { "babelfish", "Babelfish for Aurora PostgreSQL", "mssql", 1433, X, kTdsNote },

        // MongoDB wire protocol
        { "documentdb", "Amazon DocumentDB", "mongodb", 27017, X, "note.documentdb" },
        { "ferretdb", "FerretDB", "mongodb", 27017, X, kMongoNote },
    };
    return c;
}

const EngineDef *find_engine(const std::string &id)
{
    for (auto &e : engine_catalog())
        if (id == e.id) return &e;
    return nullptr;
}

bool is_file_engine(const std::string &id)
{
    const EngineDef *e = find_engine(id);
    return e && e->port == 0;
}

std::vector<const EngineDef *> filter_engines(const std::string &filter)
{
    auto lower = [](std::string s) {
        for (auto &ch : s) ch = (char)std::tolower((unsigned char)ch);
        return s;
    };
    std::string f = lower(filter);
    std::vector<const EngineDef *> out;
    for (auto &e : engine_catalog())
        if (f.empty() || lower(e.label).find(f) != std::string::npos || lower(e.id).find(f) != std::string::npos) out.push_back(&e);
    return out;
}

} // namespace dbp
