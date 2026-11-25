// Application controller: owns the Slint window, the active connection and every worker thread.
// The UI thread never blocks: connector calls run on worker threads and post their results back with
// slint::invoke_from_event_loop. Each data request carries a generation number; late answers are dropped.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <thread>

#include "app.h"
#include "connectors/factory.h"
#include "core/brand.h"
#include "core/engines.h"
#include "core/changes.h"
#include "core/exporter.h"
#include "core/filtertext.h"
#include "core/sqlsafety.h"
#include "core/activity_view.h"
#include "diagnostics.h"
#include "drivers/driver_manager.h"
#include "i18n.h"
#include "platform.h"
#include "selftest.h"
#include "column_layouts.h"
#include "core/copyformat.h"
#include "core/paste.h"
#include "core/engines.h"
#include "query_history.h"
#include "store.h"
#include "updater.h"

using namespace dbp;
using i18n::tr;
using i18n::trf;
using Clock = std::chrono::steady_clock;

namespace {

slint::SharedString S(const std::string &s) { return slint::SharedString(s); }
std::string U(const slint::SharedString &s) { return std::string(std::string_view(s)); }

std::string lower(std::string s)
{
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Keeps cells to one short line; the full value stays available in the detail viewer.
std::string cell_text(const std::string &s, size_t max = 160)
{
    std::string out;
    out.reserve(std::min(s.size(), max + 4));
    size_t n = std::min(s.size(), max);
    while (n < s.size() && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    for (size_t k = 0; k < n; ++k) {
        char c = s[k];
        if (c == '\r') continue;
        out += (c == '\n' || c == '\t') ? ' ' : c;
    }
    if (n < s.size()) out += "Ã¢â‚¬Â¦";
    return out;
}

// Slint reports modifier and function keys (Shift, F5, arrows...) as one-character strings in the
// C0 control range or the private-use area. Those must never start a cell edit.
bool starts_with_printable(const std::string &s)
{
    if (s.empty()) return false;
    unsigned char b = (unsigned char)s[0];
    if (b < 0x20 || b == 0x7f) return false;
    if (b == 0xEF && s.size() >= 3) {
        unsigned cp = ((b & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F);
        if (cp >= 0xE000 && cp <= 0xF8FF) return false;
    }
    if (b == 0xC2 && s.size() >= 2 && (unsigned char)s[1] < 0xA0) return false; // C1 controls
    return true;
}

const std::vector<EngineDef> &kEngines = engine_catalog();
const int kEngineCount = (int)engine_catalog().size();
const int kPageSizes[] = { 50, 100, 250, 500, 1000 };

struct Node {
    int kind = 0; // 0 connection, 1 database, 2 schema, 3 table, 4 view, 5 collection
    std::string label, db, schema, name;
    bool expanded = false, loaded = false, busy = false;
    std::vector<Node> kids;
    std::string key() const { return std::to_string(kind) + "\x1f" + db + "\x1f" + schema + "\x1f" + name; }
};

std::vector<Node> list_children(Connector &c, int kind, const std::string &db, const std::string &schema)
{
    std::vector<Node> out;
    if (kind == 0) {
        auto dbs = c.list_databases();
        if (dbs.size() == 1) return list_children(c, 1, dbs[0].name, "");
        for (auto &d : dbs) {
            Node n;
            n.kind = 1;
            n.label = n.db = n.name = d.name;
            out.push_back(std::move(n));
        }
        return out;
    }
    if (kind == 1 && c.caps().schemas) {
        for (auto &s : c.list_schemas(db)) {
            Node n;
            n.kind = 2;
            n.label = n.schema = n.name = s.name;
            n.db = db;
            out.push_back(std::move(n));
        }
        return out;
    }
    for (auto &o : c.list_tables(db, schema)) {
        Node n;
        n.kind = o.kind == ObjectKind::View ? 4 : o.kind == ObjectKind::Collection ? 5 : 3;
        n.label = n.name = o.name;
        n.db = db;
        n.schema = schema;
        out.push_back(std::move(n));
    }
    return out;
}

struct ColFilter {
    std::string text;
    int op = 0;
    bool explicit_op = false;
    std::string err;
    bool active = false;
};

enum class TotalMode { Unknown, Estimated, Exact };

const int kMonitorIntervals[] = { 1, 2, 5, 10, 30 };
const int kQueryTimeouts[] = { 0, 30, 60, 300, 600 };
const size_t kMaxTabs = 16;
const size_t kSparkPoints = 60;

std::string human_bytes(double v)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string plain_number(double v, bool fractional)
{
    char buf[48];
    if (fractional && v < 10.0 && v != std::floor(v)) std::snprintf(buf, sizeof buf, "%.1f", v);
    else return i18n::with_commas((unsigned long long)std::max(0.0, std::round(v)));
    return buf;
}

std::string clock_text()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv {};
    localtime_s(&tmv, &t);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    return buf;
}

// SVG path for a sparkline in a 100 x 24 box.
std::string spark_path(const std::deque<double> &h)
{
    if (h.size() < 2) return "";
    double lo = *std::min_element(h.begin(), h.end()), hi = *std::max_element(h.begin(), h.end());
    std::string out;
    char buf[48];
    for (size_t i = 0; i < h.size(); ++i) {
        double x = 100.0 * (double)i / (double)(h.size() - 1);
        double y = hi > lo ? 22.0 - (h[i] - lo) / (hi - lo) * 20.0 : 12.0;
        std::snprintf(buf, sizeof buf, "%c %.1f %.1f ", i == 0 ? 'M' : 'L', x, y);
        out += buf;
    }
    return out;
}

} // namespace

struct TabState {
    std::string title;
    TableRef ref; // the node that opened it; used to focus an already open table
    TableMeta meta;
    PageRequest req;
    std::vector<std::vector<Value>> rows;
    ChangeSet changes;
    std::vector<ColFilter> filters;
    std::vector<double> widths;
    FilterGroup pending_group;
    std::string pending_sig, applied_sig, query_text;
    int applied_filter_count = 0, sel_row = -1, sel_col = -1;
    TotalMode total_mode = TotalMode::Unknown;
    uint64_t total = 0;
    bool has_more = false, editable = false, page_loaded = false;
};

class Controller {
public:
    Controller() : ui_(AppWindow::create()), store_()
    {
        store_.load();
        history_.load();
        layouts_.load();
        i18n::set_language(store_.settings.lang);
        ui_->global<Theme>().set_dark(store_.settings.dark);
        page_size_ = std::clamp(store_.settings.page_size, 10, 5000);
        wire();
        refresh_engines();
        rebuild_tree();
        B().set_page_size_index(page_size_index());
        B().set_status(S(tr("ready")));
        B().set_columns(columns_model_);
        B().set_rows(rows_model_);
        B().set_sql_columns(sql_columns_model_);
        B().set_sql_rows(sql_rows_model_);
        B().set_tree(tree_model_);
        B().set_tabs(tabs_model_);
        B().set_hist_rows(hist_model_);
        B().set_colcfg_rows(colcfg_model_);
        B().set_sql_tabs(sql_tabs_model_);
        init_sql_tabs();
        B().set_mon_metrics(mon_metrics_model_);
        B().set_mon_columns(mon_columns_model_);
        B().set_mon_rows(mon_rows_model_);
        B().set_app_version(S(brand::kVersion));
        B().set_app_copyright(S(brand::kCopyright));
        B().set_theme_index(store_.settings.dark ? 0 : 1);
        B().set_lang_index(store_.settings.lang == "es" ? 0 : 1);
        B().set_auto_update(store_.settings.auto_update);
        B().set_monitor_interval_index(monitor_interval_index());
        B().set_op_menu(op_menu_model_);
        B().set_cd_engines(engines_model_);
        B().set_status_right(S(std::string(brand::kName) + " " + brand::kVersion));
    }

    ~Controller() { alive_->store(false, std::memory_order_release); }

    int run()
    {
        std::unique_ptr<SelfTest> selftest;
        if (const char *script = std::getenv("DBPRO_SELFTEST")) {
            const char *out = std::getenv("DBPRO_SELFTEST_OUT");
            selftest = std::make_unique<SelfTest>(ui_->window(), script, out ? out : ".", [this](const std::string &c) { return selftest_probe(c); });
            selftest->start();
        }
        startup_update_check();
        ui_->run();
        shutdown();
        return 0;
    }

private:
    bool selftest_probe(const std::string &condition) const
    {
        auto number_after = [&](const char *prefix, int &value) {
            const size_t n = std::char_traits<char>::length(prefix);
            if (condition.rfind(prefix, 0) != 0) return false;
            try { value = std::stoi(condition.substr(n)); return true; }
            catch (...) { return false; }
        };
        int expected = 0;
        if (condition == "connected") return connected_;
        if (condition == "not-connected") return !connected_;
        if (condition == "connecting") return connecting_;
        if (condition == "connection-error") return last_connection_failed_;
        if (condition == "table-open") return table_open_;
        if (condition == "settings-open") return B().get_settings_open();
        if (condition == "about-open") return B().get_about_open();
        if (condition == "copyright-visible") return U(B().get_app_copyright()) == brand::kCopyright;
        if (condition == "dirty") {
            if (!changes_.empty()) return true;
            for (const auto &t : tabs_) if (!t.changes.empty()) return true;
            return false;
        }
        if (condition == "clean") {
            if (!changes_.empty()) return false;
            for (const auto &t : tabs_) if (!t.changes.empty()) return false;
            return true;
        }
        if (condition == "dialog-open") return B().get_dlg_confirm_open();
        if (condition == "history-open") return B().get_hist_open();
        if (condition == "history-closed") return !B().get_hist_open();
        if (condition == "sql-error") return B().get_sql_error();
        if (number_after("sql-tabs=", expected)) return (int)sql_tabs_.size() == expected;
        if (condition == "sql-text-empty") return U(B().get_sql_text()).empty();
        if (condition == "sql-result-empty") return sql_out_.columns.empty();
        if (condition.rfind("sql-text-has=", 0) == 0) return U(B().get_sql_text()).find(condition.substr(13)) != std::string::npos;
        if (condition == "columns-open") return B().get_colcfg_open();
        if (number_after("columns-hidden=", expected)) { int n = 0; for (bool h : view_.hidden) n += h ? 1 : 0; return n == expected; }
        if (number_after("columns-first=", expected)) { return !view_.order.empty() && (int)view_.order[0] == expected; }
        if (condition == "paste-open") return B().get_paste_open();
        if (condition == "copyas-open") return B().get_copyas_open();
        if (number_after("paste-rows=", expected)) return (int)paste_rows_.size() == expected;
        if (condition == "sql-idle") return !sql_running_;
        if (condition == "sql-ok") return !B().get_sql_error() && !sql_running_ && !sql_out_.columns.empty();
        if (number_after("history-entries=", expected)) return (int)history_.entries().size() == expected;
        if (number_after("history-rows=", expected)) return (int)hist_model_->row_count() == expected;
        if (number_after("history-favorites=", expected)) {
            int n = 0;
            for (const auto &e : history_.entries()) n += e.favorite ? 1 : 0;
            return n == expected;
        }
        if (condition == "sql-text-is-history-second") return history_.entries().size() > 1 && U(B().get_sql_text()) == history_.entries()[1].text;
        if (condition == "monitor-supported") return B().get_mon_supported();
        if (condition == "monitor-unsupported") return !B().get_mon_supported();
        if (condition == "monitor-error") return !U(B().get_mon_error()).empty();
        if (number_after("tab-count=", expected)) return (int)tabs_.size() == expected;
        if (number_after("monitor-metrics>=", expected)) return (int)mon_activity_.metrics.size() >= expected;
        if (number_after("page-size-index=", expected)) return B().get_settings_page_size_index() == expected;
        return false;
    }

    // ------------------------------------------------------------------ plumbing
    const Backend &B() const { return ui_->global<Backend>(); }

    template <class F> void post(F f)
    {
        slint::invoke_from_event_loop([alive = alive_, f = std::move(f)]() mutable {
            if (alive->load(std::memory_order_acquire)) f();
        });
    }
    template <class F> void spawn(F f)
    {
        ++workers_;
        std::thread([this, f = std::move(f)]() mutable {
            try {
                f();
            } catch (...) {
            }
            --workers_;
        }).detach();
    }

    void shutdown()
    {
        store_.settings.page_size = page_size_;
        store_.save();
        if (fetch_cancel_) fetch_cancel_->cancel();
        if (sql_cancel_) sql_cancel_->cancel();
        if (exp_cancel_) exp_cancel_->cancel();
        stop_monitor();
        // Detached tasks capture this Controller. Do not destroy it while a connector call
        // may still be using it; cancellation is best-effort for drivers that block in I/O.
        while (workers_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    void set_status(const std::string &s) { B().set_status(S(s)); }

    void set_busy()
    {
        B().set_busy(connecting_ || loading_ || saving_ || exporting_ || sql_running_);
    }

    // ------------------------------------------------------------------ dialogs
    void confirm(const std::string &title, const std::string &text, const std::string &detail, const std::string &ok,
                 const std::string &alt, bool danger, bool prod, std::function<void(int)> cb, bool info = false)
    {
        dlg_cb_ = std::move(cb);
        B().set_dlg_title(S(title));
        B().set_dlg_text(S(text));
        B().set_dlg_detail(S(detail));
        B().set_dlg_ok(S(ok));
        B().set_dlg_alt(S(alt));
        B().set_dlg_danger(danger);
        B().set_dlg_prod(prod);
        B().set_dlg_info(info);
        B().set_dlg_confirm_open(true);
    }
    void notify(const std::string &title, const std::string &text)
    {
        confirm(title, text, "", tr("ok"), "", false, false, nullptr, true);
    }
    bool is_prod() const { return connected_ && profile_.env == (int)Env::Production; }

    // ------------------------------------------------------------------ wiring
    void wire()
    {
        auto &b = ui_->global<Backend>();
        b.on_tr([](slint::SharedString key, int) { return S(tr(U(key))); });

        b.on_toggle_theme([this] { set_dark(!store_.settings.dark); });
        b.on_toggle_language([this] { set_language(store_.settings.lang == "es" ? "en" : "es"); });
        b.on_open_settings([this] { open_settings(); });
        b.on_settings_theme([this](int i) { set_dark(i == 0); });
        b.on_settings_lang([this](int i) { set_language(i == 0 ? "es" : "en"); });
        b.on_settings_auto_update([this](bool on) {
            store_.settings.auto_update = on;
            B().set_auto_update(on);
            store_.save();
        });
        b.on_settings_monitor_interval([this](int i) {
            store_.settings.monitor_interval = kMonitorIntervals[std::clamp(i, 0, 4)];
            B().set_monitor_interval_index(monitor_interval_index());
            store_.save();
            if (monitor_active()) start_monitor();
        });
        b.on_settings_query_timeout([this](int i) {
            store_.settings.query_timeout = kQueryTimeouts[std::clamp(i, 0, 4)];
            B().set_query_timeout_index(query_timeout_index());
            store_.save();
        });
        b.on_settings_page_size([this](int i) {
            const int size = kPageSizes[std::clamp(i, 0, 4)];
            guard([this, size] {
                page_size_ = size;
                store_.settings.page_size = page_size_;
                B().set_page_size_index(page_size_index());
                B().set_settings_page_size_index(page_size_index());
                store_.save();
                if (table_open_) {
                    req_.limit = (uint32_t)page_size_;
                    req_.offset = 0;
                    load_page(false);
                }
            });
        });
        b.on_check_updates([this] { check_updates(false); });
        b.on_start_update([this] { start_update(); });
        b.on_open_data_dir([this] { platform::open_folder(platform::data_dir()); });
        b.on_open_drivers_dir([this] { platform::open_folder(drivers::user_drivers_dir()); });
        b.on_open_logs_dir([this] { platform::open_folder(platform::data_dir() + "\\logs"); });

        // tabs and monitor
        b.on_tab_clicked([this](int i) { switch_tab(i); });
        b.on_tab_close([this](int i) { close_tab(i); });
        b.on_sql_tab_clicked([this](int i) { switch_sql_tab(i); });
        b.on_sql_tab_close([this](int i) { close_sql_tab(i); });
        b.on_sql_tab_new([this] { new_sql_tab(); });
        b.on_view_changed([this](int v) { view_changed(v); });
        b.on_mon_select([this](int i) { monitor_select(i); });
        b.on_mon_search_changed([this](slint::SharedString s) {
            mon_search_ = U(s);
            render_activity();
        });
        b.on_mon_toggle_blocked([this] {
            mon_blocked_only_ = !mon_blocked_only_;
            B().set_mon_blocked_only(mon_blocked_only_);
            render_activity();
        });
        b.on_mon_sort([this](int i) {
            if (i == mon_sort_col_) mon_sort_ascending_ = !mon_sort_ascending_;
            else { mon_sort_col_ = i; mon_sort_ascending_ = true; }
            render_activity();
        });
        b.on_mon_kill([this] { monitor_kill(); });
        b.on_mon_toggle_pause([this] {
            mon_paused_ = !mon_paused_;
            B().set_mon_paused(mon_paused_);
            if (!mon_paused_) poll_monitor(true);
        });
        b.on_mon_refresh([this] { poll_monitor(true); });
        b.on_about([this] {
            B().set_about_version(S(brand::kVersion));
            B().set_about_copyright(S(brand::kCopyright));
            B().set_about_repository(S(brand::kRepositoryUrl));
            B().set_about_open(true);
        });
        b.on_open_repository([] { platform::open_url(brand::kRepositoryUrl); });

        // explorer
        b.on_tree_filter_changed([this] { rebuild_tree(); });
        b.on_tree_clicked([this](int i) { tree_click(i); });
        b.on_tree_toggled([](int) {});
        b.on_tree_activated([](int) {});
        b.on_tree_edit([this](int i) { open_connection_editor(i); });
        b.on_tree_disconnect([this](int) { guard_all([this] { disconnect(); }); });
        b.on_tree_delete([this](int i) { delete_profile(i); });
        b.on_refresh_tree([this] { refresh_tree(); });
        b.on_new_connection([this] { open_connection_editor(-1); });

        // grid
        b.on_sort_clicked([this](int c, bool shift) { guard([this, c, shift] { sort_by(c, shift); }); });
        b.on_filter_edited([this](int c, slint::SharedString t) { filter_edited(c, U(t)); });
        b.on_filter_accepted([this](int) { apply_filters(true); });
        b.on_prepare_op_menu([this](int c) { prepare_op_menu(c); });
        b.on_op_picked([this](int c, int i) { op_picked(c, i); });
        b.on_clear_filters([this] { clear_filters(); });
        b.on_cell_clicked([this](int r, int c) { cell_clicked(r, c); });
        b.on_cell_double_clicked([this](int r, int c) { begin_edit(r, c, ""); });
        b.on_begin_edit([this](int r, int c, slint::SharedString t) {
            std::string text = U(t);
            if (!text.empty() && !starts_with_printable(text)) return; // Shift, F-keys, ...
            begin_edit(r, c, text);
        });
        b.on_commit_edit([this](slint::SharedString t, int mv) { commit_edit(U(t), mv); });
        b.on_cancel_edit([this] { B().set_edit_row(-1); B().set_edit_col(-1); });
        b.on_move_selection([this](int dr, int dc) { move_selection(dr, dc); });
        b.on_set_null([this] { set_null(); });
        b.on_save([this] { start_save(nullptr); });
        b.on_discard([this] { discard_changes(); set_status(tr("status.discarded")); });
        b.on_undo([this] { if (changes_.undo()) { refresh_rows(); update_pending(); } });
        b.on_redo([this] { if (changes_.redo()) { refresh_rows(); update_pending(); } });
        b.on_add_row([this] { add_row(false); });
        b.on_duplicate_row([this] { add_row(true); });
        b.on_delete_rows([this] { delete_row(); });
        b.on_revert_cell([this] { revert_cell(); });
        b.on_copy_cell([this] { copy_cell(); });
        b.on_copy_row([this] { copy_row(); });
        b.on_first_page([this] { guard([this] { req_.offset = 0; load_page(false); }); });
        b.on_prev_page([this] { guard([this] { req_.offset = req_.offset > req_.limit ? req_.offset - req_.limit : 0; load_page(false); }); });
        b.on_next_page([this] { guard([this] { req_.offset += req_.limit; load_page(false); }); });
        b.on_reload([this] { guard([this] { load_page(true); }); });
        b.on_count_total([this] { count_total(); });
        b.on_export_data([this] { open_export(false); });
        b.on_column_resized([this](int c, float w) { resize_column(widths_, c, w, false); });
        b.on_page_size_picked([this](int i) {
            guard([this, i] {
                page_size_ = kPageSizes[std::clamp(i, 0, 4)];
                req_.limit = (uint32_t)page_size_;
                req_.offset = 0;
                if (table_open_) load_page(false);
            });
            B().set_page_size_index(page_size_index());
        });
        b.on_toggle_query([this] { B().set_show_query(!B().get_show_query()); });
        b.on_open_detail([this] { open_detail(); });

        // SQL editor
        b.on_run_sql([this](slint::SharedString t) { run_sql(U(t)); });
        b.on_selection_begin([this] { saved_clipboard_ = platform::clipboard_get(); });
        b.on_run_sql_selected([this](slint::SharedString full) {
            // Slint does not expose the selection offsets, so the editor copies it and we read it back.
            std::optional<std::string> sel = platform::clipboard_get();
            if (saved_clipboard_) platform::clipboard_set(*saved_clipboard_);
            saved_clipboard_.reset();
            bool blank = true;
            if (sel) for (char ch : *sel) if (!std::isspace((unsigned char)ch)) blank = false;
            run_sql(blank ? U(full) : *sel);
        });
        b.on_cancel_sql([this] { if (sql_cancel_) sql_cancel_->cancel(); });
        b.on_sql_column_resized([this](int c, float w) { resize_column(sql_widths_, c, w, true); });
        wire_history(b);
        wire_columns(b);
        wire_clipboard(b);
        b.on_open_sql_file([this] { open_sql_file(); });
        b.on_save_sql_file([this](slint::SharedString t) { save_sql_file(U(t)); });
        b.on_export_sql_result([this] { open_export(true); });
        b.on_copy_sql_result([this] { copy_sql_result(); });

        // dialogs
        b.on_dlg_result([this](int r) {
            B().set_dlg_confirm_open(false);
            auto cb = std::move(dlg_cb_);
            dlg_cb_ = nullptr;
            if (cb) cb(r);
        });
        b.on_cd_engine_picked([this](int i) { engine_picked(i); });
        b.on_cd_driver_install([this] { install_driver(); });
        b.on_cd_browse([this] {
            const bool duck = std::string(kEngines[std::clamp(B().get_cd_engine(), 0, kEngineCount - 1)].id) == "duckdb";
            auto p = platform::open_dialog(tr("conn.file"), { duck ? platform::Filter{ "DuckDB", "*.duckdb;*.db;*.ddb" } : platform::Filter{ "SQLite", "*.db;*.sqlite;*.sqlite3;*.db3" }, { "*.*", "*.*" } });
            if (p) B().set_cd_path(S(*p));
        });
        b.on_cd_test([this] { test_connection(); });
        b.on_cd_save([this](bool connect) { save_connection(connect); });
        b.on_cd_cancel([this] { B().set_conn_dlg_open(false); });
        b.on_exp_start([this] { start_export(); });
        b.on_exp_cancel([this] { if (exp_cancel_) exp_cancel_->cancel(); });
    }

    // ------------------------------------------------------------------ connection manager
    void refresh_engines()
    {
        std::vector<slint::SharedString> v;
        for (auto &e : kEngines) {
            auto st = drivers::probe_for_engine(e.id);
            const char *suffix = st.state == drivers::State::Missing ? "engine.download" : st.state == drivers::State::NotImplemented ? "engine.soon" : nullptr;
            std::string label = e.label;
            if (suffix) label += tr(suffix);
            else if (e.status == EngineStatus::Experimental) label += tr("engine.experimental");
            v.push_back(S(label));
        }
        engines_model_->set_vector(v);
    }

    // Shows whether the selected engine's driver is usable and offers to download it when it is not.
    void refresh_driver_panel()
    {
        const EngineDef &e = kEngines[std::clamp(B().get_cd_engine(), 0, kEngineCount - 1)];
        auto st = drivers::status_for_engine(e.id);
        int code = 0;
        std::string text;
        switch (st.state) {
        case drivers::State::Missing: code = 1; text = trf("driver.missing", { e.label }); break;
        case drivers::State::Incompatible: code = 2; text = trf("driver.rejected", { e.label, st.detail }); break;
        case drivers::State::NotImplemented: code = 3; text = trf("driver.unimplemented", { e.label }); break;
        default: break;
        }
        if (!code && e.note[0]) {
            code = 4;
            text = tr(e.note);
        }
        B().set_cd_driver(code);
        B().set_cd_driver_text(S(text));
    }

    void install_driver()
    {
        if (B().get_cd_driver_busy()) return;
        std::string engine = kEngines[std::clamp(B().get_cd_engine(), 0, kEngineCount - 1)].id;
        B().set_cd_driver_busy(true);
        B().set_cd_driver_text(S(tr("driver.installing")));
        spawn([this, engine] {
            std::string err;
            bool ok = drivers::install_for_engine(
                engine, [this](const std::string &m) { post([this, m] { B().set_cd_driver_text(S(m)); }); }, nullptr, err);
            post([this, ok, err] {
                B().set_cd_driver_busy(false);
                refresh_engines();
                refresh_driver_panel();
                if (ok) {
                    B().set_cd_message(S(tr("driver.installed")));
                    B().set_cd_message_error(false);
                } else {
                    B().set_cd_driver(1);
                    B().set_cd_driver_text(S(trf("driver.failed", { err })));
                }
            });
        });
    }

    void engine_picked(int i)
    {
        i = std::clamp(i, 0, kEngineCount - 1);
        int old_default = kEngines[std::clamp(B().get_cd_engine(), 0, kEngineCount - 1)].port;
        std::string cur = U(B().get_cd_port());
        B().set_cd_engine(i);
        B().set_cd_file_based(kEngines[i].port == 0);
        if (cur.empty() || cur == std::to_string(old_default)) B().set_cd_port(S(kEngines[i].port ? std::to_string(kEngines[i].port) : ""));
        B().set_cd_message(S(""));
        refresh_driver_panel();
    }

    void open_connection_editor(int flat_index)
    {
        editing_id_.clear();
        B().set_cd_message(S(""));
        B().set_cd_testing(false);
        if (flat_index >= 0 && flat_index < (int)flat_.size() && flat_[flat_index].profile >= 0) {
            const Profile &p = store_.profiles[flat_[flat_index].profile];
            editing_id_ = p.id;
            B().set_conn_dlg_edit(true);
            B().set_cd_name(S(p.name));
            int ei = 0;
            for (int k = 0; k < kEngineCount; ++k)
                if (p.engine == kEngines[k].id) ei = k;
            B().set_cd_engine(ei);
            B().set_cd_file_based(is_file_engine(p.engine));
            B().set_cd_host(S(p.host));
            B().set_cd_port(S(p.port ? std::to_string(p.port) : ""));
            B().set_cd_database(S(p.database));
            B().set_cd_user(S(p.user));
            B().set_cd_password(S(""));
            B().set_cd_path(S(p.path));
            B().set_cd_env(p.env);
            B().set_cd_read_only(p.read_only);
            B().set_cd_tls(p.tls);
            B().set_cd_trust(p.trust_server_cert);
        } else {
            B().set_conn_dlg_edit(false);
            B().set_cd_name(S(""));
            B().set_cd_engine(0);
            B().set_cd_file_based(true);
            B().set_cd_host(S("localhost"));
            B().set_cd_port(S(""));
            B().set_cd_database(S(""));
            B().set_cd_user(S(""));
            B().set_cd_password(S(""));
            B().set_cd_path(S(""));
            B().set_cd_env(0);
            B().set_cd_read_only(false);
            B().set_cd_tls(true);
            B().set_cd_trust(false);
        }
        refresh_driver_panel();
        B().set_conn_dlg_open(true);
    }

    // Reads the dialog into a profile; returns an error text (empty when valid).
    std::string read_profile(Profile &p)
    {
        int ei = std::clamp(B().get_cd_engine(), 0, kEngineCount - 1);
        p = Profile();
        p.id = editing_id_.empty() ? Store::new_id() : editing_id_;
        p.name = U(B().get_cd_name());
        p.engine = kEngines[ei].id;
        p.env = std::clamp(B().get_cd_env(), 0, 3);
        p.read_only = B().get_cd_read_only();
        p.tls = B().get_cd_tls();
        p.trust_server_cert = p.tls && B().get_cd_trust();
        while (!p.name.empty() && p.name.back() == ' ') p.name.pop_back();
        if (p.name.empty()) return tr("conn.need-name");
        if (drivers::probe_for_engine(p.engine).state == drivers::State::NotImplemented) return tr("conn.engine-unavailable");
        if (is_file_engine(p.engine)) {
            p.path = U(B().get_cd_path());
            if (p.path.empty()) return tr("conn.need-path");
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::u8path(p.path), ec)) return tr("conn.file-missing");
        } else {
            p.host = U(B().get_cd_host());
            if (p.host.empty()) return tr("conn.need-host");
            std::string port = U(B().get_cd_port());
            if (!port.empty()) {
                char *end = nullptr;
                long n = std::strtol(port.c_str(), &end, 10);
                if (*end || n < 1 || n > 65535) return tr("conn.bad-port");
                p.port = (int)n;
            }
            p.database = U(B().get_cd_database());
            p.user = U(B().get_cd_user());
        }
        return {};
    }

    std::string password_for(const Profile &p, const std::string &typed)
    {
        if (!typed.empty()) return typed;
        auto saved = platform::cred_read(std::string(brand::kCredentialPrefix) + p.id);
        return saved ? *saved : std::string();
    }

    void test_connection()
    {
        Profile p;
        std::string err = read_profile(p);
        auto show = [this](const std::string &m, bool bad) {
            B().set_cd_message(S(m));
            B().set_cd_message_error(bad);
        };
        if (!err.empty()) { show(err, true); return; }
        std::string pwd = password_for(p, U(B().get_cd_password()));
        B().set_cd_testing(true);
        show("", false);
        spawn([this, p, pwd] {
            try {
                auto c = make_connector(p, pwd);
                std::string info = c->server_info();
                c->close();
                post([this, info] {
                    B().set_cd_testing(false);
                    B().set_cd_message(S(trf("conn.test-ok", { info })));
                    B().set_cd_message_error(false);
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                post([this, m] {
                    B().set_cd_testing(false);
                    B().set_cd_message(S(m));
                    B().set_cd_message_error(true);
                });
            }
        });
    }

    void save_connection(bool connect_after)
    {
        Profile p;
        std::string err = read_profile(p);
        if (!err.empty()) {
            B().set_cd_message(S(err));
            B().set_cd_message_error(true);
            return;
        }
        std::string pwd = U(B().get_cd_password());
        if (!pwd.empty() && !platform::cred_write(std::string(brand::kCredentialPrefix) + p.id, p.user, pwd)) {
            B().set_cd_message(S(tr("conn.pwd-failed")));
            B().set_cd_message_error(true);
            return;
        }
        store_.upsert(p);
        store_.save();
        B().set_conn_dlg_open(false);
        rebuild_tree();
        if (connect_after) guard_all([this, p] { connect(p); });
    }

    void delete_profile(int flat_index)
    {
        if (flat_index < 0 || flat_index >= (int)flat_.size() || flat_[flat_index].profile < 0) return;
        Profile p = store_.profiles[flat_[flat_index].profile];
        confirm(tr("tree.delete-title"), trf("tree.delete-text", { p.name }), "", tr("delete"), "", true, false, [this, p](int r) {
            if (r != 1) return;
            if (connected_ && profile_.id == p.id) disconnect();
            platform::cred_delete(std::string(brand::kCredentialPrefix) + p.id);
            store_.remove(p.id);
            store_.save();
            rebuild_tree();
        });
    }

    // ------------------------------------------------------------------ connect / explorer
    void connect(const Profile &p)
    {
        if (connecting_) return;
        last_connection_failed_ = false;
        disconnect();
        connecting_ = true;
        connecting_id_ = p.id;
        uint64_t cg = ++conn_gen_;
        set_status(trf("status.connecting", { p.name }));
        set_busy();
        rebuild_tree();
        std::string pwd = password_for(p, "");
        spawn([this, p, pwd, cg] {
            try {
                std::shared_ptr<Connector> c = make_connector(p, pwd);
                std::string info = c->server_info();
                auto kids = list_children(*c, 0, "", "");
                post([this, p, c, info, cg, kids = std::move(kids)]() mutable {
                    if (cg != conn_gen_) { c->close(); return; }
                    connecting_ = false;
                    connecting_id_.clear();
                    conn_ = c;
                    caps_ = c->caps();
                    profile_ = p;
                    connected_ = true;
                    root_ = Node();
                    root_.label = p.name;
                    root_.expanded = root_.loaded = true;
                    root_.kids = std::move(kids);
                    B().set_connected(true);
                    B().set_conn_name(S(p.name));
                    B().set_conn_env(p.env);
                    B().set_conn_read_only(p.read_only);
                    B().set_server_info(S(info));
                    reset_monitor_ui();
                    if (B().get_view() == 2) start_monitor();
                    set_status(trf("status.connected", { p.name }) + " Ã¢â‚¬â€ " + info);
                    set_busy();
                    rebuild_tree();
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                post([this, cg, m] {
                    if (cg != conn_gen_) return;
                    connecting_ = false;
                    connecting_id_.clear();
                    last_connection_failed_ = true;
                    set_status(trf("status.connect-failed", { m }));
                    set_busy();
                    rebuild_tree();
                    notify(tr("status.disconnected"), trf("status.connect-failed", { m }));
                });
            }
        });
    }

    // A connection-level failure on the live connector: keep the session and the unsaved changes,
    // retry with growing waits, and only ever swap the connector (never reload data over pending edits).
    void on_connection_lost(const std::shared_ptr<Connector> &failed)
    {
        if (!connected_ || reconnecting_ || failed != conn_) return;
        reconnecting_ = true;
        reconnect_attempt_ = 0;
        schedule_reconnect();
    }

    void schedule_reconnect()
    {
        int wait = reconnect_delay_seconds(reconnect_attempt_);
        set_status(trf("status.reconnecting", { profile_.name, std::to_string(reconnect_attempt_ + 1), std::to_string(kReconnectAttempts), std::to_string(wait) }));
        reconnect_timer_.start(slint::TimerMode::SingleShot, std::chrono::seconds(wait), [this] { try_reconnect(); });
    }

    void try_reconnect()
    {
        if (!connected_ || !reconnecting_) return;
        uint64_t cg = conn_gen_;
        Profile p = profile_;
        std::string pwd = password_for(p, "");
        spawn([this, p, pwd, cg] {
            try {
                std::shared_ptr<Connector> c = make_connector(p, pwd);
                std::string info = c->server_info();
                post([this, c, info, cg] {
                    if (cg != conn_gen_ || !reconnecting_) { c->close(); return; }
                    auto old = conn_;
                    conn_ = c;
                    caps_ = c->caps();
                    reconnecting_ = false;
                    spawn([old] { try { old->close(); } catch (...) {} });
                    B().set_server_info(S(info));
                    set_status(trf("status.reconnected", { profile_.name }) + (changes_.empty() ? "" : " Ã¢â‚¬â€ " + tr("status.reconnected-pending")));
                    if (changes_.empty() && table_open_) load_page(true);
                });
            } catch (const std::exception &) {
                post([this, cg] {
                    if (cg != conn_gen_ || !reconnecting_) return;
                    if (++reconnect_attempt_ >= kReconnectAttempts) {
                        reconnecting_ = false;
                        set_status(trf("status.reconnect-failed", { profile_.name }));
                        notify(tr("status.disconnected"), trf("status.reconnect-failed", { profile_.name }));
                        return;
                    }
                    schedule_reconnect();
                });
            }
        });
    }

    void disconnect()
    {
        if (!connected_) return;
        reconnecting_ = false;
        reconnect_timer_.stop();
        ++conn_gen_;
        if (fetch_cancel_) fetch_cancel_->cancel();
        if (sql_cancel_) sql_cancel_->cancel();
        ++fetch_gen_;
        auto c = conn_;
        conn_.reset();
        connected_ = false;
        table_open_ = false;
        loading_ = false;
        changes_.clear();
        rows_.clear();
        root_ = Node();
        spawn([c] { c->close(); });
        stop_monitor();
        clear_tabs();
        B().set_connected(false);
        B().set_has_table(false);
        B().set_conn_name(S(""));
        B().set_conn_env(0);
        B().set_conn_read_only(false);
        B().set_server_info(S(""));
        B().set_edit_row(-1);
        reset_monitor_ui();
        clear_table_ui();
        set_status(tr("status.disconnected"));
        set_busy();
        rebuild_tree();
    }

    void refresh_tree()
    {
        if (!connected_ || root_.busy) return;
        root_.busy = true;
        rebuild_tree();
        uint64_t cg = conn_gen_;
        auto c = conn_;
        spawn([this, c, cg] {
            try {
                auto kids = list_children(*c, 0, "", "");
                post([this, cg, kids = std::move(kids)]() mutable {
                    if (cg != conn_gen_) return;
                    root_.kids = std::move(kids);
                    root_.busy = false;
                    rebuild_tree();
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                post([this, cg, m] {
                    if (cg != conn_gen_) return;
                    root_.busy = false;
                    rebuild_tree();
                    set_status(m);
                });
            }
        });
    }

    struct Flat {
        int profile = -1;
        std::string key; // node key for non-connection rows
    };

    Node *find_node(Node &n, const std::string &key)
    {
        if (n.kind != 0 && n.key() == key) return &n;
        for (auto &k : n.kids)
            if (Node *f = find_node(k, key)) return f;
        return nullptr;
    }

    static bool node_matches(const Node &n, const std::string &f)
    {
        if (f.empty()) return true;
        if (n.kind >= 3) return lower(n.label).find(f) != std::string::npos;
        for (auto &k : n.kids)
            if (node_matches(k, f)) return true;
        return lower(n.label).find(f) != std::string::npos;
    }

    void add_nodes(const Node &parent, int depth, const std::string &f, std::vector<TreeRowUi> &rows)
    {
        for (auto &k : parent.kids) {
            if (!f.empty() && !node_matches(k, f)) continue;
            TreeRowUi r {};
            r.label = S(k.label);
            r.depth = depth;
            r.kind = k.kind;
            r.expanded = k.expanded || !f.empty();
            r.busy = k.busy;
            r.connected = true;
            r.active = table_open_ && k.kind >= 3 && k.db == table_ref_.database && k.schema == table_ref_.schema && k.name == table_ref_.name;
            rows.push_back(r);
            flat_.push_back({ -1, k.key() });
            if (k.kind <= 2 && (k.expanded || !f.empty())) add_nodes(k, depth + 1, f, rows);
        }
    }

    void rebuild_tree()
    {
        flat_.clear();
        std::vector<TreeRowUi> rows;
        std::string f = lower(U(B().get_tree_filter()));
        for (size_t pi = 0; pi < store_.profiles.size(); ++pi) {
            const Profile &p = store_.profiles[pi];
            bool act = connected_ && p.id == profile_.id;
            TreeRowUi r {};
            r.label = S(p.name);
            r.depth = 0;
            r.kind = 0;
            r.expanded = act && root_.expanded;
            r.busy = connecting_id_ == p.id || (act && root_.busy);
            r.env = p.env;
            r.connected = act;
            rows.push_back(r);
            flat_.push_back({ (int)pi, "" });
            if (act && root_.expanded) add_nodes(root_, 1, f, rows);
        }
        tree_model_->set_vector(rows);
    }

    void tree_click(int i)
    {
        if (i < 0 || i >= (int)flat_.size()) return;
        const Flat fl = flat_[i];
        if (fl.profile >= 0) {
            const Profile &p = store_.profiles[fl.profile];
            if (connected_ && p.id == profile_.id) {
                root_.expanded = !root_.expanded;
                rebuild_tree();
            } else {
                Profile copy = p;
                guard_all([this, copy] { connect(copy); });
            }
            return;
        }
        Node *n = find_node(root_, fl.key);
        if (!n) return;
        if (n->kind >= 3) {
            TableRef ref { n->db, n->schema, n->name };
            open_table(ref);
            return;
        }
        n->expanded = !n->expanded;
        if (n->expanded && !n->loaded && !n->busy) {
            n->busy = true;
            std::string key = n->key();
            int kind = n->kind;
            std::string db = n->db, schema = n->schema;
            uint64_t cg = conn_gen_;
            auto c = conn_;
            spawn([this, c, cg, key, kind, db, schema] {
                try {
                    auto kids = list_children(*c, kind, db, schema);
                    post([this, cg, key, kids = std::move(kids)]() mutable {
                        if (cg != conn_gen_) return;
                        if (Node *t = find_node(root_, key)) {
                            t->kids = std::move(kids);
                            t->loaded = true;
                            t->busy = false;
                        }
                        rebuild_tree();
                    });
                } catch (const std::exception &e) {
                    std::string m = e.what();
                    post([this, cg, key, m] {
                        if (cg != conn_gen_) return;
                        if (Node *t = find_node(root_, key)) { t->busy = false; t->expanded = false; }
                        rebuild_tree();
                        set_status(m);
                    });
                }
            });
        }
        rebuild_tree();
    }

    // ------------------------------------------------------------------ unsaved-change guard
    void guard(std::function<void()> proceed)
    {
        if (changes_.empty() || !table_open_) { proceed(); return; }
        confirm(tr("unsaved.title"), trf("unsaved.text", { std::to_string(changes_.affected_rows()) }), "", tr("save"), tr("unsaved.discard"), false, false,
                [this, proceed](int r) {
                    if (r == 2) { discard_changes(); proceed(); }
                    else if (r == 1) start_save(proceed);
                });
    }

    // Like guard(), but also covers the pending changes kept in background tabs.
    void guard_all(std::function<void()> proceed)
    {
        bool others = false;
        for (size_t i = 0; i < tabs_.size(); ++i)
            if ((int)i != active_tab_ && !tabs_[i].changes.empty()) others = true;
        if (!others) { guard(std::move(proceed)); return; }
        size_t n = changes_.empty() ? 0 : changes_.affected_rows();
        for (size_t i = 0; i < tabs_.size(); ++i)
            if ((int)i != active_tab_) n += tabs_[i].changes.affected_rows();
        confirm(tr("unsaved.title"), trf("unsaved.text", { std::to_string(n) }), "", tr("save"), tr("unsaved.discard"), true, false, [this, proceed](int r) {
            if (r == 1) save_all_dirty_tabs(proceed);
            else if (r == 2) {
                changes_.clear();
                for (auto &t : tabs_) t.changes.clear();
                update_pending();
                update_tabs_ui();
                proceed();
            }
        });
    }

    void save_all_dirty_tabs(std::function<void()> proceed)
    {
        if (!changes_.empty()) {
            start_save([this, proceed] { save_all_dirty_tabs(proceed); });
            return;
        }
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if ((int)i == active_tab_ || tabs_[i].changes.empty()) continue;
            switch_tab((int)i);
            start_save([this, proceed] { save_all_dirty_tabs(proceed); });
            return;
        }
        proceed();
    }

    void discard_changes()
    {
        changes_.clear();
        B().set_edit_row(-1);
        B().set_edit_col(-1);
        refresh_rows();
        update_pending();
    }

    // ------------------------------------------------------------------ table tabs
    // The Controller keeps ONE live set of table members (meta_, rows_, changes_, ...). Tabs not on screen hold
    // a snapshot in tabs_; switching swaps snapshots in and out.
    static bool same_table(const TableRef &a, const TableRef &b) { return a.database == b.database && a.schema == b.schema && a.name == b.name; }

    void update_tabs_ui()
    {
        std::vector<TabUi> v;
        for (size_t i = 0; i < tabs_.size(); ++i) {
            const TabState &t = tabs_[i];
            bool dup = false;
            for (size_t k = 0; k < tabs_.size(); ++k)
                if (k != i && tabs_[k].ref.name == t.ref.name) dup = true;
            TabUi u {};
            u.title = S(dup ? t.ref.label() : t.ref.name);
            u.active = (int)i == active_tab_;
            u.dirty = u.active ? !changes_.empty() : !t.changes.empty();
            v.push_back(u);
        }
        tabs_model_->set_vector(v);
    }

    void save_tab(int i)
    {
        if (i < 0 || i >= (int)tabs_.size()) return;
        TabState &t = tabs_[i];
        t.meta = meta_;
        t.req = req_;
        t.rows = std::move(rows_);
        t.changes = std::move(changes_);
        t.filters = filters_;
        t.widths = widths_;
        t.pending_group = pending_group_;
        t.pending_sig = pending_sig_;
        t.applied_sig = applied_sig_;
        t.query_text = U(B().get_query_text());
        t.applied_filter_count = applied_filter_count_;
        t.sel_row = B().get_sel_row();
        t.sel_col = B().get_sel_col();
        t.total_mode = total_mode_;
        t.total = total_;
        t.has_more = has_more_;
        t.editable = editable_;
        t.page_loaded = page_loaded_;
        rows_.clear();
        changes_ = ChangeSet();
    }

    void restore_tab(int i)
    {
        if (i < 0 || i >= (int)tabs_.size()) return;
        TabState &t = tabs_[i];
        meta_ = t.meta;
        table_ref_ = meta_.table;
        req_ = t.req;
        rows_ = std::move(t.rows);
        changes_ = std::move(t.changes);
        filters_ = t.filters;
        widths_ = t.widths;
        pending_group_ = t.pending_group;
        pending_sig_ = t.pending_sig;
        applied_sig_ = t.applied_sig;
        applied_filter_count_ = t.applied_filter_count;
        total_mode_ = t.total_mode;
        total_ = t.total;
        has_more_ = t.has_more;
        editable_ = t.editable;
        page_loaded_ = t.page_loaded;
        t.rows.clear();
        t.changes = ChangeSet();
        active_tab_ = i;
        table_open_ = true;

        ++fetch_gen_;
        if (fetch_cancel_) fetch_cancel_->cancel();
        if (count_cancel_) count_cancel_->cancel();
        filter_timer_.stop();
        loading_ = false;
        B().set_loading(false);
        apply_layout();
        B().set_has_table(true);
        B().set_table_title(S(meta_.table.label()));
        B().set_query_text(S(t.query_text));
        B().set_filter_error(S(""));
        B().set_filter_count(applied_filter_count_);
        B().set_edit_row(-1);
        B().set_edit_col(-1);
        B().set_sel_row(t.sel_row);
        B().set_sel_col(t.sel_col);
        update_blocked();
        refresh_columns();
        refresh_rows();
        update_pending();
        update_pager();
        scroll_to_row(std::max(0, t.sel_row));
        rebuild_tree();
        set_busy();
        if (!page_loaded_) load_page(false, true);
    }

    void switch_tab(int i)
    {
        if (i < 0 || i >= (int)tabs_.size() || i == active_tab_ || saving_) return;
        if (B().get_edit_row() >= 0) commit_edit(U(B().get_edit_text()), 0);
        save_tab(active_tab_);
        restore_tab(i);
    }

    void close_tab(int i)
    {
        if (i < 0 || i >= (int)tabs_.size() || saving_) return;
        bool dirty = i == active_tab_ ? !changes_.empty() : !tabs_[i].changes.empty();
        if (!dirty) { remove_tab(i); return; }
        if (i == active_tab_) {
            guard([this, i] { if (i < (int)tabs_.size()) remove_tab(i); });
        } else {
            confirm(tr("unsaved.title"), trf("unsaved.text", { std::to_string(tabs_[i].changes.affected_rows()) }), "", tr("save"), tr("unsaved.discard"), true, false, [this, i](int r) {
                if (r == 2 && i < (int)tabs_.size()) remove_tab(i);
                else if (r == 1 && i < (int)tabs_.size()) {
                    guard([this, i] {
                        if (i >= (int)tabs_.size()) return;
                        switch_tab(i);
                        start_save([this, i] { if (i < (int)tabs_.size()) remove_tab(i); });
                    });
                }
            });
        }
    }

    void remove_tab(int i)
    {
        if (i < 0 || i >= (int)tabs_.size()) return;
        if (i != active_tab_) {
            tabs_.erase(tabs_.begin() + i);
            if (i < active_tab_) --active_tab_;
            update_tabs_ui();
            return;
        }
        B().set_edit_row(-1);
        tabs_.erase(tabs_.begin() + i);
        changes_ = ChangeSet();
        rows_.clear();
        if (!tabs_.empty()) {
            restore_tab(std::min(i, (int)tabs_.size() - 1));
            return;
        }
        active_tab_ = -1;
        ++fetch_gen_;
        if (fetch_cancel_) fetch_cancel_->cancel();
        loading_ = false;
        table_open_ = false;
        B().set_loading(false);
        B().set_has_table(false);
        clear_table_ui();
        update_tabs_ui();
        rebuild_tree();
        set_busy();
    }

    void clear_tabs()
    {
        tabs_.clear();
        active_tab_ = -1;
        update_tabs_ui();
    }

    // ------------------------------------------------------------------ table loading
    int page_size_index() const
    {
        for (int k = 0; k < 5; ++k)
            if (kPageSizes[k] == page_size_) return k;
        return 1;
    }

    void clear_table_ui()
    {
        columns_model_->set_vector({});
        rows_model_->set_vector({});
        B().set_content_width(0);
        B().set_pending_count(0);
        B().set_table_title(S(""));
        B().set_query_text(S(""));
        B().set_filter_error(S(""));
        B().set_filter_count(0);
        B().set_sel_row(-1);
        B().set_sel_col(-1);
    }

    void open_table(const TableRef &ref)
    {
        if (!connected_ || saving_) return;
        for (size_t i = 0; i < tabs_.size(); ++i)
            if (same_table(tabs_[i].ref, ref)) { switch_tab((int)i); return; }
        if (tabs_.size() >= kMaxTabs) { notify(tr("tabs.limit-title"), trf("tabs.limit-text", { std::to_string(kMaxTabs) })); return; }
        {
            if (B().get_edit_row() >= 0) commit_edit(U(B().get_edit_text()), 0);
            uint64_t cg = conn_gen_;
            uint64_t fg = ++fetch_gen_;
            if (fetch_cancel_) fetch_cancel_->cancel();
            loading_ = true;
            B().set_loading(true);
            set_busy();
            set_status(tr("loading"));
            auto c = conn_;
            spawn([this, c, ref, cg, fg] {
                diagnostics::set_phase(diagnostics::Phase::LoadingTableMetadata);
                try {
                    TableMeta m = c->table_meta(ref);
                    post([this, cg, fg, ref, m = std::move(m)]() mutable {
                        if (cg != conn_gen_ || fg != fetch_gen_) return;
                        diagnostics::set_phase(diagnostics::Phase::LoadingTableMetadata);
                        if (active_tab_ >= 0) save_tab(active_tab_);
                        tabs_.emplace_back();
                        active_tab_ = (int)tabs_.size() - 1;
                        tabs_.back().ref = ref;
                        meta_ = std::move(m);
                        table_ref_ = meta_.table;
                        table_open_ = true;
                        page_loaded_ = false;
                        req_ = PageRequest();
                        req_.limit = (uint32_t)page_size_;
                        filters_.assign(meta_.columns.size(), ColFilter());
                        applied_sig_.clear();
                        total_mode_ = TotalMode::Unknown;
                        total_ = 0;
                        if (meta_.estimated_rows >= 0) { total_mode_ = TotalMode::Estimated; total_ = (uint64_t)meta_.estimated_rows; }
                        changes_.clear();
                        rows_.clear();
                        widths_.clear();
                        for (auto &col : meta_.columns) widths_.push_back(default_width(col));
                        editable_ = meta_.edit.editable && caps_.edit && !profile_.read_only && !meta_.is_view;
                        apply_layout();
                        B().set_has_table(true);
                        B().set_table_title(S(meta_.table.label()));
                        B().set_sel_row(-1);
                        B().set_sel_col(-1);
                        B().set_edit_row(-1);
                        update_blocked();
                        refresh_columns();
                        update_pending();
                        rebuild_tree();
                        load_page(false, true);
                        diagnostics::set_phase(diagnostics::Phase::Idle);
                    });
                } catch (const std::exception &e) {
                    std::string m = e.what();
                    post([this, cg, fg, m] {
                        if (cg != conn_gen_ || fg != fetch_gen_) return;
                        diagnostics::set_phase(diagnostics::Phase::Idle);
                        loading_ = false;
                        B().set_loading(false);
                        set_busy();
                        set_status(trf("status.query-failed", { m }));
                        if (table_open_ && !page_loaded_) load_page(false, true); // the tab on screen lost its load to this request
                    });
                }
            });
        }
    }

    static double default_width(const ColumnInfo &c)
    {
        double w = 150;
        switch (c.cls) {
        case TypeClass::Integer: w = 90; break;
        case TypeClass::Decimal:
        case TypeClass::Float: w = 110; break;
        case TypeClass::Bool: w = 70; break;
        case TypeClass::Date: w = 110; break;
        case TypeClass::Time: w = 90; break;
        case TypeClass::DateTime: w = 160; break;
        case TypeClass::Uuid: w = 280; break;
        case TypeClass::Json: w = 200; break;
        case TypeClass::Text: w = 180; break;
        case TypeClass::LongText: w = 240; break;
        case TypeClass::Binary: w = 120; break;
        default: break;
        }
        return std::max(w, (double)c.name.size() * 8.0 + 44.0);
    }

    void update_blocked()
    {
        std::string r;
        if (table_open_) {
            if (meta_.is_view) r = tr("blocked.view");
            else if (profile_.read_only) r = tr("blocked.read-only");
            else if (!meta_.edit.editable) r = meta_.edit.key_columns.empty() ? tr("blocked.no-key") : meta_.edit.blocked_reason;
        }
        B().set_editable(editable_);
        B().set_blocked_reason(S(r));
    }

    void load_page(bool reset_total, bool first = false)
    {
        if (!table_open_ || !connected_) return;
        if (reset_total && applied_filter_count_ >= 0) {
            total_mode_ = (req_.filter.empty() && meta_.estimated_rows >= 0) ? TotalMode::Estimated : TotalMode::Unknown;
            total_ = (total_mode_ == TotalMode::Estimated) ? (uint64_t)meta_.estimated_rows : 0;
        }
        uint64_t fg = ++fetch_gen_;
        if (fetch_cancel_ && !first) fetch_cancel_->cancel();
        fetch_cancel_ = std::make_shared<CancelToken>();
        auto tok = fetch_cancel_;
        auto c = conn_;
        TableMeta meta = meta_;
        PageRequest req = req_;
        loading_ = true;
        B().set_loading(true);
        set_busy();
        int limit = store_.settings.query_timeout;
        spawn([this, c, meta = std::move(meta), req = std::move(req), tok, fg, limit] {
            diagnostics::set_phase(diagnostics::Phase::FetchingPage);
            try {
                Deadline dl(tok, limit);
                Page p = c->fetch_page(meta, req, tok);
                post([this, fg, p = std::move(p)]() mutable {
                    diagnostics::set_phase(diagnostics::Phase::RenderingPage);
                    on_page(fg, std::move(p));
                    diagnostics::set_phase(diagnostics::Phase::Idle);
                });
            } catch (const DbError &e) {
                std::string m = e.what();
                bool cancelled = e.kind == ErrKind::Cancelled;
                bool timed_out = tok->timed_out();
                bool lost = is_connection_lost(e.kind);
                post([this, fg, m, cancelled, timed_out, limit, lost, c] {
                    if (fg == fetch_gen_) {
                        diagnostics::set_phase(diagnostics::Phase::Idle);
                        loading_ = false;
                        B().set_loading(false);
                        set_busy();
                        set_status(timed_out ? trf("status.timeout", { std::to_string(limit) }) : cancelled ? tr("status.cancelled") : trf("status.query-failed", { m }));
                    }
                    if (lost) on_connection_lost(c);
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                post([this, fg, m] {
                    if (fg != fetch_gen_) return;
                    diagnostics::set_phase(diagnostics::Phase::Idle);
                    loading_ = false;
                    B().set_loading(false);
                    set_busy();
                    set_status(trf("status.query-failed", { m }));
                });
            }
        });
    }

    void on_page(uint64_t fg, Page p)
    {
        if (fg != fetch_gen_) return; // a newer request superseded this one
        loading_ = false;
        B().set_loading(false);
        set_busy();
        if (p.rows.empty() && req_.offset > 0) {
            req_.offset = req_.offset > req_.limit ? req_.offset - req_.limit : 0;
            load_page(false);
            return;
        }
        rows_ = std::move(p.rows);
        page_loaded_ = true;
        has_more_ = p.has_more;
        changes_.clear();
        if (!p.has_more) {
            total_mode_ = TotalMode::Exact;
            total_ = req_.offset + rows_.size();
        }
        B().set_edit_row(-1);
        B().set_edit_col(-1);
        B().set_sel_row(rows_.empty() ? -1 : 0);
        if (B().get_sel_col() < 0 && !meta_.columns.empty()) B().set_sel_col(0);
        B().set_query_text(S(p.generated));
        scroll_to_row(0);
        refresh_rows();
        update_pending();
        update_pager();
        set_status(trf("status.loaded", { i18n::with_commas(rows_.size()), std::to_string(p.elapsed_ms) }));
    }

    void scroll_to_row(int r)
    {
        B().set_scroll_row(r);
        B().set_scroll_token(B().get_scroll_token() + 1);
    }

    void update_pager()
    {
        if (!table_open_) return;
        std::string range = rows_.empty() ? tr("pager.empty")
                                          : trf("pager.range", { i18n::with_commas(req_.offset + 1), i18n::with_commas(req_.offset + rows_.size()) });
        B().set_range_text(S(range));
        std::string total;
        switch (total_mode_) {
        case TotalMode::Unknown: total = counting_ ? tr("pager.counting") : tr("pager.total-unknown"); break;
        case TotalMode::Estimated: total = trf("pager.total-est", { i18n::with_commas(total_) }); break;
        case TotalMode::Exact: total = trf(applied_filter_count_ > 0 ? "pager.filtered-exact" : "pager.total-exact", { i18n::with_commas(total_) }); break;
        }
        B().set_total_text(S(total));
        B().set_can_count(total_mode_ != TotalMode::Exact && !counting_);
        B().set_page_index((int)(req_.limit ? req_.offset / req_.limit : 0));
        B().set_has_more(has_more_);
    }

    void count_total()
    {
        if (!table_open_ || counting_) return;
        counting_ = true;
        update_pager();
        uint64_t fg = fetch_gen_;
        auto tok = std::make_shared<CancelToken>();
        count_cancel_ = tok;
        auto c = conn_;
        TableMeta meta = meta_;
        FilterGroup f = req_.filter;
        spawn([this, c, meta = std::move(meta), f = std::move(f), tok, fg] {
            try {
                uint64_t n = c->count(meta, f, tok);
                post([this, n, fg] {
                    counting_ = false;
                    if (fg == fetch_gen_) { total_mode_ = TotalMode::Exact; total_ = n; }
                    update_pager();
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                post([this, m] {
                    counting_ = false;
                    update_pager();
                    set_status(trf("status.query-failed", { m }));
                });
            }
        });
    }

    // ------------------------------------------------------------------ columns / rows models
    static const char *type_class_label(const ColumnInfo &c) { return c.native_type.empty() ? "" : c.native_type.c_str(); }

    std::vector<std::string> ops_labels_cache_;

    std::vector<Op> col_ops(size_t ci) const { return ops_for(meta_.columns[ci], caps_); }

    ColumnUi make_col_ui(size_t ci)
    {
        const ColumnInfo &col = meta_.columns[ci];
        ColumnUi u {};
        u.name = S(col.name);
        u.type_label = S(type_class_label(col));
        ensure_view();
        u.hidden = view_.hidden[ci];
        u.x = (float)col_x(ci);
        u.width = u.hidden ? 0.0f : (float)widths_[ci];
        u.pk = col.is_pk();
        u.sort = 0;
        u.sort_order = 0;
        for (size_t k = 0; k < req_.sort.size(); ++k)
            if (req_.sort[k].column == col.name) {
                u.sort = req_.sort[k].desc ? 2 : 1;
                u.sort_order = req_.sort.size() > 1 ? (int)k + 1 : 0;
            }
        u.align_right = is_numeric(col.cls);
        u.filterable = col.cls != TypeClass::Binary;
        const ColFilter &f = filters_[ci];
        auto ops = col_ops(ci);
        u.filter_text = S(f.text);
        u.filter_op = S(ops.empty() ? "" : op_symbol(ops[std::clamp(f.op, 0, (int)ops.size() - 1)]));
        u.filter_state = !f.err.empty() ? 2 : f.active ? 1 : 0;
        u.filter_tip = S(f.err);
        return u;
    }

    // ---- copy as CSV/JSON/INSERT and paste from a spreadsheet
    Dialect copy_dialect() const
    {
        const EngineDef *e = find_engine(profile_.engine);
        std::string d = e ? e->driver : "";
        if (d == "postgres") return Dialect::Postgres;
        if (d == "mysql") return Dialect::MySql;
        if (d == "mssql") return Dialect::MsSql;
        if (d == "duckdb") return Dialect::DuckDb;
        return Dialect::Sqlite;
    }

    std::vector<size_t> shown_columns()
    {
        ensure_view();
        std::vector<size_t> v;
        for (size_t k : view_.order) if (!view_.hidden[k]) v.push_back(k);
        return v;
    }

    void copy_as(int scope)
    {
        int fmt = B().get_copyas_format();
        if (!table_open_) return;
        if (fmt == 3 && profile_.engine == "mongodb") { set_status(tr("copyas.no-insert")); return; }
        auto cols_idx = shown_columns();
        std::vector<ColumnInfo> cols;
        for (size_t k : cols_idx) cols.push_back(meta_.columns[k]);
        std::vector<std::vector<Value>> rows;
        int n = (int)visible_rows();
        for (int r = 0; r < n; ++r) {
            if (scope == 0 && r != B().get_sel_row()) continue;
            if (changes_.is_deleted(r)) continue;
            std::vector<Value> row;
            for (size_t k : cols_idx) row.push_back(changes_.current(rows_, r, k));
            rows.push_back(std::move(row));
        }
        try {
            CopyFormat f = fmt == 0 ? CopyFormat::Tsv : fmt == 1 ? CopyFormat::Csv : fmt == 2 ? CopyFormat::Json : CopyFormat::SqlInsert;
            CopyResult res = format_rows(cols, rows, f, meta_.table.name, copy_dialect(), true);
            if (!platform::clipboard_set(res.text)) { set_status(tr("copyas.failed")); return; }
            B().set_copyas_open(false);
            set_status(res.truncated ? trf("copyas.truncated", { std::to_string(res.rows) }) : trf("copyas.done", { std::to_string(res.rows) }));
        } catch (const std::exception &e) {
            set_status(trf("copyas.error", { e.what() }));
        }
    }

    std::string paste_msg(const std::string &m) { return m.rfind("paste.", 0) == 0 ? tr(m.c_str()) : m; }

    void paste_request()
    {
        if (!table_open_ || !editable_) return;
        auto text = platform::clipboard_get();
        if (!text || text->empty()) { set_status(tr("paste.empty")); return; }
        std::vector<std::vector<std::string>> cells;
        std::string err;
        if (!parse_tsv(*text, cells, &err)) { set_status(paste_msg(err)); return; }
        if (cells.empty()) { set_status(tr("paste.empty")); return; }
        auto shown = shown_columns();
        size_t start = 0;
        int sc = B().get_sel_col();
        for (size_t p = 0; p < shown.size(); ++p) if ((int)shown[p] == sc) start = p;
        std::vector<size_t> targets(shown.begin() + (long)start, shown.end());
        PastePlan plan = plan_paste(meta_.columns, targets, cells);
        std::string s;
        if (!plan.ok()) {
            s = trf("paste.invalid", { std::to_string(plan.issues.size()) });
            size_t shown_n = 0;
            for (auto &i : plan.issues) {
                if (shown_n++ >= 5) break;
                std::string col = i.col < targets.size() ? meta_.columns[targets[i.col]].name : std::string("?");
                s += "\n" + trf("paste.issue", { std::to_string(i.row + 1), col, paste_msg(i.message) });
            }
            paste_targets_.clear();
            paste_rows_.clear();
            B().set_paste_summary(S(s));
            B().set_paste_open(true);
            B().set_paste_can_confirm(false);
            return;
        }
        std::string names;
        for (size_t k = 0; k < targets.size() && k < cells[0].size(); ++k) names += (k ? ", " : "") + meta_.columns[targets[k]].name;
        paste_targets_ = targets;
        paste_rows_ = std::move(plan.values);
        B().set_paste_summary(S(trf("paste.ready", { std::to_string(paste_rows_.size()), names })));
        B().set_paste_can_confirm(true);
        B().set_paste_open(true);
    }

    void paste_confirm()
    {
        if (paste_rows_.empty() || !editable_) { B().set_paste_open(false); return; }
        for (auto &vals : paste_rows_) {
            std::vector<Value> row = ChangeSet::blank_row(meta_);
            for (size_t k = 0; k < vals.size() && k < paste_targets_.size(); ++k) row[paste_targets_[k]] = vals[k];
            changes_.add_insert(std::move(row));
        }
        size_t n = paste_rows_.size();
        paste_rows_.clear();
        B().set_paste_open(false);
        refresh_rows();
        update_pending();
        scroll_to_row((int)visible_rows() - 1);
        set_status(trf("paste.done", { std::to_string(n) }));
    }

    void wire_clipboard(const Backend &b)
    {
        b.on_copyas_do([this](int scope) { copy_as(scope); });
        b.on_paste_request([this] { paste_request(); });
        b.on_paste_confirm([this] { paste_confirm(); });
    }
    // ---- column layout (hide / reorder), stored by column name per connection and table
    std::vector<std::string> column_names() const
    {
        std::vector<std::string> n;
        for (auto &c : meta_.columns) n.push_back(c.name);
        return n;
    }

    std::string layout_key() const
    {
        return ColumnLayouts::make_key(profile_.id, meta_.table.database, meta_.table.schema, meta_.table.name);
    }

    void apply_layout() { view_ = resolve_layout(layouts_.get(layout_key()), column_names()); }

    void ensure_view()
    {
        if (view_.order.size() != meta_.columns.size() || view_.hidden.size() != meta_.columns.size()) apply_layout();
    }

    double col_x(size_t ci)
    {
        ensure_view();
        double x = 0;
        for (size_t p = 0; p < view_.order.size(); ++p) {
            size_t k = view_.order[p];
            if (k == ci) break;
            if (!view_.hidden[k]) x += widths_[k];
        }
        return x;
    }

    void rebuild_colcfg()
    {
        ensure_view();
        std::vector<ColumnCfgUi> rows;
        for (size_t p = 0; p < view_.order.size(); ++p) {
            ColumnCfgUi u {};
            size_t k = view_.order[p];
            u.name = S(meta_.columns[k].name);
            u.shown = !view_.hidden[k];
            u.first = p == 0;
            u.last = p + 1 == view_.order.size();
            rows.push_back(u);
        }
        colcfg_model_->set_vector(rows);
    }

    void store_view()
    {
        layouts_.set(layout_key(), layout_from_view(view_, column_names()));
        layouts_.save();
        refresh_columns();
        rebuild_colcfg();
    }

    void wire_columns(const Backend &b)
    {
        b.on_colcfg_open_clicked([this] {
            if (!table_open_) return;
            rebuild_colcfg();
            B().set_colcfg_open(true);
        });
        b.on_colcfg_toggle([this](int row) {
            ensure_view();
            if (row < 0 || (size_t)row >= view_.order.size()) return;
            size_t k = view_.order[(size_t)row];
            if (!view_.hidden[k]) {
                size_t shown = 0;
                for (bool h : view_.hidden) shown += h ? 0 : 1;
                if (shown <= 1) { set_status(tr("cols.keep-one")); rebuild_colcfg(); return; }
                if (k < filters_.size() && (filters_[k].active || !filters_[k].text.empty())) { set_status(tr("cols.has-filter")); rebuild_colcfg(); return; }
            }
            view_.hidden[k] = !view_.hidden[k];
            store_view();
        });
        b.on_colcfg_move([this](int row, int dir) {
            ensure_view();
            int to = row + dir;
            if (row < 0 || to < 0 || (size_t)row >= view_.order.size() || (size_t)to >= view_.order.size()) return;
            std::swap(view_.order[(size_t)row], view_.order[(size_t)to]);
            store_view();
        });
        b.on_colcfg_reset([this] {
            layouts_.set(layout_key(), {});
            layouts_.save();
            apply_layout();
            refresh_columns();
            rebuild_colcfg();
        });
    }
    void refresh_columns()
    {
        std::vector<ColumnUi> v;
        double total = 0;
        for (size_t ci = 0; ci < meta_.columns.size(); ++ci) {
            v.push_back(make_col_ui(ci));
            if (!view_.hidden[ci]) total += widths_[ci];
        }
        columns_model_->set_vector(v);
        B().set_content_width((float)total);
    }

    void refresh_col(size_t ci)
    {
        if (ci < meta_.columns.size() && (int)ci < (int)columns_model_->row_count()) columns_model_->set_row_data((int)ci, make_col_ui(ci));
    }

    void resize_column(std::vector<double> &widths, int ci, float w, bool sql)
    {
        if (ci < 0 || ci >= (int)widths.size()) return;
        widths[ci] = std::max(48.0f, w);
        if (!sql) { refresh_columns(); return; }
        auto &model = sql_columns_model_;
        double x = 0;
        for (int k = 0; k < (int)widths.size() && k < (int)model->row_count(); ++k) {
            ColumnUi u = *model->row_data(k);
            u.x = (float)x;
            u.width = (float)widths[k];
            model->set_row_data(k, u);
            x += widths[k];
        }
        if (sql) B().set_sql_content_width((float)x);
        else B().set_content_width((float)x);
    }

    static CellUi make_cell(const Value &v, bool edited)
    {
        CellUi c {};
        c.edited = edited;
        switch (v.kind) {
        case Value::Kind::Null: c.text = S("NULL"); c.kind = 1; break;
        case Value::Kind::Default: c.text = S("DEFAULT"); c.kind = 2; break;
        case Value::Kind::Missing: c.text = S("Ã¢â‚¬â€"); c.kind = 3; break;
        case Value::Kind::Bytes: c.text = S(cell_text(v.display())); c.kind = 4; break;
        default: c.text = S(cell_text(v.display())); c.kind = 0; break;
        }
        return c;
    }

    RowUi make_row(size_t r)
    {
        RowUi row {};
        auto cells = std::make_shared<slint::VectorModel<CellUi>>();
        size_t nc = meta_.columns.size();
        for (size_t c = 0; c < nc; ++c) cells->push_back(make_cell(changes_.current(rows_, r, c), changes_.is_edited(r, c)));
        row.cells = cells;
        bool fresh = changes_.is_new(rows_, r);
        row.state = fresh ? 1 : changes_.is_deleted(r) ? 2 : 0;
        row.label = S(fresh ? "" : std::to_string(req_.offset + r + 1));
        return row;
    }

    void refresh_rows()
    {
        std::vector<RowUi> v;
        size_t n = changes_.row_count(rows_);
        v.reserve(n);
        for (size_t r = 0; r < n; ++r) v.push_back(make_row(r));
        rows_model_->set_vector(v);
    }

    void refresh_row(size_t r)
    {
        if ((int)r < (int)rows_model_->row_count()) rows_model_->set_row_data((int)r, make_row(r));
    }

    void update_pending()
    {
        size_t aff = changes_.affected_rows();
        B().set_pending_count((int)aff);
        size_t ins = changes_.insert_count(), del = changes_.delete_count();
        size_t edited = aff > ins + del ? aff - ins - del : 0;
        B().set_pending_text(S(trf("pending.summary", { std::to_string(aff), std::to_string(edited), std::to_string(ins), std::to_string(del) })));
        B().set_can_undo(changes_.can_undo());
        B().set_can_redo(changes_.can_redo());
        update_tabs_ui();
    }

    // ------------------------------------------------------------------ sorting and filtering
    void sort_by(int ci, bool shift)
    {
        if (!table_open_ || ci < 0 || ci >= (int)meta_.columns.size()) return;
        const std::string &name = meta_.columns[ci].name;
        auto it = std::find_if(req_.sort.begin(), req_.sort.end(), [&](const SortKey &k) { return k.column == name; });
        if (shift) {
            if (it == req_.sort.end()) req_.sort.push_back({ name, false });
            else if (!it->desc) it->desc = true;
            else req_.sort.erase(it);
        } else {
            bool had = it != req_.sort.end();
            bool desc = had && it->desc;
            bool asc = had && !it->desc;
            req_.sort.clear();
            if (!had) req_.sort.push_back({ name, false });
            else if (asc) req_.sort.push_back({ name, true });
            else if (desc) {}
        }
        req_.offset = 0;
        refresh_columns();
        load_page(false);
    }

    void filter_edited(int ci, const std::string &text)
    {
        if (!table_open_ || ci < 0 || ci >= (int)filters_.size()) return;
        if (!changes_.empty()) {
            set_status(tr("unsaved.title") + ": " + tr("save") + " / " + tr("discard"));
            refresh_col(ci);
            return;
        }
        filters_[ci].text = text;
        evaluate_filters();
        filter_timer_.start(slint::TimerMode::SingleShot, std::chrono::milliseconds(400), [this] { apply_filters(false); });
    }

    // Parses every filter box; returns the combined group, or nullopt when any box is invalid.
    bool evaluate_filters()
    {
        FilterGroup g;
        std::string err_msg;
        bool ok = true;
        std::string sig;
        for (size_t ci = 0; ci < filters_.size(); ++ci) {
            ColFilter &f = filters_[ci];
            f.err.clear();
            f.active = false;
            auto ops = col_ops(ci);
            if (!ops.empty()) {
                Op op = ops[std::clamp(f.op, 0, (int)ops.size() - 1)];
                bool needs_value = op_arity(op) != 0;
                if (needs_value || f.explicit_op) {
                    std::string err;
                    auto cond = make_condition(meta_.columns[ci], op, f.text, &err);
                    if (cond) {
                        g.conds.push_back(*cond);
                        f.active = true;
                        sig += meta_.columns[ci].name + "\x1f" + std::to_string((int)op) + "\x1f" + f.text + "\x1e";
                    } else if (!err.empty()) {
                        f.err = err;
                        ok = false;
                        if (err_msg.empty()) err_msg = trf("filter.invalid", { meta_.columns[ci].name, err });
                    }
                }
            }
            refresh_col(ci);
        }
        B().set_filter_error(S(err_msg));
        B().set_filter_count(g.count());
        pending_group_ = g;
        pending_sig_ = sig;
        return ok;
    }

    void apply_filters(bool force)
    {
        if (!table_open_) return;
        filter_timer_.stop();
        if (!evaluate_filters()) return;
        if (!force && pending_sig_ == applied_sig_) return;
        if (pending_sig_ == applied_sig_ && force) { /* Enter on unchanged text reloads */ }
        applied_sig_ = pending_sig_;
        req_.filter = pending_group_;
        applied_filter_count_ = pending_group_.count();
        req_.offset = 0;
        load_page(true);
    }

    void clear_filters()
    {
        if (!table_open_) return;
        if (!changes_.empty()) { set_status(tr("unsaved.title")); return; }
        for (auto &f : filters_) f = ColFilter();
        apply_filters(true);
    }

    void prepare_op_menu(int ci)
    {
        std::vector<slint::SharedString> labels;
        menu_ops_.clear();
        menu_col_ = ci;
        if (!table_open_ || ci < 0 || ci >= (int)meta_.columns.size()) { op_menu_model_->set_vector(labels); return; }
        labels.push_back(S("Ã¢â‚¬â€  (" + tr("discard") + ")"));
        menu_ops_ = col_ops(ci);
        for (Op o : menu_ops_) labels.push_back(S(std::string(op_symbol(o)) + "   " + op_label(o)));
        op_menu_model_->set_vector(labels);
    }

    void op_picked(int ci, int idx)
    {
        if (!table_open_ || ci != menu_col_ || ci < 0 || ci >= (int)filters_.size()) return;
        if (!changes_.empty()) { set_status(tr("unsaved.title")); return; }
        ColFilter &f = filters_[ci];
        if (idx <= 0) {
            f = ColFilter();
        } else if (idx - 1 < (int)menu_ops_.size()) {
            auto ops = col_ops(ci);
            Op chosen = menu_ops_[idx - 1];
            for (size_t k = 0; k < ops.size(); ++k)
                if (ops[k] == chosen) f.op = (int)k;
            f.explicit_op = true;
            if (op_arity(chosen) == 0) f.text.clear();
        }
        apply_filters(true);
    }

    // ------------------------------------------------------------------ cell editing
    size_t visible_rows() const { return changes_.row_count(rows_); }

    void cell_clicked(int r, int c)
    {
        if (B().get_edit_row() >= 0) commit_edit(U(B().get_edit_text()), 0);
        B().set_sel_row(r);
        B().set_sel_col(c);
    }

    void move_selection(int dr, int dc)
    {
        int rows = (int)visible_rows(), cols = (int)meta_.columns.size();
        if (rows == 0 || cols == 0) return;
        int r = std::clamp(B().get_sel_row() + dr, 0, rows - 1);
        ensure_view();
        int c = B().get_sel_col();
        if (c < 0 || c >= cols || view_.hidden[(size_t)c]) {
            c = -1;
            for (size_t p = 0; p < view_.order.size() && c < 0; ++p) if (!view_.hidden[view_.order[p]]) c = (int)view_.order[p];
        } else if (dc != 0) {
            int pos = 0;
            for (size_t p = 0; p < view_.order.size(); ++p) if ((int)view_.order[p] == c) pos = (int)p;
            int step = dc > 0 ? 1 : -1;
            for (int q = pos + step; q >= 0 && q < cols; q += step)
                if (!view_.hidden[view_.order[(size_t)q]]) { c = (int)view_.order[(size_t)q]; break; }
        }
        if (B().get_sel_row() < 0) r = 0;
        B().set_sel_row(r);
        B().set_sel_col(c);
        scroll_to_row(r);
    }

    void begin_edit(int r, int c, const std::string &initial)
    {
        if (!table_open_ || r < 0 || c < 0 || r >= (int)visible_rows() || c >= (int)meta_.columns.size()) return;
        B().set_sel_row(r);
        B().set_sel_col(c);
        if (!editable_) {
            std::string why = B().get_blocked_reason().size() ? U(B().get_blocked_reason()) : tr("edit.not-editable");
            set_status(why);
            return;
        }
        if (meta_.columns[c].read_only()) { set_status(tr("edit.readonly-col")); return; }
        if (changes_.is_deleted(r)) return;
        B().set_edit_text(S(initial.empty() ? changes_.current(rows_, r, c).edit_text() : initial));
        B().set_edit_row(r);
        B().set_edit_col(c);
    }

    // Converts typed text into a Value for the column. Returns false and sets *err when invalid.
    bool parse_cell(const ColumnInfo &col, const std::string &text, Value &out, std::string *err)
    {
        if (text.empty() && !is_textual_class(col.cls)) {
            if (!col.nullable && !col.has_default && !col.identity) { *err = col.name + " cannot be empty"; return false; }
            out = col.nullable ? Value::null() : Value::dflt();
            return true;
        }
        if (is_textual_class(col.cls)) { out = Value::text(text); return true; }
        auto v = parse_typed(col.cls, text, err);
        if (!v) return false;
        out = *v;
        return true;
    }

    void commit_edit(const std::string &text, int move)
    {
        int r = B().get_edit_row(), c = B().get_edit_col();
        if (r < 0 || c < 0) return;
        B().set_edit_row(-1);
        B().set_edit_col(-1);
        if (r >= (int)visible_rows()) return;
        Value v;
        std::string err;
        if (!parse_cell(meta_.columns[c], text, v, &err)) {
            set_status(trf("edit.bad-value", { err }));
            return;
        }
        changes_.set_cell(rows_, r, c, v);
        refresh_row(r);
        update_pending();
        if (move == 1) move_selection(1, 0);
        else if (move == 2) move_selection(0, 1);
    }

    void set_null()
    {
        int r = B().get_sel_row(), c = B().get_sel_col();
        if (!editable_ || r < 0 || c < 0 || r >= (int)visible_rows()) return;
        const ColumnInfo &col = meta_.columns[c];
        if (col.read_only()) { set_status(tr("edit.readonly-col")); return; }
        if (!col.nullable) { set_status(trf("edit.bad-value", { col.name + " is NOT NULL" })); return; }
        changes_.set_cell(rows_, r, c, Value::null());
        refresh_row(r);
        update_pending();
    }

    void revert_cell()
    {
        int r = B().get_sel_row(), c = B().get_sel_col();
        if (r < 0 || c < 0 || r >= (int)visible_rows()) return;
        changes_.restore_cell(rows_, r, c);
        refresh_row(r);
        update_pending();
    }

    void add_row(bool duplicate)
    {
        if (!editable_) return;
        int r = B().get_sel_row();
        if (duplicate) {
            if (r < 0 || r >= (int)visible_rows()) return;
            changes_.add_insert(changes_.duplicate_of(meta_, rows_, r));
        } else {
            changes_.add_insert(ChangeSet::blank_row(meta_));
        }
        refresh_rows();
        update_pending();
        int last = (int)visible_rows() - 1;
        B().set_sel_row(last);
        if (B().get_sel_col() < 0) B().set_sel_col(0);
        scroll_to_row(last);
    }

    void delete_row()
    {
        int r = B().get_sel_row();
        if (!editable_ || r < 0 || r >= (int)visible_rows()) return;
        if (changes_.is_deleted(r)) changes_.unmark_delete(r);
        else changes_.mark_delete(rows_, r);
        refresh_rows();
        update_pending();
        int n = (int)visible_rows();
        if (B().get_sel_row() >= n) B().set_sel_row(n - 1);
    }

    void copy_cell()
    {
        int r = B().get_sel_row(), c = B().get_sel_col();
        if (r < 0 || c < 0 || r >= (int)visible_rows() || c >= (int)meta_.columns.size()) return;
        const Value &v = changes_.current(rows_, r, c);
        platform::clipboard_set(v.is_null() ? std::string() : v.display());
        set_status(tr("status.copied"));
    }

    void copy_row()
    {
        int r = B().get_sel_row();
        if (r < 0 || r >= (int)visible_rows()) return;
        std::string s;
        for (size_t c = 0; c < meta_.columns.size(); ++c) {
            if (c) s += '\t';
            const Value &v = changes_.current(rows_, r, c);
            s += v.is_null() ? std::string() : v.display();
        }
        platform::clipboard_set(s);
        set_status(tr("status.copied"));
    }

    void open_detail()
    {
        int r = B().get_sel_row(), c = B().get_sel_col();
        if (r < 0 || c < 0 || r >= (int)visible_rows() || c >= (int)meta_.columns.size()) return;
        const Value &v = changes_.current(rows_, r, c);
        B().set_detail_title(S(trf("detail.title", { meta_.columns[c].name, std::to_string(req_.offset + r + 1) })));
        B().set_detail_body(S(v.display()));
        B().set_detail_open(true);
    }

    // ------------------------------------------------------------------ saving
    void start_save(std::function<void()> then)
    {
        if (!table_open_ || changes_.empty() || saving_) { if (then) then(); return; }
        std::vector<Change> list;
        try {
            list = changes_.build(meta_, rows_);
        } catch (const std::exception &e) {
            notify(tr("save.confirm-title"), trf("save.failed", { e.what() }));
            return;
        }
        std::string detail;
        try {
            detail = tr("save.preview") + "\n" + conn_->preview_changes(meta_, list);
        } catch (...) {
        }
        std::string count = std::to_string(list.size());
        std::string text = trf(is_prod() ? "save.confirm-prod" : "save.confirm-text", { count, meta_.table.label() });
        size_t dels = changes_.delete_count();
        if (dels) text += "\n" + trf("save.delete-warn", { std::to_string(dels) });
        confirm(tr("save.confirm-title"), text, detail, tr("save.apply"), "", dels > 0 || is_prod(), is_prod(), [this, list = std::move(list), then](int r) mutable {
            if (r != 1) return;
            run_save(std::move(list), then);
        });
    }

    void run_save(std::vector<Change> list, std::function<void()> then)
    {
        saving_ = true;
        set_busy();
        auto c = conn_;
        TableMeta meta = meta_;
        uint64_t cg = conn_gen_;
        spawn([this, c, meta = std::move(meta), list = std::move(list), then, cg] {
            try {
                ApplyReport rep = c->apply_changes(meta, list);
                post([this, rep, then, cg] {
                    saving_ = false;
                    set_busy();
                    if (cg != conn_gen_) return;
                    changes_.clear();
                    set_status(trf("save.done", { std::to_string(rep.inserted), std::to_string(rep.updated), std::to_string(rep.deleted) }));
                    if (then) then();
                    else load_page(true);
                });
            } catch (const DbError &e) {
                std::string m = e.what();
                bool conflict = e.kind == ErrKind::Conflict;
                bool lost = is_connection_lost(e.kind);
                post([this, m, conflict, lost, c] {
                    saving_ = false;
                    set_busy();
                    std::string msg = conflict ? tr("save.conflict") : trf("save.failed", { m });
                    set_status(msg);
                    notify(tr("save.confirm-title"), msg);
                    if (lost) on_connection_lost(c); // changes stay pending; the save is never replayed automatically
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                post([this, m] {
                    saving_ = false;
                    set_busy();
                    set_status(trf("save.failed", { m }));
                    notify(tr("save.confirm-title"), trf("save.failed", { m }));
                });
            }
        });
    }

    // ------------------------------------------------------------------ export
    void open_export(bool from_sql)
    {
        if (from_sql ? sql_out_.rows.empty() : !table_open_) return;
        export_from_sql_ = from_sql;
        B().set_exp_summary(S(from_sql ? tr("export.summary-sql") : trf("export.summary", { meta_.table.label() })));
        B().set_exp_progress(S(""));
        B().set_exp_running(false);
        B().set_exp_open(true);
    }

    void start_export()
    {
        if (exporting_) return;
        static const ExportFormat fmts[] = { ExportFormat::Csv, ExportFormat::Tsv, ExportFormat::Json, ExportFormat::Ndjson, ExportFormat::SqlInsert };
        ExportOptions opt;
        opt.format = fmts[std::clamp(B().get_exp_format(), 0, 4)];
        opt.header = B().get_exp_header();
        opt.delimiter = opt.format == ExportFormat::Tsv ? '\t' : ',';
        std::string base = export_from_sql_ ? "result" : meta_.table.name;
        opt.table_name = base;
        std::string ext = export_extension(opt.format);
        std::string label = ext.substr(1);
        for (auto &ch : label) ch = (char)std::toupper((unsigned char)ch);
        std::optional<std::string> path;
        if (const char *forced = std::getenv("DBPRO_SELFTEST_EXPORT")) path = std::string(forced) + ext; // self-test: no native dialog
        else path = platform::save_dialog(tr("export.title"), base + ext, { { label, "*" + ext } });
        if (!path) return;

        exporting_ = true;
        exp_cancel_ = std::make_shared<CancelToken>();
        auto tok = exp_cancel_;
        B().set_exp_running(true);
        B().set_exp_progress(S(trf("export.progress", { "0" })));
        set_busy();
        auto c = conn_;
        TableMeta meta = meta_;
        PageRequest req = req_;
        bool from_sql = export_from_sql_;
        QueryOutput out = from_sql ? sql_out_ : QueryOutput();
        std::string file = *path;
        spawn([this, c, meta = std::move(meta), req = std::move(req), tok, opt, file, from_sql, out = std::move(out)]() mutable {
            uint64_t written = 0;
            bool cancelled = false;
            std::string error;
            try {
                std::vector<ColumnInfo> cols;
                if (from_sql) {
                    for (auto &n : out.columns) { ColumnInfo ci; ci.name = n; cols.push_back(ci); }
                } else {
                    cols = meta.columns;
                }
                auto writer = make_writer(std::filesystem::u8path(file), opt);
                writer->begin(cols);
                auto last = Clock::now();
                auto report = [&] {
                    auto now = Clock::now();
                    if (now - last > std::chrono::milliseconds(200)) {
                        last = now;
                        uint64_t w = written;
                        post([this, w] { B().set_exp_progress(S(trf("export.progress", { i18n::with_commas(w) }))); });
                    }
                };
                if (from_sql) {
                    const size_t step = 2000;
                    for (size_t k = 0; k < out.rows.size() && !tok->cancelled(); k += step) {
                        size_t e = std::min(out.rows.size(), k + step);
                        std::vector<std::vector<Value>> batch(out.rows.begin() + k, out.rows.begin() + e);
                        writer->rows(batch);
                        written += batch.size();
                        report();
                    }
                } else {
                    c->stream_rows(meta, req, 5000, [&](std::vector<std::vector<Value>> &batch) {
                        writer->rows(batch);
                        written += batch.size();
                        report();
                        return !tok->cancelled();
                    }, tok);
                }
                writer->end();
                cancelled = tok->cancelled();
            } catch (const DbError &e) {
                if (e.kind == ErrKind::Cancelled) cancelled = true;
                else error = e.what();
            } catch (const std::exception &e) {
                error = e.what();
            }
            post([this, written, cancelled, error, file] {
                exporting_ = false;
                set_busy();
                B().set_exp_running(false);
                std::string msg = !error.empty() ? trf("export.failed", { error })
                                  : cancelled   ? tr("export.cancelled")
                                                : trf("export.done", { i18n::with_commas(written), file });
                B().set_exp_progress(S(msg));
                set_status(msg);
                if (error.empty() && !cancelled) {
                    try { store_.settings.last_export_dir = platform::narrow(std::filesystem::u8path(file).parent_path().wstring()); } catch (...) {}
                    if (!std::getenv("DBPRO_SELFTEST")) platform::reveal_in_explorer(file);
                }
            });
        });
    }

    // ------------------------------------------------------------------ settings and updates
    void set_dark(bool dark)
    {
        store_.settings.dark = dark;
        ui_->global<Theme>().set_dark(dark);
        B().set_theme_index(dark ? 0 : 1);
        store_.save();
    }

    void set_language(const std::string &lang)
    {
        store_.settings.lang = lang;
        i18n::set_language(lang);
        B().set_lang_rev(B().get_lang_rev() + 1);
        B().set_lang_index(lang == "es" ? 0 : 1);
        refresh_engines();
        update_pending();
        update_pager();
        update_blocked();
        refresh_update_text();
        if (mon_have_) render_activity();
        if (!connected_) set_status(tr("ready"));
        store_.save();
    }

    int monitor_interval_index() const
    {
        for (int k = 0; k < 5; ++k)
            if (kMonitorIntervals[k] == store_.settings.monitor_interval) return k;
        return 2;
    }

    int query_timeout_index() const
    {
        for (int k = 0; k < 5; ++k)
            if (kQueryTimeouts[k] == store_.settings.query_timeout) return k;
        return 0;
    }

    void open_settings()
    {
        B().set_query_timeout_index(query_timeout_index());
        B().set_app_version(S(brand::kVersion));
        B().set_app_copyright(S(brand::kCopyright));
        B().set_theme_index(store_.settings.dark ? 0 : 1);
        B().set_lang_index(store_.settings.lang == "es" ? 0 : 1);
        B().set_auto_update(store_.settings.auto_update);
        B().set_monitor_interval_index(monitor_interval_index());
        B().set_settings_page_size_index(page_size_index());
        B().set_settings_open(true);
    }

    void set_upd(int state, const std::string &key, std::vector<std::string> args = {})
    {
        upd_state_ = state;
        upd_key_ = key;
        upd_args_ = std::move(args);
        B().set_upd_state(state);
        refresh_update_text();
    }

    void refresh_update_text()
    {
        if (upd_key_.empty()) { B().set_upd_text(S("")); return; }
        std::string t = tr(upd_key_);
        for (size_t i = 0; i < upd_args_.size(); ++i) {
            std::string tag = "{" + std::to_string(i) + "}";
            for (size_t p = t.find(tag); p != std::string::npos; p = t.find(tag, p + upd_args_[i].size())) t.replace(p, tag.size(), upd_args_[i]);
        }
        B().set_upd_text(S(t));
    }

    // `silent` = the automatic startup check: it only lights the badge on the gear, it never shows errors.
    void check_updates(bool silent)
    {
        if (upd_state_ == 1 || upd_state_ == 4 || upd_state_ == 5) return;
        if (!silent) set_upd(1, "upd.checking");
        store_.settings.last_update_check = (long long)std::time(nullptr);
        store_.save();
        spawn([this, silent] {
            updater::Release r = updater::fetch_latest();
            post([this, silent, r = std::move(r)]() mutable {
                if (!r.ok) {
                    if (!silent) set_upd(6, "upd.check-failed", { r.error });
                    return;
                }
                if (updater::is_newer(r.version, brand::kVersion)) {
                    release_ = std::move(r);
                    B().set_update_badge(true);
                    B().set_upd_notes(S(release_.notes));
                    set_upd(3, "upd.available", { release_.version });
                } else if (!silent) {
                    set_upd(2, "upd.up-to-date", { brand::kVersion });
                }
            });
        });
    }

    void start_update()
    {
        if (upd_state_ != 3 || release_.installer_url.empty()) return;
        guard_all([this] { download_update(); });
    }

    void download_update()
    {
        updater::Release r = release_;
        set_upd(4, "upd.downloading", { r.version, "0 %" });
        B().set_upd_progress(0);
        spawn([this, r] {
            std::string dir = updater::download_dir();
            std::string file = dir + "\\" + std::filesystem::path(r.installer_name).filename().string();
            auto last = Clock::now() - std::chrono::seconds(1);
            std::string err = updater::download(r.installer_url, file, [&](int64_t done, int64_t total) {
                auto now = Clock::now();
                if (now - last < std::chrono::milliseconds(150)) return;
                last = now;
                float frac = total > 0 ? (float)((double)done / (double)total) : 0.0f;
                std::string pct = total > 0 ? std::to_string((int)(frac * 100)) + " %" : human_bytes((double)done);
                post([this, frac, pct, v = r.version] {
                    if (upd_state_ != 4) return;
                    B().set_upd_progress(frac);
                    set_upd(4, "upd.downloading", { v, pct });
                });
            });
            bool bad_hash = false;
            if (err.empty()) {
                post([this] { set_upd(4, "upd.verifying"); });
                bad_hash = !updater::verify_file(file, r.sha256);
            }
            post([this, err, bad_hash, file] {
                if (!err.empty()) { set_upd(6, "upd.failed", { err }); return; }
                if (bad_hash) { set_upd(6, "upd.hash-mismatch"); return; }
                set_upd(5, "upd.starting");
                if (!updater::run_installer(file)) { set_upd(6, "upd.installer-failed"); return; }
                slint::Timer::single_shot(std::chrono::milliseconds(600), [] { slint::quit_event_loop(); });
            });
        });
    }

    void startup_update_check()
    {
        if (!store_.settings.auto_update || std::getenv("DBPRO_SELFTEST")) return;
        long long now = (long long)std::time(nullptr);
        if (now - store_.settings.last_update_check < 24 * 3600 && now >= store_.settings.last_update_check) return;
        check_updates(true);
    }

    // ------------------------------------------------------------------ live performance monitor
    bool monitor_active() const { return connected_ && B().get_view() == 2 && caps_.activity; }

    std::string monitor_error_text(const std::string &message) const
    {
        std::string lower = message;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        bool denied = lower.find("permission") != std::string::npos || lower.find("denied") != std::string::npos ||
                      lower.find("not authorized") != std::string::npos || lower.find("insufficient privilege") != std::string::npos ||
                      lower.find("not permitted") != std::string::npos;
        if (!denied) return trf("mon.failed", { message });
        std::string needed = tr("mon.permission-generic");
        if (profile_.engine == "mssql") needed = tr("mon.permission-mssql");
        else if (profile_.engine == "postgres") needed = tr("mon.permission-postgres");
        else if (profile_.engine == "mysql" || profile_.engine == "mariadb") needed = tr("mon.permission-mysql");
        else if (profile_.engine == "mongodb") needed = tr("mon.permission-mongodb");
        return trf("mon.permission", { needed, message });
    }

    void view_changed(int v)
    {
        if (v == 2) start_monitor();
        else stop_monitor();
    }

    void start_monitor()
    {
        stop_monitor();
        B().set_mon_supported(connected_ && caps_.activity);
        if (!monitor_active()) return;
        mon_timer_.start(slint::TimerMode::Repeated, std::chrono::seconds(store_.settings.monitor_interval), [this] { poll_monitor(false); });
        poll_monitor(true);
    }

    void stop_monitor()
    {
        mon_timer_.stop();
        ++mon_gen_;
        mon_busy_ = false;
        B().set_mon_loading(false);
    }

    void reset_monitor_ui()
    {
        mon_have_ = false;
        mon_history_.clear();
        mon_prev_.clear();
        mon_sel_id_.clear();
        mon_row_indices_.clear();
        mon_metrics_model_->set_vector({});
        mon_columns_model_->set_vector({});
        mon_rows_model_->set_vector({});
        B().set_mon_supported(false);
        B().set_mon_error(S(""));
        B().set_mon_updated(S(""));
        B().set_mon_summary(S(""));
        B().set_mon_query(S(""));
        B().set_mon_search(S(""));
        mon_search_.clear();
        mon_blocked_only_ = false;
        mon_sort_col_ = -1;
        mon_sort_ascending_ = true;
        B().set_mon_blocked_only(false);
        B().set_mon_sel(-1);
        B().set_mon_can_kill(false);
        B().set_mon_kill_supported(false);
        B().set_mon_content_width(0);
    }

    void poll_monitor(bool force)
    {
        if (!monitor_active() || mon_busy_ || (mon_paused_ && !force)) return;
        mon_busy_ = true;
        if (force) B().set_mon_loading(true);
        uint64_t gen = mon_gen_, cg = conn_gen_;
        auto c = conn_;
        spawn([this, c, gen, cg] {
            try {
                Activity a = c->activity();
                post([this, gen, cg, a = std::move(a)]() mutable {
                    if (gen != mon_gen_ || cg != conn_gen_) return;
                    mon_busy_ = false;
                    B().set_mon_loading(false);
                    B().set_mon_error(S(""));
                    mon_activity_ = std::move(a);
                    mon_have_ = true;
                    mon_time_ = Clock::now();
                    mon_updated_ = clock_text();
                    absorb_metrics();
                    render_activity();
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                auto *de = dynamic_cast<const DbError *>(&e);
                bool lost = de && is_connection_lost(de->kind);
                post([this, gen, cg, m, lost, c] {
                    if (gen != mon_gen_ || cg != conn_gen_) return;
                    mon_busy_ = false;
                    B().set_mon_loading(false);
                    B().set_mon_error(S(monitor_error_text(m)));
                    if (lost) on_connection_lost(c);
                });
            }
        });
    }

    // Counters become per-second rates between two samples; every metric feeds its sparkline.
    void absorb_metrics()
    {
        double dt = mon_prev_time_.time_since_epoch().count() ? std::chrono::duration<double>(mon_time_ - mon_prev_time_).count() : 0;
        mon_rates_.clear();
        for (const Metric &m : mon_activity_.metrics) {
            double shown = m.value;
            bool known = true;
            if (m.counter) {
                auto it = mon_prev_.find(m.key);
                known = it != mon_prev_.end() && dt > 0.2 && m.value >= it->second;
                shown = known ? (m.value - it->second) / dt : 0;
                mon_prev_[m.key] = m.value;
            }
            mon_rates_[m.key] = known ? shown : -1;
            if (known) {
                auto &h = mon_history_[m.key];
                h.push_back(shown);
                if (h.size() > kSparkPoints) h.pop_front();
            }
        }
        mon_prev_time_ = mon_time_;
    }

    double metric_value(const char *key) const
    {
        for (const Metric &m : mon_activity_.metrics)
            if (m.key == key) return m.value;
        return 0;
    }

    void render_activity()
    {
        const Activity &a = mon_activity_;
        std::vector<MetricUi> metrics;
        for (const Metric &m : a.metrics) {
            MetricUi u {};
            std::string label = tr("mon.m." + m.key);
            u.label = S(label == "mon.m." + m.key ? m.label : label);
            double r = mon_rates_.count(m.key) ? mon_rates_[m.key] : m.value;
            if (r < 0) {
                u.value = S("Ã¢â‚¬â€");
            } else if (m.unit == "B") {
                u.value = S(human_bytes(r) + (m.counter ? "/s" : ""));
            } else {
                u.value = S(plain_number(r, m.counter));
            }
            auto h = mon_history_.find(m.key);
            if (h != mon_history_.end()) u.spark = S(spark_path(h->second));
            u.tone = 0;
            if (m.key == "blocked" && m.value > 0) u.tone = 2;
            else if (m.key == "sessions" && metric_value("max_sessions") > 0 && m.value / metric_value("max_sessions") >= 0.85) u.tone = 1;
            else if ((m.key == "queued" || m.key == "row_lock_waits") && m.value > 0) u.tone = 1;
            metrics.push_back(u);
        }
        mon_metrics_model_->set_vector(metrics);

        static const std::map<std::string, double> weights = { { "id", 70 }, { "user", 110 }, { "database", 120 }, { "app", 120 }, { "client", 110 }, { "host", 120 },
                                                               { "state", 100 }, { "command", 90 }, { "wait", 150 }, { "seconds", 80 }, { "blocked_by", 100 }, { "query", 420 } };
        std::vector<ColumnUi> cols;
        double x = 0;
        for (size_t ci = 0; ci < a.columns.size(); ++ci) {
            const std::string &key = a.columns[ci];
            ColumnUi u {};
            std::string label = tr("mon.col." + key);
            u.name = S(label == "mon.col." + key ? key : label);
            auto w = weights.find(key);
            double width = w == weights.end() ? 120 : w->second;
            u.x = (float)x;
            u.width = (float)width;
            u.align_right = key == "seconds";
            u.sort = mon_sort_col_ == (int)ci ? (mon_sort_ascending_ ? 1 : 2) : 0;
            cols.push_back(u);
            x += width;
        }
        mon_columns_model_->set_vector(cols);
        B().set_mon_content_width((float)x);

        std::set<std::string> blockers;
        if (a.blocked_col >= 0) {
            for (auto &row : a.rows) {
                std::string list = row[a.blocked_col].display();
                size_t s = 0;
                while (s < list.size()) {
                    size_t e = list.find(',', s);
                    if (e == std::string::npos) e = list.size();
                    blockers.insert(list.substr(s, e - s));
                    s = e + 1;
                }
            }
        }
        ActivityViewOptions opts;
        opts.search = mon_search_;
        opts.blocked_only = mon_blocked_only_;
        opts.sort_column = mon_sort_col_;
        opts.ascending = mon_sort_ascending_;
        mon_row_indices_ = activity_visible_rows(a, opts);
        std::vector<RowUi> rows;
        int sel = -1, selected_original = -1;
        for (size_t vi = 0; vi < mon_row_indices_.size(); ++vi) {
            size_t r = mon_row_indices_[vi];
            RowUi row {};
            auto cells = std::make_shared<slint::VectorModel<CellUi>>();
            for (auto &v : a.rows[r]) {
                CellUi c {};
                c.text = S(cell_text(v.display(), 200));
                cells->push_back(c);
            }
            row.cells = cells;
            if (a.blocked_col >= 0 && a.blocked_col < (int)a.rows[r].size() &&
                a.rows[r][a.blocked_col].kind != Value::Kind::Null && a.rows[r][a.blocked_col].kind != Value::Kind::Missing &&
                !a.rows[r][a.blocked_col].display().empty()) row.state = 1;
            else if (a.id_col >= 0 && a.id_col < (int)a.rows[r].size() && blockers.count(a.rows[r][a.id_col].display())) row.state = 2;
            if (a.id_col >= 0 && a.id_col < (int)a.rows[r].size() && !mon_sel_id_.empty() && a.rows[r][a.id_col].display() == mon_sel_id_) {
                sel = (int)vi;
                selected_original = (int)r;
            }
            rows.push_back(row);
        }
        mon_rows_model_->set_vector(rows);
        if (sel < 0) mon_sel_id_.clear();
        B().set_mon_sel(sel);
        update_monitor_selection(selected_original);

        std::string upd = trf("mon.updated", { mon_updated_ });
        if (mon_paused_) upd += " Ã‚Â· " + tr("mon.paused");
        B().set_mon_updated(S(upd));
        B().set_mon_summary(S(a.columns.empty()
                                  ? tr("mon.local-summary")
                                  : trf("mon.summary", { plain_number(metric_value("sessions"), false), plain_number(metric_value("active"), false),
                                                         plain_number(metric_value("blocked"), false) })));
    }

    void update_monitor_selection(int sel)
    {
        const Activity &a = mon_activity_;
        std::string q;
        if (sel >= 0 && sel < (int)a.rows.size() && a.query_col >= 0 && a.query_col < (int)a.rows[sel].size()) q = a.rows[sel][a.query_col].display();
        B().set_mon_query(S(q));
        B().set_mon_kill_supported(caps_.kill_session && !profile_.read_only);
        B().set_mon_can_kill(sel >= 0 && caps_.kill_session && !profile_.read_only);
    }

    void monitor_select(int i)
    {
        const Activity &a = mon_activity_;
        if (i < 0 || i >= (int)mon_row_indices_.size() || a.id_col < 0) return;
        int original = (int)mon_row_indices_[i];
        if (original < 0 || original >= (int)a.rows.size() || a.id_col >= (int)a.rows[original].size()) return;
        mon_sel_id_ = a.rows[original][a.id_col].display();
        B().set_mon_sel(i);
        update_monitor_selection(original);
    }

    void monitor_kill()
    {
        if (mon_sel_id_.empty() || !caps_.kill_session || profile_.read_only || !connected_) return;
        std::string id = mon_sel_id_;
        confirm(trf("mon.kill-title", { id }), tr("mon.kill-text"), U(B().get_mon_query()).substr(0, 800), tr("mon.kill"), "", true, is_prod(), [this, id](int r) {
            if (r != 1 || !connected_) return;
            auto c = conn_;
            uint64_t cg = conn_gen_;
            spawn([this, c, id, cg] {
                try {
                    c->kill_session(id);
                    post([this, id, cg] {
                        if (cg != conn_gen_) return;
                        set_status(trf("mon.kill-done", { id }));
                        poll_monitor(true);
                    });
                } catch (const std::exception &e) {
                    std::string m = e.what();
                    post([this, cg, m] {
                        if (cg != conn_gen_) return;
                        notify(tr("mon.kill"), monitor_error_text(m));
                    });
                }
            });
        });
    }

    // ------------------------------------------------------------------ SQL editor
    void run_sql(const std::string &text)
    {
        if (sql_running_) return;
        if (!connected_) { B().set_sql_message(S(tr("sql.need-connection"))); B().set_sql_error(true); return; }
        bool blank = true;
        for (char ch : text) if (!std::isspace((unsigned char)ch)) blank = false;
        if (blank) { B().set_sql_message(S(tr("sql.empty"))); B().set_sql_error(true); return; }
        if (!statement_is_read_only(text)) {
            confirm(tr("sql.confirm-title"), is_prod() ? tr("sql.confirm-prod") : tr("sql.confirm-text"), text.substr(0, 1200), tr("sql.run-anyway"), "", true, is_prod(), [this, text](int r) {
                if (r == 1) execute_sql(text);
            });
            return;
        }
        execute_sql(text);
    }

    void execute_sql(const std::string &text)
    {
        sql_running_ = true;
        sql_last_text_ = text;
        sql_run_id_ = sql_tabs_[sql_active_].id;
        sql_cancel_ = std::make_shared<CancelToken>();
        auto tok = sql_cancel_;
        B().set_sql_running(true);
        B().set_sql_error(false);
        B().set_sql_message(S(tr("sql.running")));
        set_busy();
        auto c = conn_;
        uint64_t cg = conn_gen_;
        constexpr size_t kMaxRows = 10000;
        int limit = store_.settings.query_timeout;
        spawn([this, c, text, tok, cg, limit] {
            try {
                Deadline dl(tok, limit);
                QueryOutput out = c->execute(text, kMaxRows, tok);
                post([this, cg, out = std::move(out)]() mutable { on_sql_done(cg, std::move(out)); });
            } catch (const DbError &e) {
                std::string m = e.what();
                bool cancelled = e.kind == ErrKind::Cancelled;
                if (tok->timed_out()) m = trf("status.timeout", { std::to_string(limit) }), cancelled = false;
                bool lost = is_connection_lost(e.kind);
                post([this, m, cancelled, lost, c] {
                    sql_failed(cancelled ? tr("status.cancelled") : m);
                    if (lost) on_connection_lost(c);
                });
            } catch (const std::exception &e) {
                std::string m = e.what();
                post([this, m] { sql_failed(m); });
            }
        });
    }

    void sql_failed(const std::string &m)
    {
        sql_running_ = false;
        B().set_sql_running(false);
        set_busy();
        SqlTab *t = sql_tab_by_id(sql_run_id_);
        if (t && t->id != sql_tabs_[sql_active_].id) { t->msg = m; t->err = true; return; }
        B().set_sql_error(true);
        B().set_sql_message(S(m));
    }

    void on_sql_done(uint64_t cg, QueryOutput out)
    {
        sql_running_ = false;
        B().set_sql_running(false);
        set_busy();
        if (history_.record(sql_last_text_, (long long)std::time(nullptr))) history_.save();
        if (cg != conn_gen_) return;
        std::string msg = sql_message_for(out);
        bool refresh_table = out.columns.empty() && table_open_ && changes_.empty();
        SqlTab *t = sql_tab_by_id(sql_run_id_);
        if (t && t->id != sql_tabs_[sql_active_].id) {
            t->out = std::move(out);
            t->msg = msg;
            t->err = false;
        } else {
            sql_out_ = std::move(out);
            render_sql_out();
            B().set_sql_error(false);
            B().set_sql_message(S(msg));
        }
        if (refresh_table) load_page(true);
    }

    std::string sql_message_for(const QueryOutput &o) const
    {
        if (!o.message.empty() && o.columns.empty()) return o.message;
        if (o.columns.empty() && o.affected >= 0) return trf("sql.affected", { std::to_string(o.affected), std::to_string(o.elapsed_ms) });
        return trf("sql.done-rows", { i18n::with_commas(o.rows.size()), std::to_string(o.elapsed_ms), o.truncated ? trf("sql.truncated", { i18n::with_commas(o.rows.size()) }) : std::string() });
    }

    // ------------------------------------------------------------------ SQL query tabs
    struct SqlTab {
        int id = 0;
        std::string title, text, msg;
        bool err = false;
        QueryOutput out;
        std::vector<double> widths;
    };

    SqlTab *sql_tab_by_id(int id)
    {
        for (auto &t : sql_tabs_) if (t.id == id) return &t;
        return nullptr;
    }

    void init_sql_tabs()
    {
        add_sql_tab_entry();
        sql_active_ = 0;
        refresh_sql_tab_model();
    }

    void add_sql_tab_entry()
    {
        SqlTab t;
        t.id = ++sql_tab_seq_;
        t.title = trf("sql.tab-title", { std::to_string(t.id) });
        sql_tabs_.push_back(std::move(t));
    }

    void refresh_sql_tab_model()
    {
        std::vector<TabUi> v;
        for (size_t k = 0; k < sql_tabs_.size(); ++k) {
            TabUi u {};
            u.title = S(sql_tabs_[k].title);
            u.active = k == sql_active_;
            v.push_back(u);
        }
        sql_tabs_model_->set_vector(v);
    }

    void stash_active_sql_tab()
    {
        SqlTab &t = sql_tabs_[sql_active_];
        t.text = U(B().get_sql_text());
        t.out = std::move(sql_out_);
        t.widths = sql_widths_;
        t.msg = U(B().get_sql_message());
        t.err = B().get_sql_error();
        sql_out_ = QueryOutput();
    }

    void show_sql_tab(size_t k)
    {
        sql_active_ = k;
        SqlTab &t = sql_tabs_[k];
        B().set_sql_text(S(t.text));
        sql_out_ = std::move(t.out);
        t.out = QueryOutput();
        render_sql_out();
        B().set_sql_message(S(t.msg));
        B().set_sql_error(t.err);
        refresh_sql_tab_model();
    }

    void switch_sql_tab(int i)
    {
        if (i < 0 || (size_t)i >= sql_tabs_.size() || (size_t)i == sql_active_) return;
        stash_active_sql_tab();
        show_sql_tab((size_t)i);
    }

    void new_sql_tab()
    {
        stash_active_sql_tab();
        add_sql_tab_entry();
        show_sql_tab(sql_tabs_.size() - 1);
    }

    void close_sql_tab(int i)
    {
        if (i < 0 || (size_t)i >= sql_tabs_.size()) return;
        if (sql_running_ && sql_tabs_[(size_t)i].id == sql_run_id_) return; // let it finish or be cancelled first
        if (sql_tabs_.size() == 1) {
            // the last tab is cleared instead of removed
            sql_tabs_[0].text.clear();
            sql_tabs_[0].msg.clear();
            sql_tabs_[0].err = false;
            sql_out_ = QueryOutput();
            show_sql_tab(0);
            return;
        }
        bool was_active = (size_t)i == sql_active_;
        if (!was_active) stash_active_sql_tab();
        int keep_id = was_active ? 0 : sql_tabs_[sql_active_].id;
        sql_tabs_.erase(sql_tabs_.begin() + i);
        size_t next = 0;
        if (was_active) next = std::min((size_t)i, sql_tabs_.size() - 1);
        else for (size_t k = 0; k < sql_tabs_.size(); ++k) if (sql_tabs_[k].id == keep_id) next = k;
        sql_out_ = QueryOutput();
        show_sql_tab(next);
    }

    void render_sql_out()
    {
        std::vector<ColumnUi> cols;
        sql_widths_.clear();
        double x = 0;
        for (size_t k = 0; k < sql_out_.columns.size(); ++k) {
            size_t longest = sql_out_.columns[k].size();
            bool numeric = false;
            for (size_t r = 0; r < sql_out_.rows.size() && r < 100; ++r) {
                const Value &v = sql_out_.rows[r][k];
                longest = std::max(longest, std::min<size_t>(v.display().size(), 60));
                if (v.kind == Value::Kind::Int || v.kind == Value::Kind::Float || v.kind == Value::Kind::Decimal) numeric = true;
            }
            double w = std::clamp((double)longest * 8.0 + 28.0, 80.0, 420.0);
            sql_widths_.push_back(w);
            ColumnUi u {};
            u.name = S(sql_out_.columns[k]);
            u.x = (float)x;
            u.width = (float)w;
            u.align_right = numeric;
            cols.push_back(u);
            x += w;
        }
        sql_columns_model_->set_vector(cols);
        B().set_sql_content_width((float)x);
        std::vector<RowUi> rows;
        rows.reserve(sql_out_.rows.size());
        for (size_t r = 0; r < sql_out_.rows.size(); ++r) {
            RowUi row {};
            auto cells = std::make_shared<slint::VectorModel<CellUi>>();
            for (auto &v : sql_out_.rows[r]) cells->push_back(make_cell(v, false));
            row.cells = cells;
            row.label = S(std::to_string(r + 1));
            rows.push_back(row);
        }
        sql_rows_model_->set_vector(rows);
    }

    // ------------------------------------------------------------------ SQL history
    static std::string when_text(long long t)
    {
        long long now = (long long)std::time(nullptr);
        long long d = now - t;
        if (d < 60) return tr("hist.just-now");
        std::time_t tt = (std::time_t)t;
        std::tm tm {};
        localtime_s(&tm, &tt);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
        return buf;
    }

    void rebuild_history()
    {
        hist_index_ = history_.view(hist_needle_, B().get_hist_favorites_only());
        std::vector<HistoryUi> rows;
        rows.reserve(hist_index_.size());
        for (size_t k : hist_index_) {
            const HistoryEntry &e = history_.entries()[k];
            HistoryUi u {};
            u.preview = S(QueryHistory::preview(e.text));
            u.when = S(when_text(e.last_used));
            u.runs = e.runs;
            u.favorite = e.favorite;
            rows.push_back(u);
        }
        hist_model_->set_vector(rows);
        B().set_hist_total((int)history_.entries().size());
    }

    // Map the row the user clicked back to the entry, ignoring stale indexes.
    const HistoryEntry *hist_entry(int row, size_t *real = nullptr) const
    {
        if (row < 0 || (size_t)row >= hist_index_.size()) return nullptr;
        if (real) *real = hist_index_[(size_t)row];
        return &history_.entries()[hist_index_[(size_t)row]];
    }

    void wire_history(const Backend &b)
    {
        b.on_hist_open_clicked([this] {
            hist_needle_.clear();
            B().set_hist_favorites_only(false);
            rebuild_history();
            B().set_hist_open(true);
        });
        b.on_hist_search([this](slint::SharedString t) { hist_needle_ = U(t); rebuild_history(); });
        b.on_hist_favorites_toggled([this](bool) { rebuild_history(); });
        b.on_hist_use([this](int row) {
            const HistoryEntry *e = hist_entry(row);
            if (!e) return;
            B().set_sql_text(S(e->text));
            B().set_hist_open(false);
        });
        b.on_hist_toggle_favorite([this](int row) {
            size_t real = 0;
            const HistoryEntry *e = hist_entry(row, &real);
            if (!e) return;
            history_.set_favorite(real, !e->favorite);
            history_.save();
            rebuild_history();
        });
        b.on_hist_delete([this](int row) {
            size_t real = 0;
            if (!hist_entry(row, &real)) return;
            history_.remove(real);
            history_.save();
            rebuild_history();
        });
        b.on_hist_clear([this] {
            confirm(tr("hist.clear-title"), tr("hist.clear-text"), "", tr("hist.clear"), "", true, false, [this](int r) {
                if (r != 1) return;
                history_.clear_plain();
                history_.save();
                rebuild_history();
            });
        });
    }

    void open_sql_file()
    {
        auto p = platform::open_dialog(tr("sql.open"), { { "SQL", "*.sql" }, { "*.*", "*.*" } });
        if (!p) return;
        std::ifstream f(std::filesystem::u8path(*p), std::ios::binary);
        if (!f) return;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string t = ss.str();
        if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t.erase(0, 3);
        B().set_sql_text(S(t));
    }

    void save_sql_file(const std::string &text)
    {
        auto p = platform::save_dialog(tr("sql.save"), "query.sql", { { "SQL", "*.sql" } });
        if (!p) return;
        std::ofstream f(std::filesystem::u8path(*p), std::ios::binary | std::ios::trunc);
        f << text;
    }

    void copy_sql_result()
    {
        if (sql_out_.rows.empty()) {
            set_status(tr("sql.copy-empty"));
            return;
        }
        std::string text;
        for (size_t c = 0; c < sql_out_.columns.size(); ++c) {
            if (c) text += '\t';
            text += sql_out_.columns[c];
        }
        text += "\r\n";
        for (const auto &row : sql_out_.rows) {
            for (size_t c = 0; c < row.size(); ++c) {
                if (c) text += '\t';
                std::string cell = row[c].is_null() ? std::string() : row[c].display();
                for (char &ch : cell) if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
                text += cell;
            }
            text += "\r\n";
        }
        if (platform::clipboard_set(text)) set_status(tr("status.copied"));
    }

private:
    slint::ComponentHandle<AppWindow> ui_;
    Store store_;
    QueryHistory history_;
    ColumnLayouts layouts_;
    std::vector<size_t> paste_targets_;
    std::vector<std::vector<Value>> paste_rows_;
    ColumnView view_;
    std::shared_ptr<slint::VectorModel<ColumnCfgUi>> colcfg_model_ = std::make_shared<slint::VectorModel<ColumnCfgUi>>();
    std::optional<std::string> saved_clipboard_;
    std::vector<size_t> hist_index_;
    std::string hist_needle_, sql_last_text_;
    std::shared_ptr<slint::VectorModel<HistoryUi>> hist_model_ = std::make_shared<slint::VectorModel<HistoryUi>>();
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<int> workers_ { 0 };

    // connection
    bool connected_ = false, connecting_ = false, last_connection_failed_ = false;
    std::string connecting_id_;
    Profile profile_;
    std::shared_ptr<Connector> conn_;
    Capabilities caps_;
    uint64_t conn_gen_ = 0;
    Node root_;
    std::vector<Flat> flat_;
    std::string editing_id_;

    // table view
    bool table_open_ = false, editable_ = false, loading_ = false, saving_ = false, counting_ = false, has_more_ = false;
    TableMeta meta_;
    TableRef table_ref_;
    PageRequest req_;
    std::vector<std::vector<Value>> rows_;
    ChangeSet changes_;
    std::vector<ColFilter> filters_;
    std::vector<double> widths_;
    FilterGroup pending_group_;
    std::string pending_sig_, applied_sig_;
    int applied_filter_count_ = 0;
    TotalMode total_mode_ = TotalMode::Unknown;
    uint64_t total_ = 0;
    int page_size_ = 100;
    uint64_t fetch_gen_ = 0;
    CancelPtr fetch_cancel_, count_cancel_;
    std::vector<Op> menu_ops_;
    int menu_col_ = -1;
    slint::Timer filter_timer_;

    // tabs
    std::vector<TabState> tabs_;
    int active_tab_ = -1;
    bool page_loaded_ = false;

    // updates
    updater::Release release_;
    int upd_state_ = 0;
    std::string upd_key_;
    std::vector<std::string> upd_args_;

    // performance monitor
    slint::Timer mon_timer_;
    slint::Timer reconnect_timer_;
    bool reconnecting_ = false;
    int reconnect_attempt_ = 0;
    bool mon_busy_ = false, mon_paused_ = false, mon_have_ = false;
    uint64_t mon_gen_ = 0;
    Activity mon_activity_;
    Clock::time_point mon_time_, mon_prev_time_;
    std::string mon_updated_, mon_sel_id_;
    std::map<std::string, double> mon_prev_, mon_rates_;
    std::map<std::string, std::deque<double>> mon_history_;
    std::string mon_search_;
    bool mon_blocked_only_ = false;
    int mon_sort_col_ = -1;
    bool mon_sort_ascending_ = true;
    std::vector<size_t> mon_row_indices_;

    // sql
    bool sql_running_ = false;
    std::vector<SqlTab> sql_tabs_;
    size_t sql_active_ = 0;
    int sql_tab_seq_ = 0, sql_run_id_ = 0;
    std::shared_ptr<slint::VectorModel<TabUi>> sql_tabs_model_ = std::make_shared<slint::VectorModel<TabUi>>();
    CancelPtr sql_cancel_;
    QueryOutput sql_out_;
    std::vector<double> sql_widths_;

    // export
    bool exporting_ = false, export_from_sql_ = false;
    CancelPtr exp_cancel_;

    std::function<void(int)> dlg_cb_;

    std::shared_ptr<slint::VectorModel<ColumnUi>> columns_model_ = std::make_shared<slint::VectorModel<ColumnUi>>();
    std::shared_ptr<slint::VectorModel<RowUi>> rows_model_ = std::make_shared<slint::VectorModel<RowUi>>();
    std::shared_ptr<slint::VectorModel<ColumnUi>> sql_columns_model_ = std::make_shared<slint::VectorModel<ColumnUi>>();
    std::shared_ptr<slint::VectorModel<RowUi>> sql_rows_model_ = std::make_shared<slint::VectorModel<RowUi>>();
    std::shared_ptr<slint::VectorModel<TabUi>> tabs_model_ = std::make_shared<slint::VectorModel<TabUi>>();
    std::shared_ptr<slint::VectorModel<MetricUi>> mon_metrics_model_ = std::make_shared<slint::VectorModel<MetricUi>>();
    std::shared_ptr<slint::VectorModel<ColumnUi>> mon_columns_model_ = std::make_shared<slint::VectorModel<ColumnUi>>();
    std::shared_ptr<slint::VectorModel<RowUi>> mon_rows_model_ = std::make_shared<slint::VectorModel<RowUi>>();
    std::shared_ptr<slint::VectorModel<TreeRowUi>> tree_model_ = std::make_shared<slint::VectorModel<TreeRowUi>>();
    std::shared_ptr<slint::VectorModel<slint::SharedString>> op_menu_model_ = std::make_shared<slint::VectorModel<slint::SharedString>>();
    std::shared_ptr<slint::VectorModel<slint::SharedString>> engines_model_ = std::make_shared<slint::VectorModel<slint::SharedString>>();
};

int run_app()
{
    Controller c;
    return c.run();
}