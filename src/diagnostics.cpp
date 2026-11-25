#include "diagnostics.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>

#include <windows.h>
#include <process.h>

#include "platform.h"

namespace dbp::diagnostics {

namespace {

thread_local Phase t_phase = Phase::Idle;
std::atomic<uint32_t> g_last_phase { 0 }; // last phase set on any thread

struct Marker {
    uint32_t magic;
    uint32_t clean; // 1 = ended normally
    std::atomic<uint32_t> phase;
    uint32_t pid;
    char version[32];
};
constexpr uint32_t kMarkerMagic = 0x44425031; // 'DBP1'
Marker *g_marker = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
std::atomic_flag g_written = ATOMIC_FLAG_INIT;

const char *phase_name(Phase phase)
{
    switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::Connecting: return "connecting";
    case Phase::ListingObjects: return "listing_objects";
    case Phase::LoadingTableMetadata: return "table_metadata";
    case Phase::FetchingPage: return "fetching_page";
    case Phase::RenderingPage: return "rendering_page";
    case Phase::SavingChanges: return "saving_changes";
    case Phase::Monitoring: return "monitoring";
    case Phase::Exporting: return "exporting";
    default: return "unknown";
    }
}

std::string safe_module(const std::string &input)
{
    size_t slash = input.find_last_of("/\\");
    std::string name = slash == std::string::npos ? input : input.substr(slash + 1);
    std::string out;
    out.reserve((std::min)(name.size(), size_t(96)));
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) break;
        if (out.size() == 96) break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out.push_back((char)c);
    }
    return out.empty() ? "unknown" : out;
}

void write_record(const std::string &record) noexcept
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st {};
    GetLocalTime(&st);
    char prefix[64] {};
    int n = std::snprintf(prefix, sizeof prefix, "%04u-%02u-%02uT%02u:%02u:%02u ",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD written = 0;
    if (n > 0) WriteFile(g_log, prefix, (DWORD)n, &written, nullptr);
    WriteFile(g_log, record.data(), (DWORD)record.size(), &written, nullptr);
    static constexpr char nl = '\n';
    WriteFile(g_log, &nl, 1, &written, nullptr);
    FlushFileBuffers(g_log);
}

void append_record(const std::string &record) noexcept
{
    if (g_written.test_and_set()) return;
    write_record(record);
}

std::string module_for_address(void *address)
{
    MEMORY_BASIC_INFORMATION mbi {};
    if (!address || !VirtualQuery(address, &mbi, sizeof mbi) || !mbi.AllocationBase) return "unknown";
    wchar_t path[MAX_PATH] {};
    DWORD n = GetModuleFileNameW((HMODULE)mbi.AllocationBase, path, MAX_PATH);
    return n ? platform::narrow(std::wstring(path, n)) : "unknown";
}

LONG WINAPI unhandled_exception(EXCEPTION_POINTERS *ep)
{
    uint32_t code = 0;
    uintptr_t address = 0;
    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        address = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    }
    append_record(format_exception_record(code, address, module_for_address((void *)address), current_phase()));
    return EXCEPTION_CONTINUE_SEARCH;
}

void report_other(uint32_t code, const char *what) noexcept
{
    append_record(format_exception_record(code, 0, what, Phase(g_last_phase.load())));
}

void abort_signal(int) { report_other(0xE0000002u, "abort"); _exit(3); }
void invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) { report_other(0xE0000003u, "invalid-parameter"); _exit(3); }
void purecall() { report_other(0xE0000004u, "pure-virtual-call"); _exit(3); }

void terminate_handler() noexcept
{
    append_record(format_exception_record(0xE0000001u, 0, "std-terminate", current_phase()));
    std::abort();
}

} // namespace

void set_phase(Phase phase) noexcept
{
    t_phase = phase;
    g_last_phase.store((uint32_t)phase, std::memory_order_relaxed);
    if (g_marker) g_marker->phase.store((uint32_t)phase, std::memory_order_relaxed);
}
Phase current_phase() noexcept { return t_phase; }

std::string format_exception_record(uint32_t code, uintptr_t address, const std::string &module, Phase phase)
{
    std::ostringstream out;
    out << "exception code=0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << code
        << " address=0x" << std::setw((int)(sizeof(uintptr_t) * 2)) << address
        << " module=" << safe_module(module) << " phase=" << phase_name(phase);
    return out.str();
}

void install_crash_handler()
{
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(platform::widen(platform::data_dir())) / L"logs";
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
        g_log = CreateFileW((dir / L"crashes.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    SetUnhandledExceptionFilter(unhandled_exception);
    std::set_terminate(terminate_handler);
    std::signal(SIGABRT, abort_signal);
    _set_invalid_parameter_handler(invalid_parameter);
    _set_purecall_handler(purecall);
    ULONG guarantee = 64 * 1024; // leave room for the handler to run after a stack overflow
    SetThreadStackGuarantee(&guarantee);
}

std::string format_previous_session_record(const PreviousSession &p)
{
    return std::string("previous-session abnormal-exit version=") + safe_module(p.version) + " phase=" + phase_name(p.phase);
}

PreviousSession begin_session(const std::string &dir, const std::string &version)
{
    PreviousSession prev;
    std::error_code ec;
    std::filesystem::path p = std::filesystem::path(platform::widen(dir)) / L"session.marker";
    std::filesystem::create_directories(p.parent_path(), ec);
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return prev;
    LARGE_INTEGER size {};
    GetFileSizeEx(f, &size);
    bool existed = size.QuadPart >= (LONGLONG)sizeof(Marker);
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, sizeof(Marker), nullptr);
    CloseHandle(f);
    if (!map) return prev;
    auto *m = (Marker *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Marker));
    CloseHandle(map);
    if (!m) return prev;
    if (existed && m->magic == kMarkerMagic && m->clean == 0) {
        prev.abnormal = true;
        prev.phase = (Phase)m->phase.load();
        prev.version.assign(m->version, strnlen(m->version, sizeof m->version));
    }
    m->magic = kMarkerMagic;
    m->clean = 0;
    m->phase.store(0);
    m->pid = GetCurrentProcessId();
    std::memset(m->version, 0, sizeof m->version);
    std::strncpy(m->version, version.c_str(), sizeof m->version - 1);
    g_marker = m;
    if (prev.abnormal) write_record(format_previous_session_record(prev));
    return prev;
}

void end_session_clean() noexcept
{
    if (g_marker) g_marker->clean = 1;
}

} // namespace dbp::diagnostics
