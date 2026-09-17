#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <tiny-orm/dao.hxx>

#include "fake-connection.hxx"
#include "model.hxx"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("the key type is recovered from the primary key member", "[dao]") {
    STATIC_REQUIRE(std::is_same_v<DAO<Account>::key_type, long>);
}

TEST_CASE("insert writes the generated key back into the entity", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id"}, {{"42"}}));

    auto accounts = DAO<Account>{fake};
    auto ada = Account{.ownerName = "Ada", .balance = 10.0};
    accounts.insert(ada);

    CHECK(ada.id == 42);
    CHECK_THAT(fake.sql(0), ContainsSubstring(R"(RETURNING "id")"));
}

TEST_CASE("insert leaves a non-generated key alone", "[dao]") {
    auto fake = fake_connection{};
    auto ledgers = DAO<Ledger>{fake};
    auto l = Ledger{.id = 5, .label = "petty cash"};
    ledgers.insert(l);

    CHECK(l.id == 5);
}

TEST_CASE("load materialises an entity from a row", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "owner", "balance", "nick_name"},
                                         {{"7", "Ada", "1234.5", std::nullopt}}));

    auto const found = DAO<Account>{fake}.load(7);

    REQUIRE(found.has_value());
    CHECK(found->id == 7);
    CHECK(found->ownerName == "Ada");
    CHECK(found->balance == 1234.5);
    CHECK_FALSE(found->nickName.has_value());
}

TEST_CASE("load returns nothing for a missing row rather than throwing", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "owner", "balance", "nick_name"}, {}));

    CHECK_FALSE(DAO<Account>{fake}.load(999).has_value());
}

TEST_CASE("update and remove report the affected row count", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::affected(1));
    fake.queued.push_back(result_set::affected(0));

    auto accounts = DAO<Account>{fake};
    CHECK(accounts.update(Account{.id = 7, .ownerName = "Ada"}) == 1);
    CHECK(accounts.remove(999) == 0);
}

TEST_CASE("a query materialises every row", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "owner", "balance", "nick_name"},
                                         {{"1", "Ada", "10.0", std::nullopt},
                                          {"2", "Grace", "20.0", "Amazing"}}));

    constexpr auto balance = field<^^Account::balance>;
    auto const rows = DAO<Account>{fake}.select().where(balance > 5.0).all();

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].ownerName == "Ada");
    CHECK(rows[1].nickName == "Amazing");
}

TEST_CASE("first asks the server for a single row", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "owner", "balance", "nick_name"},
                                         {{"1", "Ada", "10.0", std::nullopt}}));

    auto const one = DAO<Account>{fake}.select().first();

    REQUIRE(one.has_value());
    CHECK(one->ownerName == "Ada");
    CHECK_THAT(fake.sql(0), ContainsSubstring("LIMIT $1"));
}

TEST_CASE("count reads the single COUNT(*) cell", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"count"}, {{"17"}}));

    CHECK(DAO<Account>{fake}.count() == 17);
    CHECK_THAT(fake.sql(0), ContainsSubstring("SELECT COUNT(*)"));
}

TEST_CASE("exists counts rows matching the key", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"count"}, {{"1"}}));

    CHECK(DAO<Account>{fake}.exists(7));
    CHECK_THAT(fake.sql(0), ContainsSubstring(R"(WHERE "id" = $1)"));
}

TEST_CASE("create and drop issue DDL", "[dao]") {
    auto fake = fake_connection{};
    auto accounts = DAO<Account>{fake};
    accounts.create();
    accounts.drop();

    CHECK_THAT(fake.sql(0), ContainsSubstring("CREATE TABLE IF NOT EXISTS"));
    CHECK_THAT(fake.sql(1), ContainsSubstring("DROP TABLE IF EXISTS"));
}

TEST_CASE("a NULL in a non-optional column is an error, not a default", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "owner", "balance", "nick_name"},
                                         {{"1", std::nullopt, "10.0", std::nullopt}}));

    CHECK_THROWS_AS(DAO<Account>{fake}.load(1), db_error);
}

TEST_CASE("unparseable numeric text is reported", "[dao]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "owner", "balance", "nick_name"},
                                         {{"1", "Ada", "not-a-number", std::nullopt}}));

    CHECK_THROWS_AS(DAO<Account>{fake}.load(1), db_error);
}
