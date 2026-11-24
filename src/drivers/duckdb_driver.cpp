#include <cstring>
#include <string>

#include <duckdb.h>

#include "../connectors/duckdb_conn.h"
#include "driver_api.h"

DBPRO_EXPORT int dbpro_driver_abi() { return DBPRO_DRIVER_ABI; }

DBPRO_EXPORT const char *dbpro_driver_info()
{
    static const std::string info = std::string("{\"id\":\"duckdb\",\"engines\":[\"duckdb\"],\"abi\":") + std::to_string(DBPRO_DRIVER_ABI)
                                    + ",\"app_version\":\"" APP_VERSION "\",\"library\":\"DuckDB " + duckdb_library_version() + "\"}";
    return info.c_str();
}

DBPRO_EXPORT void *dbpro_driver_connect(const char *, const DbproConnectParams *p, int *err_kind, char *err, size_t err_size)
{
    auto fail = [&](int kind, const char *msg) -> void * {
        *err_kind = kind;
        if (err_size) {
            std::strncpy(err, msg, err_size - 1);
            err[err_size - 1] = 0;
        }
        return nullptr;
    };
    try {
        return dbp::connect_duckdb(p->path ? p->path : "", p->read_only != 0).release();
    } catch (const dbp::DbError &e) {
        return fail((int)e.kind, e.what());
    } catch (const std::exception &e) {
        return fail((int)dbp::ErrKind::Other, e.what());
    }
}