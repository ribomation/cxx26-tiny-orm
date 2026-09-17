#include <catch2/catch_test_macros.hpp>

#include <tiny-orm/connect.hxx>
#include <tiny-orm/psql.hxx>

#include "scenario.hxx"

#include <cstdlib>
#include <memory>
#include <string>

namespace {

    /// Where the container from docker/docker-compose.yml listens.
    /// Override with TINY_ORM_TEST_PG_URL to point at another server.
    auto test_url() -> std::string {
        if (auto const* env = std::getenv("TINY_ORM_TEST_PG_URL")) return env;
        return "postgresql://orm:orm@localhost:5432/tiny_orm_test";
    }

    /// Connects, or returns nothing if no server is listening. The container is
    /// not part of the build, so an unreachable database skips rather than fails.
    auto try_connect() -> std::unique_ptr<tiny_orm::connection> {
        try {
            return tiny_orm::connect(test_url());
        } catch (tiny_orm::db_error const&) {
            return {};
        }
    }

} // namespace

TEST_CASE("PostgreSQL", "[integration][psql]") {
    auto db = try_connect();
    if (!db) {
        SKIP("no PostgreSQL at " + test_url()
             + " -- start it with: docker compose -f docker/docker-compose.yml up -d");
    }
    CHECK(db->sql_dialect().name() == "postgresql");
    run_crud_scenario(*db);
}

TEST_CASE("PostgreSQL generates keys with an identity column", "[integration][psql]") {
    auto db = try_connect();
    if (!db) SKIP("no PostgreSQL at " + test_url());

    auto accounts = DAO<Account>{*db};
    accounts.drop({.cascade = true});
    accounts.create();

    auto first = Account{.ownerName = "first", .balance = 1.0};
    auto second = Account{.ownerName = "second", .balance = 2.0};
    accounts.insert(first);
    accounts.insert(second);

    CHECK(second.id == first.id + 1);
    accounts.drop({.cascade = true});
}
