# Decisions

Each entry is binding until superseded by a newer entry that states why.

## D-001 — Product name is "Db Pro"
The brief's text says "DB Lite" in places; the owner explicitly asked for **Db Pro**. All identity strings come from `src/core/brand.h`.

## D-002 — C++20 + Slint instead of Rust + Slint
The initial brief asked for Rust; the owner then explicitly asked to use C++ like Rclone Lite. A first Rust workspace scaffold was started and discarded before any commit (no code was kept). Consequences:
- Rust-only crates named in the brief (SQLx, Tiberius, redis-rs, keyring, rustls…) are replaced by C/C++ client libraries.
- SQL Server: ODBC Driver 18 (present on the dev machine, 64-bit).
- PostgreSQL: libpq. MySQL/MariaDB: MariaDB Connector/C (libmariadb). MongoDB: mongo-cxx-driver. SQLite: sqlite3 amalgamation via vcpkg.
- Credentials: Windows Credential Manager API instead of the `keyring` crate.
- Mirrors the Rclone Lite toolchain: CMake + Ninja + MSVC, Slint via FetchContent (v1.19.0), `winit-software` backend by default.

## D-003 — Dependency management with vcpkg manifest
`vcpkg.json` with pinned `builtin-baseline`; vcpkg cloned in `third_party/vcpkg` (not committed). Triplet `x64-windows-static-md` (static libs, dynamic CRT) keeps the distribution to one exe plus Slint's DLL and the VC runtime, as in Rclone Lite.
`libmariadb` on Windows uses Schannel natively, so no OpenSSL feature is requested for it.

## D-004 — Filtering is typed, SQL is generated in one place
`core/sqlgen.cpp` is the only producer of SQL for grid operations. User text is bound as parameters; identifiers must match table metadata and are delimited per dialect; LIKE patterns are escaped (`ESCAPE '!'`).

## D-005 — Optimistic concurrency by original-value WHERE
Updates include the originally loaded values in the WHERE clause. Zero rows affected ⇒ Conflict, whole batch rolled back. Works on every SQL engine without schema changes. Large/JSON/binary columns are excluded from the check.

## D-006 - One optional DLL per engine family; the base app ships only SQLite
Reason: users should not download client libraries for engines they never use.
- `DbPro.exe` links SQLite only. Each other engine family is `dbpro_driver_<id>.dll` (today: `mysql`, covering MySQL and MariaDB) and statically contains its client library (libmariadb, later libpq, ODBC glue, mongo-cxx).
- Contract in `src/drivers/driver_api.h` (three C exports). The `Connector` object crosses the DLL boundary as a C++ object, so app and drivers must be built together; the loader rejects a driver whose ABI number or app version differs.
- Lookup order: `<exe>\drivers\` (shipped, trusted like the exe) then `%APPDATA%\DbPro\drivers\` (downloaded). A DLL in the user folder loads only if its SHA-256 equals the hash recorded when Db Pro installed it, so a file dropped there by someone else is refused.
- Download happens only when the user presses "Download and install driver". HTTPS only, certificate validation on, size cap, SHA-256 from `drivers.json` of release `v<app version>` checked before the file is written. No background or automatic downloads.
- Trade-off accepted: dropping the vcpkg libraries from the main exe makes the driver DLLs larger than a shared-library design, but each is independent and removable.

## D-007 - Engine catalog and phased scope (owner request: "connect to everything")
The owner asked for ~230 engines (SQL, NoSQL, key-value, graph, embedded, analytic, big data, time series, search).
- `src/core/engines.cpp` is the single catalog of engines the UI offers. A row exists only if a real route to the engine exists in that build (own driver, or a driver speaking a compatible protocol). Nothing is listed "to look complete".
- Status is `Tested` only after integration tests pass against a real server of that exact engine. Everything else is `Experimental` and shows a note in the connection dialog. `tests/test_engines.cpp` pins the Tested list.
- Routes: A) compatible wire protocol on an existing driver (PostgreSQL, MySQL, TDS, MongoDB); B) new driver on an open protocol (DuckDB, Redis/Valkey, ClickHouse, Elasticsearch/OpenSearch, Firebird, ...); C) generic ODBC driver plus a dialect per engine (Oracle, Db2, HANA, Teradata, ...); D) Java-only engines.
- **Java-only engines are excluded** (H2, HSQLDB, Derby, H2GIS embedded, GemFire XD, SnappyData). The no-JVM rule stands. H2 Server is reachable through its PostgreSQL mode.
- Non-tabular families (key-value, graph, search, time series) are shown in the data grid, read-only first. Editing is added only where it is safe and tested.
- Phases: 0.2.0 = route A presets + first route B drivers that can be tested locally. 0.3.0 = generic ODBC driver + dialects. Later = graph, time series, cloud-only services. Each phase is published with honest per-engine status.
- ODBC vendor drivers are the user's to install (often licensed, not redistributable).