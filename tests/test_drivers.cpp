#include <windows.h>

#include <filesystem>
#include <fstream>

#include "drivers/driver_manager.h"
#include "test.h"

using namespace dbp;
namespace fs = std::filesystem;

static void set_env(const wchar_t *k, const fs::path &v) { SetEnvironmentVariableW(k, v.wstring().c_str()); }

static fs::path fresh_dir(const char *name)
{
    fs::path p = fs::temp_directory_path() / (std::string("dbpro_drv_") + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

TEST(sha256_known_vectors)
{
    CHECK_EQ(drivers::sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(drivers::sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(sqlite_is_built_in_and_other_engines_are_not)
{
    CHECK(drivers::probe_for_engine("sqlite").state == drivers::State::BuiltIn);
    CHECK(drivers::probe_for_engine("postgres").state == drivers::State::NotImplemented);
    CHECK(drivers::probe_for_engine("mssql").state == drivers::State::NotImplemented);
    CHECK(drivers::probe_for_engine("mongodb").state == drivers::State::NotImplemented);
    CHECK(drivers::probe_for_engine("nonsense").state == drivers::State::NotImplemented);
}

TEST(missing_driver_is_reported_not_crashed)
{
    drivers::unload_all_for_tests();
    fs::path empty = fresh_dir("empty"), data = fresh_dir("data_empty");
    set_env(L"DBPRO_DRIVERS_DIR", empty);
    set_env(L"DBPRO_DATA_DIR", data);
    CHECK(drivers::status_for_engine("mysql").state == drivers::State::Missing);
    Profile p;
    p.engine = "mysql";
    p.host = "127.0.0.1";
    bool threw = false;
    try {
        drivers::connect(p, "");
    } catch (const DbError &e) {
        threw = e.kind == ErrKind::Unsupported;
    }
    CHECK(threw);
}

TEST(user_folder_driver_without_recorded_checksum_is_rejected)
{
    drivers::unload_all_for_tests();
    fs::path empty = fresh_dir("empty2"), data = fresh_dir("data_user");
    set_env(L"DBPRO_DRIVERS_DIR", empty);
    set_env(L"DBPRO_DATA_DIR", data);
    fs::create_directories(data / "drivers");
    std::ofstream(data / "drivers" / "dbpro_driver_mysql.dll", std::ios::binary) << "this is not a dll";
    auto st = drivers::status_for_engine("mysql");
    CHECK(st.state == drivers::State::Incompatible);
    CHECK(st.detail.find("checksum") != std::string::npos);
}

TEST(shipped_driver_loads_and_reports_a_clear_connection_error)
{
    fs::path shipped = fs::path(DBPRO_TEST_DRIVERS_DIR);
    if (!fs::exists(shipped / "dbpro_driver_mysql.dll")) {
        std::printf("    (skipped: build target dbpro_drivers first)\n");
        return;
    }
    set_env(L"DBPRO_DRIVERS_DIR", shipped);
    set_env(L"DBPRO_DATA_DIR", fresh_dir("data_ok"));
    auto st = drivers::status_for_engine("mysql");
    CHECK(st.state == drivers::State::Ready);
    CHECK(st.detail.find("libmariadb") != std::string::npos);
    // Port 1 on loopback: nothing listens there. Nothing real is contacted.
    Profile p;
    p.engine = "mariadb";
    p.host = "127.0.0.1";
    p.port = 1;
    p.tls = false;
    bool threw = false;
    try {
        drivers::connect(p, "");
    } catch (const DbError &e) {
        threw = e.kind == ErrKind::Connection && std::string(e.what()).size() > 0;
    }
    CHECK(threw);
}

static void write_index(const fs::path &data, const std::string &hex)
{
    std::ofstream(data / "drivers" / "installed.json") << "{\"files\":{\"dbpro_driver_mysql.dll\":\"" << hex << "\"}}";
}

TEST(user_folder_driver_loads_only_with_matching_checksum)
{
    fs::path shipped = fs::path(DBPRO_TEST_DRIVERS_DIR) / "dbpro_driver_mysql.dll";
    if (!fs::exists(shipped)) { std::printf("    (skipped: build target dbpro_drivers first)\n"); return; }
    drivers::unload_all_for_tests();
    fs::path empty = fresh_dir("empty3"), data = fresh_dir("data_sum");
    set_env(L"DBPRO_DRIVERS_DIR", empty);
    set_env(L"DBPRO_DATA_DIR", data);
    fs::create_directories(data / "drivers");
    fs::path target = data / "drivers" / "dbpro_driver_mysql.dll";
    fs::copy_file(shipped, target, fs::copy_options::overwrite_existing);

    std::string hex;
    CHECK(drivers::sha256_file(target.string(), hex));
    write_index(data, std::string(64, '0')); // wrong checksum: must be refused before loading
    CHECK(drivers::status_for_engine("mysql").state == drivers::State::Incompatible);
    CHECK(drivers::probe_for_engine("mysql").state == drivers::State::Ready); // probe never loads, only status_for_engine validates

    write_index(data, hex);
    auto st = drivers::status_for_engine("mysql");
    CHECK(st.state == drivers::State::Ready);
    CHECK(st.detail.find("libmariadb") != std::string::npos);
}