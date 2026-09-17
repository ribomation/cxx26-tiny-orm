#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <tiny-orm/dao.hxx>
#include <tiny-orm/psql.hxx>
#include <tiny-orm/sqlite.hxx>

#include "fake-connection.hxx"
#include "model.hxx"

#include <vector>

using Catch::Matchers::ContainsSubstring;

static psql_dialect const pg{};
static sqlite_dialect const lite{};

static auto make_rows(int n) -> std::vector<Account> {
    auto rows = std::vector<Account>{};
    for (auto i = 0; i < n; ++i)
        rows.push_back(Account{.ownerName = std::format("owner{}", i), .balance = double(i)});
    return rows;
}

TEST_CASE("a multi-row INSERT names the columns once", "[bulk]") {
    auto const rows = make_rows(3);
    auto const s = insert_many<Account>(rows, pg);

    CHECK_THAT(s.sql, ContainsSubstring(R"(INSERT INTO "bank_accounts" ("owner", "balance", "nick_name") VALUES )"));
    CHECK_THAT(s.sql, ContainsSubstring("($1, $2, $3), ($4, $5, $6), ($7, $8, $9)"));
    CHECK_THAT(s.sql, ContainsSubstring(R"(RETURNING "id")"));
    CHECK(s.params.size() == 9); // 3 rows x 3 bound columns; the key is generated
}

TEST_CASE("placeholders in a batch follow the dialect", "[bulk]") {
    auto const rows = make_rows(2);
    CHECK_THAT(insert_many<Account>(rows, lite).sql, ContainsSubstring("(?, ?, ?), (?, ?, ?)"));
}

TEST_CASE("a bulk insert is chopped into batches", "[bulk]") {
    auto fake = fake_connection{};
    auto accounts = DAO<Account>{fake};
    auto rows = make_rows(250);

    accounts.insert_all(rows, {.batch_size = 100});

    CHECK(fake.executed.size() == 3); // 100 + 100 + 50
    CHECK(fake.executed[0].params.size() == 300);
    CHECK(fake.executed[2].params.size() == 150);
}

TEST_CASE("the batch is clamped to the engine's parameter budget", "[bulk]") {
    auto fake = fake_connection{};       // PostgreSQL: 65535 params, 3 bound columns
    auto accounts = DAO<Account>{fake};
    auto rows = make_rows(50'000);

    accounts.insert_all(rows, {.batch_size = 1'000'000}); // absurd on purpose

    REQUIRE_FALSE(fake.executed.empty());
    for (auto const& s : fake.executed) CHECK(s.params.size() <= 65535);
}

TEST_CASE("generated keys are written back across batches", "[bulk]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id"}, {{"1"}, {"2"}}));
    fake.queued.push_back(result_set::of({"id"}, {{"3"}}));

    auto accounts = DAO<Account>{fake};
    auto rows = make_rows(3);
    auto const n = accounts.insert_all(rows, {.batch_size = 2});

    CHECK(n == 3);
    CHECK(rows[0].id == 1);
    CHECK(rows[1].id == 2);
    CHECK(rows[2].id == 3);
}

TEST_CASE("an empty bulk insert issues nothing", "[bulk]") {
    auto fake = fake_connection{};
    auto rows = std::vector<Account>{};
    CHECK(DAO<Account>{fake}.insert_all(rows) == 0);
    CHECK(fake.executed.empty());
}

// ---------------------------------------------------------------- select_into

/// A plain struct: no annotations, not an entity.
struct AirlineDelay {
    std::string airline{};
    long flights{};
    double avgDelay{};  // -> avg_delay
    double onTimePct{}; // -> on_time_pct
};

TEST_CASE("select_into materialises an arbitrary struct", "[select_into]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"airline", "flights", "avg_delay", "on_time_pct"},
                                         {{"Finnair", "1204", "7.4", "88.2"},
                                          {"SAS", "980", "12.1", "79.5"}}));

    auto const rows = select_into<AirlineDelay>(fake, "SELECT ...");

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].airline == "Finnair");
    CHECK(rows[0].flights == 1204);
    CHECK(rows[1].avgDelay == 12.1);
    CHECK(rows[1].onTimePct == 79.5);
}

TEST_CASE("select_into passes its parameters through", "[select_into]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"airline", "flights", "avg_delay", "on_time_pct"}, {}));

    auto const rows = select_into<AirlineDelay>(fake, "SELECT ... WHERE delay > $1 AND name = $2",
                                                bind(15, "SAS"));

    CHECK(rows.empty());
    REQUIRE(fake.executed.size() == 1);
    CHECK(fake.executed[0].params == std::vector<param>{"15", "SAS"});
}

TEST_CASE("select_one_into returns nothing for an empty result", "[select_into]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"airline", "flights", "avg_delay", "on_time_pct"}, {}));
    CHECK_FALSE(select_one_into<AirlineDelay>(fake, "SELECT ...").has_value());
}
