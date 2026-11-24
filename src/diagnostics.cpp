#include "diagnostics.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>

#include <windows.h>

#include "platform.h"

namespace dbp::diagnostics {

namespace {

thread_local Phase t_phase = Phase::Idle;
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

void append_record(const std::string &record) noexcept
{
    if (g_log == INVALID_HANDLE_VALUE || g_written.test_and_set()) return;
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

void terminate_handler() noexcept
{
    append_record(format_exception_record(0xE0000001u, 0, "std-terminate", current_phase()));
    std::abort();
}

} // namespace

void set_phase(Phase phase) noexcept { t_phase = phase; }
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
}

} // namespace dbp::diagnostics
