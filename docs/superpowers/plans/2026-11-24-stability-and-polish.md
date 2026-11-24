# Db Pro Stability and Polish Implementation Plan

> **For agentic workers:** execute natively in this session; use a failing regression test before each code fix.

**Goal:** Diagnose and prevent unexpected exits, validate Db Pro's database workflows, finish the prioritized usability work, and ship consistent product attribution.

**Architecture:** Start with the SQL Server table-open failure using isolated data and diagnostic evidence. Add regression coverage at the narrowest reproducible boundary, then improve the existing Controller/Slint and connector abstractions without overstating untested engine support. Publish only after build, tests, packaging, and checksum verification.

**Tech Stack:** C++20, MSVC, CMake/Ninja, Slint 1.19, Windows Event Log/WER, SQLite test fixtures, Inno Setup, GitHub CLI.

**Spec:** `AGENTS.md`, `docs/DECISIONS.md`, `docs/PROGRESS.md`, and the user's request (2026-11-24).

## Global Constraints
- Keep the UI native: C++20 + Slint; no Electron, WebView, Java, Node/Python runtime, or local web server.
- Never use the developer's XAMPP MariaDB on port 3306; reproduce with isolated temporary databases.
- Do not claim a connector is tested without integration tests against that exact real engine.
- Updates stay opt-in, HTTPS-only, checksum-verified; no telemetry or secrets in files/logs.
- Preserve row identity, bound-parameter SQL, optimistic concurrency, and non-blocking UI.
- Product identity and copyright: Db Pro, Gonzalo Cruz Núñez, and `https://github.com/GonzaloCN-Dev`.

## Review Focus
- SQL Server metadata or row values containing unusual SQL types, nulls, long/binary data, or permissions: report a connector error, not terminate.
- Late worker responses after tab switching/disconnect/window close: discard safely; do not access destroyed Controller state.
- Rapid tab opening/closing while metadata/page requests are in flight: preserve state and never install stale rows into another table.
- Unsupported or permission-denied monitor APIs: keep the data view usable and show a translated error/state.
- Missing/corrupt settings, unwritable folders, and failed update downloads: preserve startup and user data; surface an actionable message.

---

### Task 1: Diagnose and regress SQL Server table-open exits

**Files:**
- Inspect/modify: `src/connectors/mssql_conn.cpp`, `src/app.cpp`
- Test: `tests/test_mssql.cpp` or existing test harness; isolated SQL Server fixture only if one is available.
- Diagnostics: `src/platform.cpp`, `src/platform.h`, `src/app.cpp`

- [ ] Collect the user's SQL Server version, authentication/TLS mode, selected table/schema, and crash event or log from the other machine.
- [ ] Reproduce only with a throwaway/local SQL Server or a sanitized fixture; otherwise mark the specific issue unresolved and keep capture instrumentation.
- [ ] Add a test that fails on the identified bad SQL type/metadata/data/exception path before changing production code.
- [ ] Implement the smallest connector/controller fix and verify no exception escapes the worker/UI boundary.
- [ ] Verify with build, unit suite, and exact integration reproduction; document if server reproduction remains unavailable.

### Task 2: Application crash diagnostics and worker lifetime

**Files:**
- Modify: `src/app.cpp`, `src/platform.cpp`, `src/platform.h`, `src/store.cpp` (only if a diagnostics setting is needed)
- Test: `tests/test_app_lifecycle.cpp` or a narrow worker/lifecycle test.

- [ ] Write a regression test for stale completion after cancellation/disconnect.
- [ ] Replace unsafe detached-worker lifetime assumptions with owned/joined or safely shared task state; ensure shutdown cannot destroy state still used by a worker.
- [ ] Add opt-in, privacy-safe crash diagnostics (exception code/module/time only; never SQL, host, username, or passwords) with a user-visible log-folder action.
- [ ] Test worker completion, cancellation, shutdown, and log-write failure paths.

### Task 3: Tabs and settings hardening

**Files:**
- Modify: `src/app.cpp`, `src/store.{h,cpp}`, `ui/backend.slint`, `ui/app.slint`, `ui/dialogs.slint`, `ui/types.slint`, `src/i18n.cpp`
- Test: `tests/test_tabs.cpp` or UI self-test scripts.

- [ ] Add regression cases for dirty active/inactive tab close, switch during load, duplicate table, disconnect, and maximum tab count.
- [ ] Make close/close-others/disconnect guards consistently offer safe save/discard/cancel behavior for every dirty tab.
- [ ] Add useful settings as justified: diagnostics opt-in, page size, update preferences, monitor refresh interval, and data/log/driver folder access.
- [ ] Extend UI self-test to assert visible states and produce screenshots for settings, tabs, and error states.
- [ ] Verify ES/EN strings, restart persistence, and dark/light rendering.

### Task 4: Monitor and engine-specific verification

**Files:**
- Modify: `src/connectors/{postgres,mysql,mongodb,mssql,sqlite,duckdb}_conn.cpp`, `src/app.cpp`, `ui/app.slint`, `src/i18n.cpp`
- Test: per-engine integration tests and monitor model tests.

- [ ] Add tests for metric counter deltas, reset/decrease, missing baseline, byte formatting, blockers, and query selection.
- [ ] Run monitor integration tests only against throwaway local servers on non-production ports.
- [ ] Add local-engine diagnostics only where the engine exposes meaningful activity; otherwise retain explicit unsupported states.
- [ ] Test permission-denied monitor and kill-session responses, and read-only profiles.
- [ ] Update compatibility docs conservatively with exact tested server versions.

### Task 5: UX, performance, installer, and attribution

**Files:**
- Modify: `README.md`, `RELEASE_NOTES.md`, `CHANGELOG.md`, `docs/PROGRESS.md`, `docs/DATABASE_COMPATIBILITY.md`, `installer/DbPro.iss`, brand/about UI and i18n.
- Test: UI self-test, bounded-memory/export test, silent install/update test, copyright/resource checks.

- [ ] Show “© 2026 Gonzalo Cruz Núñez” and a clickable repository URL in About and installer metadata.
- [ ] Add monitor session search/filter/sort and clear refresh/permission affordances if tests support the UI behavior.
- [ ] Measure startup and memory on a fresh 70k+ row fixture; retain bounded paging and streaming export.
- [ ] Verify install, update-over-existing-install, and relaunch on a disposable directory; do not run against the user's live install.
- [ ] Keep release notes, compatibility matrix, version, and installer assets aligned; publish after final verification only.

### Task 6: Final verification and deployment

**Files:** release/version/docs/assets only after Tasks 1–5 pass.

- [ ] Build all app, drivers, tests, and integration harnesses.
- [ ] Run CTest and UI self-tests; record unverified server-dependent cases.
- [ ] Scan changed files for secrets and run `git diff --check`.
- [ ] Generate installer and driver manifests, independently verify SHA-256, then publish source and release assets to GitHub.
