#include <cstring>
#include <string>

#include "../connectors/mssql_conn.h"
#include "driver_api.h"

DBPRO_EXPORT int dbpro_driver_abi() { return DBPRO_DRIVER_ABI; }

DBPRO_EXPORT const char *dbpro_driver_info()
{
    static const std::string info = std::string("{\"id\":\"mssql\",\"engines\":[\"mssql\"],\"abi\":") + std::to_string(DBPRO_DRIVER_ABI)
                                    + ",\"app_version\":\"" APP_VERSION "\",\"library\":\"ODBC Driver 18 for SQL Server (installed separately)\"}";
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
        dbp::MsSqlParams m;
        m.host = p->host && *p->host ? p->host : "localhost";
        m.port = p->port > 0 ? (unsigned)p->port : 1433;
        m.user = p->user ? p->user : "";
        m.password = p->password ? p->password : "";
        m.database = p->database ? p->database : "";
        m.tls = p->tls != 0;
        m.trust_server_cert = p->trust_server_cert != 0;
        m.read_only = p->read_only != 0;
        return dbp::connect_mssql(m).release();
    } catch (const dbp::DbError &e) {
        return fail((int)e.kind, e.what());
    } catch (const std::exception &e) {
        return fail((int)dbp::ErrKind::Other, e.what());
    }
}
