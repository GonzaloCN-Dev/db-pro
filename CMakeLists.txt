cmake_minimum_required(VERSION 3.21)
project(DbPro VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Dependencies come from vcpkg (manifest in vcpkg.json), installed to vcpkg_installed/.
set(DBPRO_PREFIX "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows-static-md")
list(APPEND CMAKE_PREFIX_PATH "${DBPRO_PREFIX}")

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------- core (no UI)
add_library(dbpro_core STATIC
    src/core/core.cpp
    src/core/sqlgen.cpp
    src/core/changes.cpp
    src/core/filtertext.cpp
    src/core/exporter.cpp
    src/store.cpp
    src/platform.cpp
    src/connectors/sqlite_conn.cpp
    src/connectors/factory.cpp
    src/drivers/driver_manager.cpp
    src/core/sqlsafety.cpp
    src/i18n.cpp
)
target_include_directories(dbpro_core PUBLIC src)
target_link_libraries(dbpro_core PUBLIC unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json advapi32 shell32 ole32 comdlg32 user32 bcrypt winhttp)
if(MSVC)
    target_compile_options(dbpro_core PUBLIC /utf-8 /W3 /permissive- /EHsc)
    target_compile_definitions(dbpro_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
target_compile_definitions(dbpro_core PUBLIC APP_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------- optional drivers (one DLL per engine family)
# Each driver carries its own client libraries. They are found at runtime in <exe>\drivers or %APPDATA%\DbPro\drivers
# and downloaded on demand, so the base app stays small and only ships SQLite.
set(DBPRO_DRIVER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/drivers")

find_package(unofficial-libmariadb CONFIG REQUIRED)
add_library(dbpro_driver_mysql SHARED src/drivers/mysql_driver.cpp src/connectors/mysql_conn.cpp)
target_link_libraries(dbpro_driver_mysql PRIVATE dbpro_core unofficial::libmariadb)

add_custom_target(dbpro_drivers DEPENDS dbpro_driver_mysql)
foreach(drv dbpro_driver_mysql)
    set_target_properties(${drv} PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${DBPRO_DRIVER_OUTPUT_DIR}")
endforeach()
enable_testing()
add_executable(dbpro_tests tests/test_main.cpp tests/test_sqlgen.cpp tests/test_sqlite.cpp tests/test_changes.cpp tests/test_export_store.cpp tests/test_sqlsafety.cpp tests/test_drivers.cpp)
target_link_libraries(dbpro_tests PRIVATE dbpro_core)
target_compile_definitions(dbpro_tests PRIVATE DBPRO_TEST_DRIVERS_DIR="${DBPRO_DRIVER_OUTPUT_DIR}")
add_dependencies(dbpro_tests dbpro_drivers)
add_test(NAME dbpro_tests COMMAND dbpro_tests)

add_executable(dbpro_sqlq tools/sqlq.cpp)
target_link_libraries(dbpro_sqlq PRIVATE unofficial::sqlite3::sqlite3)

add_executable(dbpro_seed tools/seed_sample.cpp)
target_link_libraries(dbpro_seed PRIVATE unofficial::sqlite3::sqlite3)
if(MSVC)
    target_compile_options(dbpro_seed PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------- application (Slint)
option(DBPRO_BUILD_APP "Build the Slint application" ON)
if(DBPRO_BUILD_APP)
    include(FetchContent)
    FetchContent_Declare(
        Slint
        GIT_REPOSITORY https://github.com/slint-ui/slint.git
        GIT_TAG v1.19.0
        GIT_SHALLOW ON
        SOURCE_SUBDIR api/cpp
    )
    set(SLINT_FEATURE_BACKEND_WINIT ON CACHE BOOL "" FORCE)
    set(SLINT_FEATURE_RENDERER_SKIA OFF CACHE BOOL "" FORCE)
    set(SLINT_FEATURE_RENDERER_FEMTOVG ON CACHE BOOL "" FORCE)
    set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(Slint)

    add_executable(DbPro WIN32
        src/main.cpp
        src/app.cpp
        src/selftest.cpp
        resources/app.rc
    )
    target_include_directories(DbPro PRIVATE src)
    target_link_libraries(DbPro PRIVATE dbpro_core Slint::Slint advapi32 shell32 ole32)
    if(MSVC)
        target_link_options(DbPro PRIVATE /ENTRY:mainCRTStartup)
    endif()
    set_property(TARGET DbPro PROPERTY SLINT_EMBED_RESOURCES embed-files)
    slint_target_sources(DbPro ui/app.slint)
    target_include_directories(DbPro PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    add_custom_command(TARGET DbPro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:DbPro> $<TARGET_FILE_DIR:DbPro>
        COMMAND_EXPAND_LISTS)
endif()
