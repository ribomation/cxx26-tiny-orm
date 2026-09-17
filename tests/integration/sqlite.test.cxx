#include <catch2/catch_test_macros.hpp>

#include <tiny-orm/connect.hxx>
#include <tiny-orm/sqlite.hxx>

#include "scenario.hxx"

#include <cstdio>
#include <filesystem>
#include <string>

TEST_CASE("SQLite in memory", "[integration][sqlite]") {
    auto db = tiny_orm::sqlite_connection{":memory:"};
    run_crud_scenario(db);
}

TEST_CASE("SQLite on disk", "[integration][sqlite][file]") {
    // One file-backed run, so the tests exercise a database that is actually
    // written and reopened rather than only living in process memory.
    auto const path = std::filesystem::temp_directory_path() / "tiny-orm-integration.sqlite";
    std::filesystem::remove(path);

    {
        auto db = tiny_orm::sqlite_connection{path.string()};
        run_crud_scenario(db);
    }
    CHECK(std::filesystem::exists(path));

    SECTION("data written by one connection is visible to the next") {
        {
            auto db = tiny_orm::sqlite_connection{path.string()};
            auto accounts = DAO<Account>{db};
            accounts.drop();
            accounts.create();
            auto a = Account{.ownerName = "Persisted", .balance = 7.0};
            accounts.insert(a);
        }
        {
            auto db = tiny_orm::sqlite_connection{path.string()};
            auto accounts = DAO<Account>{db};
            REQUIRE(accounts.count() == 1);
            CHECK(accounts.select().first()->ownerName == "Persisted");
        }
    }
    std::filesystem::remove(path);
}

TEST_CASE("the factory dispatches on the URL scheme", "[integration][sqlite]") {
    auto db = tiny_orm::connect("sqlite::memory:");
    REQUIRE(db != nullptr);
    CHECK(db->sql_dialect().name() == "sqlite");

    CHECK_THROWS_AS(tiny_orm::connect("mysql://localhost/x"), tiny_orm::db_error);
}
