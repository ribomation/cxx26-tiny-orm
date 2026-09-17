#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <tiny-orm/dml.hxx>
#include <tiny-orm/psql.hxx>

#include "model.hxx"

using Catch::Matchers::ContainsSubstring;

static psql_dialect const pg{};

TEST_CASE("INSERT omits generated keys and returns them", "[dml]") {
    auto const s = insert_object(Account{.id = 99, .ownerName = "Ada", .balance = 10.0}, pg);
    CHECK(s.sql == R"(INSERT INTO "bank_accounts" ("owner", "balance", "nick_name") )"
                   R"(VALUES ($1, $2, $3) RETURNING "id";)");
    REQUIRE(s.params.size() == 3); // id is not bound
    CHECK(s.params[0] == "Ada");
    CHECK_FALSE(s.params[2].has_value()); // nullopt member -> SQL NULL
}

TEST_CASE("INSERT binds a non-generated key", "[dml]") {
    auto const s = insert_object(Ledger{.id = 5, .label = "petty cash"}, pg);
    CHECK_THAT(s.sql, ContainsSubstring(R"(("id", "label"))"));
    REQUIRE(s.params.size() == 2);
    CHECK(s.params[0] == "5");
}

TEST_CASE("UPDATE sets every non-key column and binds the key last", "[dml]") {
    auto const s = update_by_object(Account{.id = 7, .ownerName = "Ada", .balance = 10.0}, pg);
    CHECK(s.sql == R"(UPDATE "bank_accounts" SET "owner" = $1, "balance" = $2, )"
                   R"("nick_name" = $3 WHERE "id" = $4;)");
    REQUIRE(s.params.size() == 4);
    CHECK(s.params.back() == "7");
}

TEST_CASE("DELETE and load by key take a single parameter", "[dml]") {
    auto const del = delete_by_key<Account>(7L, pg);
    CHECK(del.sql == R"(DELETE FROM "bank_accounts" WHERE "id" = $1;)");
    CHECK(del.params == std::vector<param>{"7"});

    auto const sel = load_by_key<Account>(7L, pg);
    CHECK_THAT(sel.sql, ContainsSubstring(R"(SELECT "id", "owner", "balance", "nick_name")"));
    CHECK_THAT(sel.sql, ContainsSubstring(R"(WHERE "id" = $1)"));
}

TEST_CASE("predicates compose and number their placeholders in order", "[dml]") {
    constexpr auto balance = field<^^Account::balance>;
    constexpr auto owner = field<^^Account::ownerName>;

    auto const s = from<Account>(pg)
                       .where(balance > 100.0 && owner.like("A%"))
                       .order_by(balance, sort::desc)
                       .limit(10)
                       .to_sql();

    CHECK_THAT(s.sql, ContainsSubstring(R"(WHERE ("balance" > $1 AND "owner" LIKE $2))"));
    CHECK_THAT(s.sql, ContainsSubstring(R"(ORDER BY "balance" DESC)"));
    CHECK_THAT(s.sql, ContainsSubstring("LIMIT $3"));
    CHECK(s.params == std::vector<param>{"100", "A%", "10"});
}

TEST_CASE("IS NULL and IN need no value and many values", "[dml]") {
    constexpr auto nick = field<^^Account::nickName>;
    constexpr auto owner = field<^^Account::ownerName>;

    auto const s = from<Account>(pg).where(nick.is_null() || owner.in({"Ada", "Grace"})).to_sql();
    CHECK_THAT(s.sql, ContainsSubstring(R"(("nick_name" IS NULL OR "owner" IN ($1, $2)))"));
    CHECK(s.params.size() == 2);
}

TEST_CASE("repeated where clauses are ANDed", "[dml]") {
    constexpr auto balance = field<^^Account::balance>;
    auto q = from<Account>(pg);
    q.where(balance > 1.0);
    q.where(balance < 100.0);
    CHECK_THAT(q.to_sql().sql, ContainsSubstring(R"(("balance" > $1 AND "balance" < $2))"));
}

TEST_CASE("counting ignores ordering and paging", "[dml]") {
    constexpr auto balance = field<^^Account::balance>;
    auto const s = from<Account>(pg).where(balance > 100.0).order_by(balance).limit(5).count_sql();
    CHECK(s.sql == R"(SELECT COUNT(*) FROM "bank_accounts" WHERE "balance" > $1;)");
    CHECK(s.params == std::vector<param>{"100"});
}

TEST_CASE("an unbound query refuses to execute", "[dml]") {
    CHECK_THROWS_AS(from<Account>(pg).all(), db_error);
}
