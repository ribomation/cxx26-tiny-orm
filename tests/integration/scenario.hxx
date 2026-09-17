#pragma once

/**
 * The scenarios both backends must pass, written once.
 *
 * Everything here talks only to `tiny_orm::connection`, so the same assertions
 * run against SQLite and PostgreSQL. Anything that behaves differently between
 * the two belongs in the dialect, not in a test.
 */

#include <catch2/catch_test_macros.hpp>

#include <tiny-orm/csv.hxx>
#include <tiny-orm/dao.hxx>

#include "model.hxx"

#include <format>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

/**
 * Drops the scenario's tables however the section ends, a failed assertion
 * included.
 *
 * Catch2 re-runs the whole function body once per SECTION, so plain statements
 * after the sections would still be skipped when an assertion throws -- and the
 * integration database is shared, so anything left behind leaks into the next
 * run and into whatever else is using that database.
 */
class scenario_cleanup {
    tiny_orm::connection* db_;

public:
    explicit scenario_cleanup(tiny_orm::connection& db) : db_{&db} {}
    scenario_cleanup(scenario_cleanup const&) = delete;
    auto operator=(scenario_cleanup const&) -> scenario_cleanup& = delete;

    ~scenario_cleanup() {
        try {
            DAO<Transaction>{*db_}.drop(); // the dependent table first
            DAO<Account>{*db_}.drop();
        } catch (...) { // NOLINT: a destructor must not propagate
        }
    }
};

