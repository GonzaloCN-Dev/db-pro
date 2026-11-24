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
