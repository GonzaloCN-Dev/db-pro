# AGENTS.md — Db Pro

Read this first. Do not change decisions listed here without recording the reason in `docs/DECISIONS.md`.

## Product
**Db Pro** — native universal database studio. Tagline: *Connect. Query. Explore.*
Author: Gonzalo Cruz Núñez. © 2026. Free distribution. Target: Windows 10/11 x64 first.
Product name, author and version live in ONE place: `src/core/brand.h` (to be created) and `CMakeLists.txt` `project(... VERSION)`.
The name is **Db Pro** (not "DB Lite").

## Stack (decided: C++20 + Slint, same as sibling project Rclone Lite)
- Language: C++20, MSVC (VS 2022 Build Tools / VS Community), CMake + Ninja.
- UI: Slint 1.19.0 via CMake FetchContent, `winit` backend, software renderer by default (Rclone Lite found the GL renderer blank on some drivers), FemtoVG available.
- Dependencies: vcpkg manifest (`vcpkg.json`, pinned baseline), triplet `x64-windows-static-md`, vcpkg cloned in `third_party/vcpkg`.
- Drivers: sqlite3, libpq (PostgreSQL), libmariadb (MySQL + MariaDB), mongo-cxx-driver (MongoDB), SQL Server via ODBC Driver 18 (installed on this machine) — Tiberius is Rust-only and is NOT used.
- Forbidden: Electron, WebView UI, Java, Node/Python runtime, local HTTP servers, web frameworks.
- Rclone Lite (`G:\Proyectos APP\Rclone Lite`) is a visual/tech reference. NEVER modify it. No code dependency between the two.

## Architecture
```
src/core/        Value, metadata, typed filters, Connector interface, SQL generator (no DB access)
src/connectors/  one file per engine implementing dbp::Connector
tests/           self-contained test runner (tests/test.h), run via ctest / dbpro_tests.exe
ui/              Slint (theme, widgets, data grid, dialogs)  -- to be built
```
Rules:
- All engines go through `dbp::Connector`. Engines declare real `Capabilities`. Never fake support.
- Filters are typed (`FilterGroup`/`Condition`/`Op`). SQL is produced ONLY by `sqlgen.cpp` with bound parameters and identifiers validated against table metadata. Concatenating user text into SQL is forbidden.
- Row identity = primary key (or `_id`). Never a visual row index. No key → editing blocked.
- Updates carry the original values in WHERE (optimistic concurrency); 0 rows affected ⇒ `ErrKind::Conflict`. Never overwrite silently.
- Change sets are applied atomically.
- Paging is deterministic (PK appended to ORDER BY), fetches `limit+1` to know `has_more`; no automatic `COUNT(*)`. Totals are exact-on-request, estimated (shown as ≈) or unknown.
- Export streams through `stream_rows` in bounded batches; never loads the full result in memory.
- The Slint UI thread must never block: queries, network, disk, import/export run on worker threads and post results via `slint::invoke_from_event_loop`. Stale responses are discarded with a request generation counter.
- Passwords: Windows Credential Manager (`CredWrite`/`CredRead`), never in SQLite/JSON/logs. Exported profiles omit passwords.
- TLS validation is ON by default; `TrustServerCertificate` must be an explicit opt-in.
- No telemetry, no ads, no AI/analytics endpoints. Updater (later) is opt-in and verifies integrity.

## Build / test
```powershell
# one-time: dependencies
$env:VCPKG_ROOT="$PWD\third_party\vcpkg"
third_party\vcpkg\vcpkg.exe install --triplet x64-windows-static-md --x-install-root=vcpkg_installed
# build + tests (inside a VS developer environment, see build.bat)
.\build.bat
build\dbpro_tests.exe
```

## Status
See `docs/PROGRESS.md` (truth about what works), `docs/TODO.md`, `docs/KNOWN_ISSUES.md`.
Engine status is conservative: nothing is "Stable" until its integration tests pass against a real server.

## Acceptance (abridged)
Open a 70k+ row table showing 100 rows; filter any column server-side across the whole table; combine filters; sort remotely; edit a cell; save; verify on server; export ALL filtered rows; same flow on MongoDB; UI never freezes; memory bounded.

## Environment notes (this machine)
- Rust is installed but NOT used. No Docker. A MariaDB/MySQL (XAMPP) service listens on 3306; no SQL Server, PostgreSQL or MongoDB servers are installed, so those connectors cannot be integration-tested here until a server is available.
- Do not run destructive tests against any real database. Tests use temp SQLite files.
