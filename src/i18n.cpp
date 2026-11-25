#include "i18n.h"

#include <string_view>
#include <unordered_map>

namespace dbp::i18n {

namespace {

struct Entry {
    const char *key;
    const char *en;
    const char *es;
};

const Entry kTable[] = {
    // generic
    { "cancel", "Cancel", "Cancelar" },
    { "close", "Close", "Cerrar" },
    { "save", "Save", "Guardar" },
    { "discard", "Discard", "Descartar" },
    { "export", "Export", "Exportar" },
    { "loading", "Loading…", "Cargando…" },
    { "ready", "Ready", "Listo" },
    { "ok", "OK", "Aceptar" },
    { "delete", "Delete", "Eliminar" },
    { "view.data", "Data", "Datos" },
    { "badge.readonly", "Read-only", "Solo lectura" },
    { "env.dev", "Development", "Desarrollo" },
    { "env.test", "Testing", "Pruebas" },
    { "env.staging", "Staging", "Preproducción" },
    { "env.prod", "PRODUCTION", "PRODUCCIÓN" },

    // explorer
    { "tree.title", "CONNECTIONS", "CONEXIONES" },
    { "tree.search", "Filter objects…", "Filtrar objetos…" },
    { "tree.empty-title", "No connections", "Sin conexiones" },
    { "tree.empty-text", "Add one with the + button.", "Añade una con el botón +." },
    { "tree.disconnect", "Disconnect", "Desconectar" },
    { "tree.edit", "Edit…", "Editar…" },
    { "tree.delete", "Delete", "Eliminar" },
    { "tree.delete-title", "Delete connection", "Eliminar conexión" },
    { "tree.delete-text", "Delete the connection \"{0}\" and its saved password? The database itself is not touched.", "¿Eliminar la conexión \"{0}\" y su contraseña guardada? La base de datos no se toca." },
    { "tree.tables", "{0} objects", "{0} objetos" },

    // data view
    { "data.not-connected", "Not connected", "Sin conexión" },
    { "data.not-connected-text", "Double-click a connection in the explorer to open it.", "Haz doble clic en una conexión del explorador para abrirla." },
    { "data.pick-table", "Pick a table", "Elige una tabla" },
    { "data.pick-table-text", "Open a table or collection from the explorer to browse its rows.", "Abre una tabla o colección del explorador para ver sus filas." },
    { "grid.empty", "This table has no rows.", "Esta tabla no tiene filas." },
    { "grid.no-match", "No rows match the current filters.", "Ninguna fila coincide con los filtros." },
    { "pager.count", "Count", "Contar" },
    { "pager.rows-per-page", "Rows per page", "Filas por página" },
    { "pager.range", "Rows {0}–{1}", "Filas {0}–{1}" },
    { "pager.empty", "No rows", "Sin filas" },
    { "pager.total-unknown", "total unknown", "total desconocido" },
    { "pager.total-exact", "{0} total", "{0} en total" },
    { "pager.total-est", "≈ {0} total (estimate)", "≈ {0} en total (estimado)" },
    { "pager.counting", "counting…", "contando…" },
    { "pager.filtered-exact", "{0} matching", "{0} coinciden" },

    // editing
    { "pending.summary", "{0} pending change(s): {1} edited, {2} new, {3} deleted", "{0} cambio(s) pendientes: {1} editados, {2} nuevos, {3} eliminados" },
    { "blocked.no-key", "Editing is disabled: this table has no primary key, so a row cannot be identified safely.", "Edición desactivada: la tabla no tiene clave primaria y no se puede identificar una fila con seguridad." },
    { "blocked.read-only", "This connection is read-only.", "Esta conexión es de solo lectura." },
    { "blocked.view", "Views cannot be edited.", "Las vistas no se pueden editar." },
    { "edit.bad-value", "Invalid value: {0}", "Valor no válido: {0}" },
    { "edit.readonly-col", "This column cannot be edited here.", "Esta columna no se puede editar aquí." },
    { "edit.not-editable", "Editing is not available for this table.", "La edición no está disponible para esta tabla." },
    { "save.confirm-title", "Apply changes", "Aplicar cambios" },
    { "save.confirm-text", "Apply {0} change(s) to \"{1}\" in one transaction?", "¿Aplicar {0} cambio(s) a \"{1}\" en una sola transacción?" },
    { "save.confirm-prod", "This is a PRODUCTION connection. Apply {0} change(s) to \"{1}\"?", "Esta es una conexión de PRODUCCIÓN. ¿Aplicar {0} cambio(s) a \"{1}\"?" },
    { "save.apply", "Apply", "Aplicar" },
    { "save.done", "Saved: {0} inserted, {1} updated, {2} deleted.", "Guardado: {0} insertadas, {1} actualizadas, {2} eliminadas." },
    { "save.conflict", "Not saved: a row was changed or removed by someone else. Reload and review. Nothing was applied.", "No guardado: otra persona modificó o eliminó una fila. Recarga y revisa. No se aplicó nada." },
    { "save.failed", "Not saved: {0}. Nothing was applied.", "No guardado: {0}. No se aplicó nada." },
    { "save.delete-warn", "{0} row(s) will be DELETED.", "Se ELIMINARÁN {0} fila(s)." },
    { "save.preview", "Statements that will run:", "Sentencias que se ejecutarán:" },
    { "unsaved.title", "Unsaved changes", "Cambios sin guardar" },
    { "unsaved.text", "You have {0} pending change(s). What do you want to do?", "Tienes {0} cambio(s) pendientes. ¿Qué quieres hacer?" },
    { "unsaved.discard", "Discard changes", "Descartar cambios" },

    // filters
    { "filter.invalid", "Filter \"{0}\": {1}", "Filtro \"{0}\": {1}" },
    { "filter.active", "{0} filter(s)", "{0} filtro(s)" },

    // status
    { "status.connecting", "Connecting to {0}…", "Conectando a {0}…" },
    { "status.connected", "Connected to {0}", "Conectado a {0}" },
    { "status.disconnected", "Disconnected", "Desconectado" },
    { "status.connect-failed", "Could not connect: {0}", "No se pudo conectar: {0}" },
    { "status.loaded", "{0} row(s) in {1} ms", "{0} fila(s) en {1} ms" },
    { "status.query-failed", "Query failed: {0}", "La consulta falló: {0}" },
    { "status.cancelled", "Cancelled", "Cancelado" },
    { "status.copied", "Copied to the clipboard", "Copiado al portapapeles" },
    { "status.lang", "Language changed", "Idioma cambiado" },
    { "status.discarded", "Changes discarded", "Cambios descartados" },

    // connection dialog
    { "conn.new", "New connection", "Nueva conexión" },
    { "conn.edit", "Edit connection", "Editar conexión" },
    { "conn.name", "Name", "Nombre" },
    { "conn.engine", "Engine", "Motor" },
    { "conn.file", "Database file", "Archivo de base de datos" },
    { "conn.browse", "Browse…", "Examinar…" },
    { "conn.host", "Host", "Servidor" },
    { "conn.port", "Port", "Puerto" },
    { "conn.user", "User", "Usuario" },
    { "conn.password", "Password", "Contraseña" },
    { "conn.password-hint", "Stored in Windows Credential Manager", "Se guarda en el Administrador de credenciales" },
    { "conn.database", "Database", "Base de datos" },
    { "conn.tls", "Use TLS and validate the server certificate", "Usar TLS y validar el certificado del servidor" },
    { "conn.trust", "Trust the server certificate without validating it", "Confiar en el certificado sin validarlo" },
    { "conn.trust-warning", "Not recommended: the identity of the server will not be verified.", "No recomendado: no se verificará la identidad del servidor." },
    { "conn.environment", "Environment", "Entorno" },
    { "conn.readonly", "Read-only connection (blocks every write)", "Conexión de solo lectura (bloquea toda escritura)" },
    { "conn.test", "Test connection", "Probar conexión" },
    { "conn.testing", "Testing…", "Probando…" },
    { "conn.save-connect", "Save and connect", "Guardar y conectar" },
    { "conn.test-ok", "Connection successful: {0}", "Conexión correcta: {0}" },
    { "conn.need-name", "Enter a name for the connection.", "Escribe un nombre para la conexión." },
    { "conn.need-path", "Choose a database file.", "Elige un archivo de base de datos." },
    { "conn.need-host", "Enter the server address.", "Escribe la dirección del servidor." },
    { "conn.bad-port", "The port must be a number between 1 and 65535.", "El puerto debe ser un número entre 1 y 65535." },
    { "conn.engine-unavailable", "This engine is not available in this build yet.", "Este motor aún no está disponible en esta versión." },
    { "conn.file-missing", "That file does not exist.", "Ese archivo no existe." },
    { "engine.soon", " (not available yet)", " (aún no disponible)" },
    { "engine.experimental", " (experimental)", " (experimental)" },
    { "note.crdb", "Tested against CockroachDB 25.2 (single node, insecure mode, 58 checks). Authentication and TLS are untested.", "Probado con CockroachDB 25.2 (nodo único, modo inseguro, 58 comprobaciones). Autenticación y TLS sin probar." },
    { "note.duckdb", "Tested with DuckDB 1.5 on a file (88 checks). MotherDuck and extensions are not supported.", "Probado con DuckDB 1.5 sobre un archivo (88 comprobaciones). MotherDuck y las extensiones no están soportadas." },
    { "note.pg", "Uses the PostgreSQL driver over the compatible protocol. Not tested against a real server of this engine.", "Usa el driver de PostgreSQL por el protocolo compatible. Sin probar contra un servidor real de este motor." },
    { "note.my", "Uses the MySQL/MariaDB driver over the compatible protocol. Not tested against a real server of this engine.", "Usa el driver de MySQL/MariaDB por el protocolo compatible. Sin probar contra un servidor real de este motor." },
    { "note.mongo", "Uses the MongoDB driver over the compatible protocol. Not tested against a real server of this engine.", "Usa el driver de MongoDB por el protocolo compatible. Sin probar contra un servidor real de este motor." },
    { "note.tds", "Uses the SQL Server driver over the TDS protocol. Not tested against a real server of this engine.", "Usa el driver de SQL Server por el protocolo TDS. Sin probar contra un servidor real de este motor." },
    { "note.mysql", "Uses the MariaDB client library. Tested against MariaDB only, not against a MySQL server.", "Usa la biblioteca cliente de MariaDB. Probado solo con MariaDB, no con un servidor MySQL." },
    { "note.mssql", "Needs ODBC Driver 18 for SQL Server. Not tested against a real SQL Server.", "Necesita ODBC Driver 18 para SQL Server. Sin probar contra un SQL Server real." },
    { "note.mongodb", "Tested against a standalone server. Atlas (mongodb+srv), authentication and TLS are untested.", "Probado con un servidor standalone. Atlas (mongodb+srv), autenticación y TLS sin probar." },
    { "note.documentdb", "Uses the MongoDB driver. DocumentDB needs TLS with the Amazon CA bundle and lacks some operators and transactions. Not tested.", "Usa el driver de MongoDB. DocumentDB requiere TLS con el paquete de CA de Amazon y carece de algunos operadores y transacciones. Sin probar." },
    { "engine.download", " (driver to download)", " (driver por descargar)" },
    { "driver.install", "Download and install driver", "Descargar e instalar driver" },
    { "driver.installing", "Installing…", "Instalando…" },
    { "driver.missing", "{0} needs its own driver, which is not installed. It is downloaded only for this engine, checked against a SHA-256 and stored in your user folder.", "{0} necesita su propio driver y no está instalado. Se descarga solo para este motor, se verifica con un SHA-256 y se guarda en tu carpeta de usuario." },
    { "driver.rejected", "The {0} driver was rejected: {1}", "El driver de {0} fue rechazado: {1}" },
    { "driver.unimplemented", "{0} is not implemented in this version.", "{0} aún no está implementado en esta versión." },
    { "driver.installed", "Driver installed.", "Driver instalado." },
    { "driver.failed", "Driver installation failed: {0}", "No se pudo instalar el driver: {0}" },
    { "conn.pwd-failed", "The password could not be saved in Windows Credential Manager.", "No se pudo guardar la contraseña en el Administrador de credenciales." },

    // export
    { "export.title", "Export data", "Exportar datos" },
    { "export.format", "Format", "Formato" },
    { "export.header", "Include column names", "Incluir nombres de columna" },
    { "export.start", "Export…", "Exportar…" },
    { "export.summary", "Exports ALL rows matching the current filters and sort of \"{0}\", not only the visible page.", "Exporta TODAS las filas que cumplen los filtros y el orden actuales de \"{0}\", no solo la página visible." },
    { "export.summary-sql", "Exports the whole result of the query.", "Exporta el resultado completo de la consulta." },
    { "export.progress", "{0} rows written…", "{0} filas escritas…" },
    { "export.done", "Exported {0} rows to {1}", "Exportadas {0} filas a {1}" },
    { "export.failed", "Export failed: {0}", "La exportación falló: {0}" },
    { "export.cancelled", "Export cancelled; the partial file was kept.", "Exportación cancelada; se conservó el archivo parcial." },

    // detail
    { "detail.title", "{0} — row {1}", "{0} — fila {1}" },

    // sql
    { "sql.run", "Run", "Ejecutar" },
    { "sql.running", "Running…", "Ejecutando…" },
    { "sql.open", "Open", "Abrir" },
    { "sql.save", "Save", "Guardar" },
    { "sql.done-rows", "{0} row(s) in {1} ms{2}", "{0} fila(s) en {1} ms{2}" },
    { "sql.truncated", " (showing the first {0})", " (se muestran las primeras {0})" },
    { "sql.affected", "{0} row(s) affected in {1} ms", "{0} fila(s) afectadas en {1} ms" },
    { "sql.empty", "Type a statement first.", "Escribe una sentencia primero." },
    { "sql.confirm-title", "Run statement", "Ejecutar sentencia" },
    { "sql.confirm-text", "This statement may modify data. Run it?", "Esta sentencia puede modificar datos. ¿Ejecutarla?" },
    { "sql.confirm-prod", "This is a PRODUCTION connection and the statement may modify data. Run it?", "Es una conexión de PRODUCCIÓN y la sentencia puede modificar datos. ¿Ejecutarla?" },
    { "sql.run-anyway", "Run", "Ejecutar" },
    { "sql.need-connection", "Connect to a database first.", "Conecta primero a una base de datos." },
    { "sql.copy-result", "Copy result", "Copiar resultado" },
    { "sql.copy-empty", "There is no query result to copy.", "No hay resultado de consulta para copiar." },

    // about
    { "about.title", "About Db Pro", "Acerca de Db Pro" },
    { "about.text", "Version {0}\n{1}\n\nNo telemetry. Passwords stay in Windows Credential Manager.\n\nMade with Slint (slint.dev)", "Versión {0}\n{1}\n\nSin telemetría. Las contraseñas permanecen en el Administrador de credenciales.\n\nHecho con Slint (slint.dev)" },
    { "about.description", "Native database studio. Connect. Query. Explore.", "Estudio nativo de bases de datos. Conecta. Consulta. Explora." },
    { "about.privacy", "No telemetry. Passwords stay in Windows Credential Manager.", "Sin telemetría. Las contraseñas permanecen en el Administrador de credenciales." },

    // settings and updates
    { "settings.title", "Settings", "Ajustes" },
    { "settings.appearance", "Appearance", "Apariencia" },
    { "settings.dark", "Dark", "Oscuro" },
    { "settings.light", "Light", "Claro" },
    { "settings.language", "Language", "Idioma" },
    { "settings.monitor-interval", "Performance refresh interval", "Intervalo de actualización del rendimiento" },
    { "settings.page-size", "Rows per page", "Filas por página" },
    { "settings.updates", "UPDATES", "ACTUALIZACIONES" },
    { "settings.version", "Version", "Versión" },
    { "settings.auto-update", "Check for updates when Db Pro starts (at most once a day)", "Buscar actualizaciones al iniciar Db Pro (como máximo una vez al día)" },
    { "settings.update-privacy", "Only a plain HTTPS request to the project's GitHub releases is made. Nothing about you or your databases is sent. Installers are verified with SHA-256 before running.", "Solo se hace una petición HTTPS simple a las versiones del proyecto en GitHub. No se envía nada sobre ti ni tus bases de datos. Los instaladores se verifican con SHA-256 antes de ejecutarse." },
    { "settings.check", "Check for updates", "Buscar actualizaciones" },
    { "settings.install", "Download and install", "Descargar e instalar" },
    { "settings.folders", "Folders", "Carpetas" },
    { "settings.data-dir", "Data folder", "Carpeta de datos" },
    { "settings.drivers-dir", "Drivers folder", "Carpeta de drivers" },
    { "settings.crash-logs", "Crash logs", "Registros de cierres" },
    { "settings.crash-logs-note", "Crash logs contain only the fault code, module and operation phase—not SQL, server names or credentials.", "Los registros solo contienen el código del fallo, el módulo y la fase de la operación; no incluyen SQL, servidores ni credenciales." },
    { "upd.checking", "Checking for updates…", "Buscando actualizaciones…" },
    { "upd.up-to-date", "You are up to date (version {0}).", "Estás al día (versión {0})." },
    { "upd.available", "Version {0} is available.", "Hay una versión nueva: {0}." },
    { "upd.downloading", "Downloading {0}… {1}", "Descargando {0}… {1}" },
    { "upd.verifying", "Verifying the download…", "Verificando la descarga…" },
    { "upd.starting", "Starting the installer. Db Pro will close and reopen by itself.", "Iniciando el instalador. Db Pro se cerrará y volverá a abrirse solo." },
    { "upd.failed", "Update failed: {0}", "La actualización falló: {0}" },
    { "upd.check-failed", "Could not check for updates: {0}", "No se pudo buscar actualizaciones: {0}" },
    { "upd.hash-mismatch", "The downloaded file does not match its SHA-256. It was deleted.", "El archivo descargado no coincide con su SHA-256. Se eliminó." },
    { "upd.installer-failed", "The installer could not be started.", "No se pudo iniciar el instalador." },

    // performance monitor
    { "view.monitor", "Performance", "Rendimiento" },
    { "mon.title", "Live performance", "Rendimiento en vivo" },
    { "mon.search", "Search sessions…", "Buscar sesiones…" },
    { "mon.blocked-only", "Blocked only", "Solo bloqueadas" },
    { "mon.kill", "End session", "Terminar sesión" },
    { "mon.unsupported", "Monitoring is not available for this engine", "El monitoreo no está disponible para este motor" },
    { "mon.unsupported-text", "Db Pro only shows live activity for engines that expose it and that it has implemented.", "Db Pro solo muestra la actividad en vivo de los motores que la exponen y que ya tiene implementados." },
    { "mon.no-sessions", "No other sessions right now.", "No hay otras sesiones ahora mismo." },
    { "mon.no-matches", "No sessions match these filters.", "Ninguna sesión coincide con esos filtros." },
    { "mon.local-empty", "This local engine exposes metrics, not a server session list.", "Este motor local ofrece métricas, no una lista de sesiones del servidor." },
    { "mon.local-summary", "Local engine activity", "Actividad del motor local" },
    { "mon.updated", "Updated {0}", "Actualizado {0}" },
    { "mon.paused", "Paused", "En pausa" },
    { "mon.summary", "{0} sessions · {1} running · {2} blocked", "{0} sesiones · {1} en ejecución · {2} bloqueadas" },
    { "mon.kill-title", "End session {0}?", "¿Terminar la sesión {0}?" },
    { "mon.kill-text", "The server will cancel whatever this session is doing and disconnect it. Uncommitted work in that session is rolled back.", "El servidor cancelará lo que esta sesión esté haciendo y la desconectará. El trabajo sin confirmar de esa sesión se revierte." },
    { "mon.kill-done", "Session {0} ended.", "Sesión {0} terminada." },
    { "mon.kill-failed", "Could not end the session: {0}", "No se pudo terminar la sesión: {0}" },
    { "mon.failed", "Could not read activity: {0}", "No se pudo leer la actividad: {0}" },
    { "mon.permission", "The server refused access. Check required privileges ({0}). Details: {1}", "El servidor rechazó el acceso. Comprueba los permisos necesarios ({0}). Detalles: {1}" },
    { "mon.permission-generic", "the engine's monitoring privileges", "los permisos de monitorización del motor" },
    { "mon.permission-mssql", "VIEW SERVER STATE or VIEW SERVER PERFORMANCE STATE, depending on SQL Server version", "VIEW SERVER STATE o VIEW SERVER PERFORMANCE STATE, según la versión de SQL Server" },
    { "mon.permission-postgres", "pg_monitor or an equivalent monitoring role", "pg_monitor o un rol equivalente de monitorización" },
    { "mon.permission-mysql", "PROCESS privilege for server-wide process visibility", "privilegio PROCESS para ver los procesos del servidor" },
    { "mon.permission-mongodb", "clusterMonitor or equivalent privileges", "clusterMonitor o permisos equivalentes" },
    { "mon.m.sessions", "Sessions", "Sesiones" },
    { "mon.m.active", "Running", "En ejecución" },
    { "mon.m.blocked", "Blocked", "Bloqueadas" },
    { "mon.m.max_sessions", "Max sessions", "Sesiones máximas" },
    { "mon.m.cache_used", "Page cache", "Caché de páginas" },
    { "mon.m.schema_used", "Schema memory", "Memoria del esquema" },
    { "mon.m.statement_memory", "Statement memory", "Memoria de instrucciones" },
    { "mon.m.cache_hits", "Cache hits", "Aciertos de caché" },
    { "mon.m.cache_misses", "Cache misses", "Fallos de caché" },
    { "mon.m.cache_writes", "Cache writes", "Escrituras en caché" },
    { "mon.m.memory_used", "Engine memory", "Memoria del motor" },
    { "mon.m.temporary_storage", "Temporary storage", "Almacenamiento temporal" },
    { "mon.m.tps", "Transactions/s", "Transacciones/s" },
    { "mon.m.rows_read", "Rows read/s", "Filas leídas/s" },
    { "mon.m.rows_written", "Rows written/s", "Filas escritas/s" },
    { "mon.m.disk_reads", "Disk block reads/s", "Lecturas de bloque a disco/s" },
    { "mon.m.deadlocks", "Deadlocks (total)", "Interbloqueos (total)" },
    { "mon.m.temp_bytes", "Temp files written/s", "Archivos temporales escritos/s" },
    { "mon.m.ops_insert", "Inserts/s", "Inserciones/s" },
    { "mon.m.ops_query", "Queries/s", "Consultas/s" },
    { "mon.m.ops_update", "Updates/s", "Actualizaciones/s" },
    { "mon.m.ops_delete", "Deletes/s", "Eliminaciones/s" },
    { "mon.m.ops_command", "Commands/s", "Comandos/s" },
    { "mon.m.net_in", "Network in/s", "Red entrante/s" },
    { "mon.m.net_out", "Network out/s", "Red saliente/s" },
    { "mon.m.queued", "Operations queued for locks", "Operaciones en cola por bloqueos" },
    { "mon.m.batches", "Batch requests/s", "Lotes/s" },
    { "mon.m.compilations", "SQL compilations/s", "Compilaciones SQL/s" },
    { "mon.m.ple", "Page life expectancy (s)", "Esperanza de vida de página (s)" },
    { "mon.m.queries", "Statements/s", "Sentencias/s" },
    { "mon.m.slow", "Slow queries (total)", "Consultas lentas (total)" },
    { "mon.m.row_lock_waits", "InnoDB row lock waits now", "Esperas de bloqueo de fila InnoDB ahora" },
    { "mon.col.id", "ID", "ID" },
    { "mon.col.user", "User", "Usuario" },
    { "mon.col.database", "Database", "Base de datos" },
    { "mon.col.app", "Application", "Aplicación" },
    { "mon.col.client", "Client", "Cliente" },
    { "mon.col.host", "Host", "Host" },
    { "mon.col.state", "State", "Estado" },
    { "mon.col.command", "Command", "Comando" },
    { "mon.col.wait", "Waiting on", "Esperando" },
    { "mon.col.seconds", "Seconds", "Segundos" },
    { "mon.col.blocked_by", "Blocked by", "Bloqueada por" },
    { "mon.col.query", "Statement", "Sentencia" },
    { "tabs.limit-title", "Too many tabs", "Demasiadas pestañas" },
    { "tabs.limit-text", "You can keep up to {0} tables open. Close one first.", "Puedes mantener hasta {0} tablas abiertas. Cierra una primero." },
};

std::string g_lang = "en";

const std::unordered_map<std::string_view, const Entry *> &index()
{
    static const auto m = [] {
        std::unordered_map<std::string_view, const Entry *> r;
        for (auto &e : kTable) r.emplace(e.key, &e);
        return r;
    }();
    return m;
}

} // namespace

void set_language(const std::string &lang) { g_lang = lang == "es" ? "es" : "en"; }
const std::string &language() { return g_lang; }

std::string tr(const std::string &key)
{
    auto &m = index();
    auto it = m.find(key);
    if (it == m.end()) return key;
    const char *s = g_lang == "es" ? it->second->es : it->second->en;
    if (!s || !*s) s = it->second->en;
    return s;
}

std::string trf(const std::string &key, std::initializer_list<std::string> args)
{
    std::string s = tr(key);
    size_t i = 0;
    for (auto &a : args) {
        std::string tag = "{" + std::to_string(i++) + "}";
        for (size_t p = s.find(tag); p != std::string::npos; p = s.find(tag, p + a.size()))
            s.replace(p, tag.size(), a);
    }
    return s;
}

std::string with_commas(unsigned long long n)
{
    std::string d = std::to_string(n), out;
    char sep = g_lang == "es" ? '.' : ',';
    int c = 0;
    for (size_t k = d.size(); k-- > 0;) {
        out.insert(out.begin(), d[k]);
        if (++c % 3 == 0 && k > 0) out.insert(out.begin(), sep);
    }
    return out;
}

} // namespace dbp::i18n
