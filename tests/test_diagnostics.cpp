#include "test.h"
#include "diagnostics.h"

#include <filesystem>
#include <string>

using namespace dbp::diagnostics;

TEST(crash_record_contains_only_fault_metadata)
{
    std::string r = format_exception_record(0xC0000005u, 0x1234u, "dbpro_driver_mssql.dll", Phase::LoadingTableMetadata);
    CHECK(r.find("0xC0000005") != std::string::npos);
    CHECK(r.find("0x0000000000001234") != std::string::npos);
    CHECK(r.find("dbpro_driver_mssql.dll") != std::string::npos);
    CHECK(r.find("table_metadata") != std::string::npos);
    CHECK(r.find("password") == std::string::npos);
    CHECK(r.find("SELECT") == std::string::npos);
}

TEST(crash_record_uses_unknown_for_invalid_module)
{
    std::string r = format_exception_record(0, 0, "", static_cast<Phase>(999));
    CHECK(r.find("unknown") != std::string::npos);
    CHECK(r.find("0x00000000") != std::string::npos);
}

TEST(crash_record_strips_paths_and_control_characters_from_module)
{
    std::string r = format_exception_record(1, 1, "C:\\private\\dbpro.dll\r\npassword=secret", Phase::Idle);
    CHECK(r.find("C:\\") == std::string::npos);
    CHECK(r.find("password") == std::string::npos);
    CHECK(r.find('\n') == std::string::npos);
    CHECK(r.find("module=dbpro.dll") != std::string::npos);
}

TEST(crash_phase_is_thread_local_and_restorable)
{
    set_phase(Phase::LoadingTableMetadata);
    CHECK_EQ(current_phase(), Phase::LoadingTableMetadata);
    set_phase(Phase::Idle);
    CHECK_EQ(current_phase(), Phase::Idle);
}

TEST(session_marker_detects_abnormal_exit_with_phase_and_version)
{
    namespace fs = std::filesystem;
    fs::path d = fs::temp_directory_path() / "dbpro_session_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    auto first = begin_session(d.string(), "1.2.3");
    CHECK(!first.abnormal); // no marker yet
    set_phase(Phase::LoadingTableMetadata);
    // Simulated crash: the run never calls end_session_clean().
    auto second = begin_session(d.string(), "1.2.4");
    CHECK(second.abnormal);
    CHECK(second.version == "1.2.3");
    CHECK(second.phase == Phase::LoadingTableMetadata);
    end_session_clean();
    auto third = begin_session(d.string(), "1.2.5");
    CHECK(!third.abnormal); // the previous run ended cleanly
    end_session_clean();
    fs::remove_all(d, ec);
}

TEST(previous_session_record_is_sanitised)
{
    PreviousSession p;
    p.abnormal = true;
    p.version = "0.2.7\r\npassword=x";
    p.phase = Phase::FetchingPage;
    std::string r = format_previous_session_record(p);
    CHECK(r.find('\n') == std::string::npos && r.find("password") == std::string::npos);
    CHECK(r.find("phase=fetching_page") != std::string::npos);
}