inline void run_crud_scenario(tiny_orm::connection& db) {
    auto accounts = DAO<Account>{db};
    auto txns = DAO<Transaction>{db};

    // Belt and braces: the guard below cleans up, but a previous run that was
    // killed outright would leave tables behind.
    txns.drop();
    accounts.drop();
    accounts.create();
    txns.create();
    auto const cleanup = scenario_cleanup{db};

    SECTION("insert assigns a generated key") {
        auto ada = Account{.ownerName = "Ada Lovelace", .balance = 1000.0};
        accounts.insert(ada);
        CHECK(ada.id > 0);

        auto grace = Account{.ownerName = "Grace Hopper", .balance = 2500.0,
                             .nickName = "Amazing Grace"};
        accounts.insert(grace);
        CHECK(grace.id != ada.id);
    }

    SECTION("a round trip preserves every field, NULL included") {
        auto ada = Account{.ownerName = "Ada", .balance = 1234.5};
        accounts.insert(ada);

        auto const back = accounts.load(ada.id);
        REQUIRE(back.has_value());
        CHECK(back->id == ada.id);
        CHECK(back->ownerName == "Ada");
        CHECK(back->balance == 1234.5);
        CHECK_FALSE(back->nickName.has_value()); // NULL, not ""
    }

    SECTION("a non-NULL optional survives the round trip") {
        auto g = Account{.ownerName = "Grace", .balance = 1.0, .nickName = "Amazing"};
        accounts.insert(g);
        auto const back = accounts.load(g.id);
        REQUIRE(back.has_value());
        REQUIRE(back->nickName.has_value());
        CHECK(*back->nickName == "Amazing");
    }

    SECTION("update reports what it changed") {
        auto ada = Account{.ownerName = "Ada", .balance = 10.0};
        accounts.insert(ada);

        ada.balance = 99.5;
        CHECK(accounts.update(ada) == 1);
        CHECK(accounts.load(ada.id)->balance == 99.5);

        auto ghost = Account{.id = 999999, .ownerName = "nobody"};
        CHECK(accounts.update(ghost) == 0);
    }

    SECTION("remove reports what it deleted") {
        auto ada = Account{.ownerName = "Ada", .balance = 10.0};
        accounts.insert(ada);
        CHECK(accounts.remove(ada.id) == 1);
        CHECK(accounts.remove(ada.id) == 0);
        CHECK_FALSE(accounts.load(ada.id).has_value());
    }

    SECTION("load of a missing key yields nothing rather than throwing") {
        CHECK_FALSE(accounts.load(123456).has_value());
    }

    SECTION("queries filter, order and page") {
        for (auto const& [owner, balance] : {std::pair{"Ada", 100.0}, {"Grace", 200.0},
                                             {"Alan", 300.0}, {"Barbara", 400.0}}) {
            auto a = Account{.ownerName = owner, .balance = balance};
            accounts.insert(a);
        }
        constexpr auto balance = field<^^Account::balance>;
        constexpr auto owner = field<^^Account::ownerName>;

        CHECK(accounts.count() == 4);
        CHECK(accounts.select().where(balance > 150.0).count() == 3);

        auto const rich = accounts.select().where(balance >= 200.0).order_by(balance, sort::desc).all();
        REQUIRE(rich.size() == 3);
        CHECK(rich[0].ownerName == "Barbara");
        CHECK(rich[2].ownerName == "Grace");

        auto const paged = accounts.select().order_by(balance, sort::asc).limit(2).offset(1).all();
        REQUIRE(paged.size() == 2);
        CHECK(paged[0].ownerName == "Grace");

        auto const a_names = accounts.select().where(owner.like("A%")).order_by(owner).all();
        REQUIRE(a_names.size() == 2);
        CHECK(a_names[0].ownerName == "Ada");

        CHECK(accounts.select().where(owner.in({"Ada", "Grace"})).count() == 2);
        CHECK(accounts.select().where(field<^^Account::nickName>.is_null()).count() == 4);

        auto const first = accounts.select().order_by(balance, sort::desc).first();
        REQUIRE(first.has_value());
        CHECK(first->ownerName == "Barbara");
    }

    SECTION("exists answers without materialising") {
        auto ada = Account{.ownerName = "Ada", .balance = 10.0};
        accounts.insert(ada);
        CHECK(accounts.exists(ada.id));
        CHECK_FALSE(accounts.exists(999999));
    }

    SECTION("a committed transaction persists") {
        {
            auto tx = db.begin(tiny_orm::tx_mode::auto_rollback);
            auto a = Account{.ownerName = "Committed", .balance = 1.0};
            accounts.insert(a);
            tx.commit();
        }
        CHECK(accounts.select().where(field<^^Account::ownerName> == "Committed").count() == 1);
    }

    SECTION("an abandoned transaction rolls back") {
        {
            auto tx = db.begin(tiny_orm::tx_mode::auto_rollback);
            auto a = Account{.ownerName = "Abandoned", .balance = 1.0};
            accounts.insert(a);
        } // no commit
        CHECK(accounts.select().where(field<^^Account::ownerName> == "Abandoned").count() == 0);
    }

    SECTION("an explicit rollback discards the work") {
        {
            auto tx = db.begin(tiny_orm::tx_mode::auto_commit);
            auto a = Account{.ownerName = "Undone", .balance = 1.0};
            accounts.insert(a);
            tx.rollback();
        }
        CHECK(accounts.select().where(field<^^Account::ownerName> == "Undone").count() == 0);
    }

    SECTION("bulk insert loads many rows and writes every key back") {
        auto rows = std::vector<Account>{};
        for (auto i = 0; i < 2500; ++i)
            rows.push_back(Account{.ownerName = std::format("owner{}", i),
                                   .balance = double(i),
                                   .nickName = (i % 3 == 0) ? std::optional<std::string>{}
                                                            : std::format("nick{}", i)});

        auto const inserted = accounts.insert_all(rows, {.batch_size = 400});
        CHECK(inserted == 2500);
        CHECK(accounts.count() == 2500);

        // Keys come back in VALUES order -- the assumption insert_all documents.
        for (auto i = 0uz; i < rows.size(); ++i) {
            REQUIRE(rows[i].id > 0);
            if (i > 0) CHECK(rows[i].id > rows[i - 1].id);
        }

        // And the row a key names is the row that produced it.
        auto const back = accounts.load(rows[1234].id);
        REQUIRE(back.has_value());
        CHECK(back->ownerName == "owner1234");
        CHECK(back->nickName == rows[1234].nickName); // 1234 % 3 != 0, so it is set

        // ...and a row whose optional was left empty comes back empty.
        auto const null_row = accounts.load(rows[1233].id); // 1233 % 3 == 0
        REQUIRE(null_row.has_value());
        CHECK_FALSE(null_row->nickName.has_value());
    }

    SECTION("bulk insert clamps the batch to the engine's parameter budget") {
        auto rows = std::vector<Account>{};
        for (auto i = 0; i < 1200; ++i)
            rows.push_back(Account{.ownerName = std::format("o{}", i), .balance = double(i)});

        // Far more than any engine accepts in one statement; must still succeed.
        CHECK(accounts.insert_all(rows, {.batch_size = 1'000'000}) == 1200);
        CHECK(accounts.count() == 1200);
    }

    SECTION("select_into runs an aggregate the query builder cannot express") {
        auto rows = std::vector<Account>{};
        for (auto const& [owner, balance] : {std::pair{"Ada", 100.0}, {"Ada", 300.0},
                                             {"Grace", 200.0}, {"Grace", 600.0},
                                             {"Alan", 50.0}}) {
            rows.push_back(Account{.ownerName = owner, .balance = balance});
        }
        accounts.insert_all(rows);

        struct OwnerTotal {
            std::string owner{};
            long accounts{};
            double totalBalance{}; // -> total_balance
        };
        auto const totals = select_into<OwnerTotal>(db, R"(
            SELECT "owner"           AS owner,
                   count(*)          AS accounts,
                   sum("balance")    AS total_balance
            FROM bank_accounts
            GROUP BY "owner"
            HAVING count(*) > 1
            ORDER BY total_balance DESC)");

        REQUIRE(totals.size() == 2);
        CHECK(totals[0].owner == "Grace");
        CHECK(totals[0].accounts == 2);
        CHECK(totals[0].totalBalance == 800.0);
        CHECK(totals[1].owner == "Ada");
        CHECK(totals[1].totalBalance == 400.0);
    }

    SECTION("a CSV loads straight into the database") {
        // The same descriptor drives all three steps: it emitted the CREATE TABLE,
        // it matched these header columns to members, and it built the INSERT.
        auto csv = std::istringstream{
            "id,owner,balance,nick_name\n"
            "1,Ada Lovelace,1000.50,Countess\n"
            "2,Grace Hopper,2500.00,\\N\n"
            "3,\"Turing, Alan\",750.25,\n"};

        auto rows = read_csv<Account>(csv, {.null_marker = "\\N"});
        REQUIRE(rows.size() == 3);

        accounts.insert_all(rows);
        REQUIRE(accounts.count() == 3);

        auto const alan = accounts.select()
                              .where(field<^^Account::ownerName> == "Turing, Alan")
                              .first();
        REQUIRE(alan.has_value());
        CHECK(alan->balance == 750.25);
        CHECK_FALSE(alan->nickName.has_value()); // empty field, optional member

        // Both null spellings survived the round trip through the database.
        CHECK(accounts.select().where(field<^^Account::nickName>.is_null()).count() == 2);

        struct OwnerRow {
            std::string owner{};
            double balance{};
        };
        auto const top = select_into<OwnerRow>(db, R"(
            SELECT "owner" AS owner, "balance" AS balance
            FROM bank_accounts ORDER BY "balance" DESC LIMIT 1)");
        REQUIRE(top.size() == 1);
        CHECK(top[0].owner == "Grace Hopper");
    }

    SECTION("timestamps survive the round trip through the engine") {
        auto stamps = DAO<Reading>{db};
        stamps.drop();
        stamps.create();

        auto const moment = std::chrono::sys_days{std::chrono::August / 3 / 2026}
                            + std::chrono::hours{14} + std::chrono::minutes{7}
                            + std::chrono::seconds{9} + std::chrono::microseconds{654321};

        auto rows = std::vector<Reading>{
            Reading{.takenAt = moment, .settledAt = {}, .value = 1.5},
            Reading{.takenAt = moment + std::chrono::hours{1},
                    .settledAt = moment + std::chrono::hours{2},
                    .value = 2.5}};
        stamps.insert_all(rows);

        auto const back = stamps.load(rows[0].id);
        REQUIRE(back.has_value());
        CHECK(back->takenAt == moment); // microseconds intact
        CHECK_FALSE(back->settledAt.has_value());

        auto const second = stamps.load(rows[1].id);
        REQUIRE(second.has_value());
        REQUIRE(second->settledAt.has_value());
        CHECK(*second->settledAt == moment + std::chrono::hours{2});

        // Ordering and comparison work on the stored representation, which for
        // SQLite is ISO-8601 text -- lexicographic order matches chronological.
        constexpr auto taken = field<^^Reading::takenAt>;
        auto const later = stamps.select().where(taken > moment).all();
        CHECK(later.size() == 1);

        auto const newest = stamps.select().order_by(taken, sort::desc).first();
        REQUIRE(newest.has_value());
        CHECK(newest->value == 2.5);

        stamps.drop();
    }

    SECTION("a boolean survives the round trip and is still truthy in raw SQL") {
        auto flags = DAO<Switch>{db};
        flags.drop();
        flags.create();

        auto on = Switch{.label = "on", .active = true};
        auto off = Switch{.label = "off", .active = false};
        flags.insert(on);
        flags.insert(off);

        auto const back = flags.load(on.id);
        REQUIRE(back.has_value());
        CHECK(back->active);

        // Regression, and the reason parameters bind "1"/"0" rather than
        // "true"/"false": the round trip above passed either way, because the
        // text went out and came back through from_cell<bool>. What broke was
        // asking the *engine* to read the column as a boolean. SQLite's INTEGER
        // affinity cannot convert "true", so it stored it as text, and a
        // non-numeric string is false -- silently, for every row.
        struct Count {
            long n{};
        };
        auto const truthy = select_into<Count>(db, R"(SELECT count(*) AS n FROM "switches" WHERE "active")");
        REQUIRE(truthy.size() == 1);
        CHECK(truthy.front().n == 1);

        flags.drop();
    }

    SECTION("a foreign key ties the two tables together") {
        auto ada = Account{.ownerName = "Ada", .balance = 500.0};
        accounts.insert(ada);

        auto coffee = Transaction{.accountId = ada.id, .amount = -100.0, .memo = "coffee"};
        txns.insert(coffee);
        CHECK(coffee.id > 0);
        CHECK(txns.count() == 1);

        // ON DELETE CASCADE removes the dependent row
        CHECK(accounts.remove(ada.id) == 1);
        CHECK(txns.count() == 0);
    }
}
