cmake_minimum_required(VERSION 4.1)
project(cxx26_tiny_orm
        VERSION 1.0.0
        DESCRIPTION "An experiment with C++26 static reflection: how far can an ORM be derived from plain structs?"
        HOMEPAGE_URL "https://github.com/ribomation/cxx26-tiny-orm"
        LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(WARNING "This demo targets GCC's experimental C++26 reflection support; "
            "other compilers may not accept -freflection.")
endif()


set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE indexing
add_compile_options(-Wall -Wextra -Wpedantic)


# The framework itself.
#
# INTERFACE, because the compile-time half is header-only and produces no object
# code -- and add_library(STATIC) with no .cxx cannot pick a linker language.
# When the run-time half (SQL generation, libpq client) lands, change INTERFACE
# to STATIC, list the .cxx files in a plain target_sources(), and switch the
# keywords below from INTERFACE to PUBLIC.
add_library(tiny-orm INTERFACE)

# FILE_SET HEADERS also puts BASE_DIRS on the consumer's include path, so
# dependents can write #include <framework/compiletime-string.hxx>.
target_sources(tiny-orm INTERFACE
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/compiletime-string.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/annotations.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/schema.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/ddl.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/dml.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/connection.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/mapping.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/dao.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/dialect.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/psql.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/sqlite.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/connect.hxx
              ${CMAKE_CURRENT_SOURCE_DIR}/src/tiny-orm/csv.hxx)

target_compile_features(tiny-orm INTERFACE cxx_std_26)

# Propagated, not PRIVATE: the header uses std::define_static_string from <meta>,
# so every translation unit that includes it needs the flag too.
target_compile_options(tiny-orm INTERFACE -freflection)


# ---------------------------------------------------------------------------
# Backends. Both are optional: without either, the library still builds and the
# whole unit-test suite still runs, because those tests use a fake connection.
# ---------------------------------------------------------------------------
function(tiny_orm_report_missing what apt yum apk)
    message(STATUS "tiny-orm: ${what} not found -- that backend will NOT be built.")
    message(STATUS "          Install the client development package, then re-run CMake:")
    message(STATUS "            Debian/Ubuntu   sudo apt install ${apt}")
    message(STATUS "            Fedora/RHEL     sudo yum install ${yum}")
    message(STATUS "            Alpine          sudo apk add ${apk}")
endfunction()

find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
    message(STATUS "tiny-orm: libpq ${PostgreSQL_VERSION_STRING} found -- PostgreSQL backend enabled.")
    target_link_libraries(tiny-orm INTERFACE PostgreSQL::PostgreSQL)
    target_compile_definitions(tiny-orm INTERFACE TINY_ORM_HAS_POSTGRES)
else()
    tiny_orm_report_missing("libpq" "libpq-dev" "libpq-devel" "libpq-dev")
endif()

find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    message(STATUS "tiny-orm: SQLite ${SQLite3_VERSION} found -- SQLite backend enabled.")
    target_link_libraries(tiny-orm INTERFACE SQLite::SQLite3)
    target_compile_definitions(tiny-orm INTERFACE TINY_ORM_HAS_SQLITE)
else()
    tiny_orm_report_missing("libsqlite3" "libsqlite3-dev" "sqlite-devel" "sqlite-dev")
endif()


# Prints the SQL the framework generates, in both dialects. No database needed.
add_executable(sql-generation-example src/sql-generation-example.cxx)
target_link_libraries(sql-generation-example PRIVATE tiny-orm)

# Loads the OpenFlights data into whichever database the URL names. Needs a
# backend, so it is only built when one was found.
if(PostgreSQL_FOUND OR SQLite3_FOUND)
    add_executable(openflights-app src/openflights-app.cxx)
    target_link_libraries(openflights-app PRIVATE tiny-orm)
endif()


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(Catch2)

enable_testing()

# Unit tests: no database, everything through a recording fake connection.
add_executable(tiny-orm-unit-tests
        tests/unit/schema.test.cxx
        tests/unit/ddl.test.cxx
        tests/unit/dml.test.cxx
        tests/unit/dao.test.cxx
        tests/unit/transaction.test.cxx
        tests/unit/dialect.test.cxx
        tests/unit/bulk.test.cxx
        tests/unit/csv.test.cxx
        tests/unit/timestamp.test.cxx)
target_include_directories(tiny-orm-unit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tiny-orm-unit-tests PRIVATE tiny-orm Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tiny-orm-unit-tests)

# Integration tests: real engines. SQLite runs anywhere it is installed;
# PostgreSQL needs the docker/ container up, and skips itself when it is not.
set(TINY_ORM_INTEGRATION_SOURCES)
if(SQLite3_FOUND)
    list(APPEND TINY_ORM_INTEGRATION_SOURCES tests/integration/sqlite.test.cxx)
endif()
if(PostgreSQL_FOUND)
    list(APPEND TINY_ORM_INTEGRATION_SOURCES tests/integration/psql.test.cxx)
endif()

if(TINY_ORM_INTEGRATION_SOURCES)
    add_executable(tiny-orm-integration-tests ${TINY_ORM_INTEGRATION_SOURCES})
    target_include_directories(tiny-orm-integration-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(tiny-orm-integration-tests PRIVATE tiny-orm Catch2::Catch2WithMain)
    catch_discover_tests(tiny-orm-integration-tests)
else()
    message(STATUS "tiny-orm: no backend found -- integration tests will NOT be built.")
endif()
