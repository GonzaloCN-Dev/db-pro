# Usability Improvements Implementation Plan

> **For agentic workers:** Execute natively in this session; add regression tests before production changes and verify each task.

**Goal:** Improve monitor usability, accurate local-activity behavior, visual-test coverage, installation/update confidence, and signing readiness.

**Architecture:** Keep the monitor capability-driven and perform filtering/sorting on the sampled activity rows in the UI model, never by constructing SQL. Keep local engines explicitly unsupported unless meaningful, real metrics can be implemented and tested. Extend the existing scripted Slint self-test and Inno Setup release flow; signing remains optional and must use a user-provided certificate.

**Tech Stack:** C++20, Slint 1.19, MSVC, CMake/Ninja, Inno Setup 6, existing self-test harness, Windows Authenticode tooling.

**Spec:** User request (2026-11-24), `AGENTS.md`, `docs/DECISIONS.md`, `docs/PROGRESS.md`.

## Global Constraints
- No Electron, WebView, Java, Node/Python runtime, local HTTP server, or web framework.
- Never use XAMPP MariaDB on 3306; integration tests must use isolated data and servers.
- Never claim engine support without meaningful implementation; keep explicit unsupported states.
- Never log SQL, row values, connection details or credentials.
- Do not sign with a self-signed or unprovided certificate; signing is conditional on a valid publisher certificate.
- Installer/update tests use a temporary directory, not the user's installed copy.

## Review Focus
- Empty, partial, unicode and case-varied monitor search input; no matching sessions must remain a clear state.
- Sorting with nulls, numeric strings and equal values must be deterministic and preserve selection identity.
- Permission errors from monitoring and killing must preserve the view and explain the likely permission required.
- UI self-test failure or timeout must not leave Db Pro running or touch user data.
- Silent installation/upgrade failure must not uninstall or overwrite the current installation.

---

### Task 1: Monitor search, filters, sorting and permission guidance

**Files:** `src/app.cpp`, `ui/backend.slint`, `ui/app.slint`, `ui/widgets.slint`, `src/i18n.cpp`, tests for activity-model transforms.

- [ ] Add failing tests for case-insensitive search, blocked-only filtering, stable sort, missing/null values, and selection retained by session ID.
- [ ] Implement a pure activity-row transform with filter/search/sort state; keep connector activity data and identity separate from display indices.
- [ ] Add compact search, blocked-only filter, sortable column headers and explicit refresh/pause affordances to `MonitorView`.
- [ ] Classify permission-denied errors without hiding raw safe connector diagnostics; show translated guidance that viewing may require `VIEW SERVER STATE` (SQL Server), `pg_monitor`/equivalent (PostgreSQL), process-list privileges (MySQL), or suitable MongoDB privileges.
- [ ] Verify build, tests, ES/EN rendering and unsupported/read-only behavior.

### Task 2: Local activity support decision

**Files:** `src/connectors/sqlite_conn.cpp`, `src/connectors/duckdb_conn.cpp`, `src/core/core.h`, compatibility/progress docs; local-activity tests.

- [ ] Inspect public engine APIs for meaningful per-connection activity/lock metrics without inventing server metrics.
- [ ] If usable, add read-only local activity with a bounded query history and tests; otherwise add explicit tests that capabilities remain false and UI says unsupported.
- [ ] Keep `kill_session` false for local engines unless the engine exposes a safe, tested operation.
- [ ] Document the actual capabilities and verification evidence.

### Task 3: Expand scripted visual self-tests

**Files:** `src/selftest.{h,cpp}`, UI self-test scripts under `tests/ui/`, test invocation/docs.

- [ ] Add scenarios for settings persistence, two tabs, dirty-tab save/discard/cancel, connection success/failure, monitor supported/unsupported/permission error and window shutdown.
- [ ] Ensure scripts use isolated `%TEMP%` profiles/databases and never read or modify real user profiles.
- [ ] Make the runner capture screenshots and report command/assertion failures with nonzero exit status.
- [ ] Run each scenario and verify no process remains after completion.

### Task 4: Installer/update confidence, disk-space notice and portable-build evaluation

**Files:** `installer/DbPro.iss`, `installer/build-installer.ps1`, `src/updater.cpp`, package scripts, `docs/PROGRESS.md`, release notes.

- [ ] Add a failing packaging test for the required-space value based on the actual packaged payload; show it in installer UI.
- [ ] Test fresh silent install and upgrade over a disposable older version, verify files/version/launch/uninstall, and ensure cancellation/failure preserves the previous install.
- [ ] Test the updater only against a controlled test release or local test seam; never replace the current machine's installation.
- [ ] Evaluate portable packaging in a separate directory, confirming settings/secrets behavior and driver discovery; do not ship portable if it violates Credential Manager or data-location guarantees.
- [ ] Update docs with exact test setup/results and remaining limitations.

### Task 5: Authenticode signing readiness

**Files:** `installer/build-installer.ps1`, optional `scripts/sign-release.ps1`, release docs.

- [ ] Detect signing configuration via an explicitly supplied certificate thumbprint/store or secure signing service; never commit a certificate or password.
- [ ] Add a dry-run/test that refuses missing, expired, untrusted or mismatched certificates without failing unsigned developer builds.
- [ ] If a valid authorized certificate is available, sign the final installer and verify `Get-AuthenticodeSignature`; otherwise publish unsigned and state that no certificate was available.

### Task 6: Release and verification

- [ ] Build `DbPro`, all drivers, unit tests and integration harnesses.
- [ ] Run CTest, visual self-tests, installer tests and secret/diff checks.
- [ ] Update compatibility/progress/release docs conservatively.
- [ ] Publish the source and release artifacts to GitHub after checks; verify the release assets and checksums remotely.
