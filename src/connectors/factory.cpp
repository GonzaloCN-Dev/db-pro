#include "factory.h"

#include "../drivers/driver_manager.h"
#include "sqlite_conn.h"

namespace dbp {

bool engine_available(const std::string &id)
{
    return drivers::status_for_engine(id).usable();
}

std::unique_ptr<Connector> make_connector(const Profile &p, const std::string &password)
{
    if (p.engine == "sqlite") {
        if (p.path.empty()) throw DbError(ErrKind::Connection, "No database file selected");
        return connect_sqlite(p.path, p.read_only);
    }
    return drivers::connect(p, password);
}

} // namespace dbp
