#include <catch2/catch_test_macros.hpp>

#include <tiny-orm/dao.hxx>

#include "fake-connection.hxx"
#include "model.hxx"

TEST_CASE("an explicit commit issues BEGIN then COMMIT", "[tx]") {
    auto fake = fake_connection{};
    {
        auto tx = fake.begin(tx_mode::auto_rollback);
        tx.commit();
    }
    CHECK(fake.all_sql() == std::vector<std::string>{"BEGIN;", "COMMIT;"});
}

TEST_CASE("auto_rollback undoes an unfinished scope", "[tx]") {
    auto fake = fake_connection{};
    {
        auto tx = fake.begin(tx_mode::auto_rollback);
        (void) tx;
    }
    CHECK(fake.all_sql() == std::vector<std::string>{"BEGIN;", "ROLLBACK;"});
}

TEST_CASE("auto_commit commits an unfinished scope", "[tx]") {
    auto fake = fake_connection{};
    {
        auto tx = fake.begin(tx_mode::auto_commit);
        (void) tx;
    }
    CHECK(fake.all_sql() == std::vector<std::string>{"BEGIN;", "COMMIT;"});
}

TEST_CASE("an explicit rollback wins over auto_commit", "[tx]") {
    auto fake = fake_connection{};
    {
        auto tx = fake.begin(tx_mode::auto_commit);
        tx.rollback();
    }
    CHECK(fake.all_sql() == std::vector<std::string>{"BEGIN;", "ROLLBACK;"});
}

TEST_CASE("a transaction spans several DAOs on one connection", "[tx]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::affected(0));                        // BEGIN
    fake.queued.push_back(result_set::of({"id"}, {{"1"}}));                // account insert
    fake.queued.push_back(result_set::of({"id"}, {{"2"}}));                // txn insert

    auto accounts = DAO<Account>{fake};
    auto txns = DAO<Transaction>{fake};
    {
        auto tx = fake.begin(tx_mode::auto_rollback);
        auto ada = Account{.ownerName = "Ada"};
        accounts.insert(ada);
        auto t = Transaction{.accountId = ada.id, .amount = -100.0};
        txns.insert(t);
        tx.commit();
    }

    auto const sql = fake.all_sql();
    REQUIRE(sql.size() == 4);
    CHECK(sql.front() == "BEGIN;");
    CHECK(sql.back() == "COMMIT;");
}

TEST_CASE("the destructor swallows a failing rollback", "[tx]") {
    auto fake = fake_connection{};
    CHECK_NOTHROW([&] {
        auto tx = fake.begin(tx_mode::auto_rollback);
        fake.fail_next = true;
    }());
}

TEST_CASE("finishing twice is an error", "[tx]") {
    auto fake = fake_connection{};
    auto tx = fake.begin(tx_mode::auto_rollback);
    tx.commit();
    CHECK_FALSE(tx.active());
    CHECK_THROWS_AS(tx.rollback(), db_error);
}
