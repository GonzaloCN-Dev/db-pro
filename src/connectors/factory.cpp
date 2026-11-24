#include "factory.h"

#include "mysql_conn.h"
#include "sqlite_conn.h"

namespace dbp {

bool engine_available(const std::string &id)
{
    return id == "sqlite" || id == "mysql" || id == "mariadb";
}

std::unique_ptr<Connector> make_connector(const Profile &p, const std::string &password)
{
    if (p.engine == "sqlite") {
        if (p.path.empty()) throw DbError(ErrKind::Connection, "No database file selected");
        return connect_sqlite(p.path, p.read_only);
    }
    if (p.engine == "mysql" || p.engine == "mariadb") {
        MySqlParams m;
        m.host = p.host.empty() ? "localhost" : p.host;
        m.port = p.port > 0 ? (unsigned)p.port : 3306;
        m.user = p.user;
        m.password = password;
        m.database = p.database;
        m.tls = p.tls;
        m.trust_server_cert = p.trust_server_cert;
        m.read_only = p.read_only;
        return connect_mysql(m);
    }
    throw DbError(ErrKind::Unsupported, "Engine not available in this build: " + p.engine);
}

} // namespace dbp
