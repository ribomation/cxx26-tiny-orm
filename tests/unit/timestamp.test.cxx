#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <tiny-orm/dao.hxx>
#include <tiny-orm/psql.hxx>
#include <tiny-orm/sqlite.hxx>

#include "fake-connection.hxx"

#include <chrono>
#include <optional>
#include <string>

using namespace tiny_orm;
using Catch::Matchers::ContainsSubstring;
using namespace std::chrono;

static psql_dialect const pg{};
static sqlite_dialect const lite{};

struct [[=Table{.name = "samples"}]] Sample {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    system_clock::time_point recordedAt{};
    std::optional<system_clock::time_point> processedAt{};
    double value{};
};

/// A coarser precision, to show the kind is matched structurally.
struct [[=Table{.name = "days"}]] Day {
    [[=PrimaryKey{}]] long id{};
    sys_seconds startedAt{};
};

static constexpr auto moment =
    sys_days{2026y / September / 14} + 13h + 21min + 55s + 123456us;

TEST_CASE("a chrono time_point is a timestamp column", "[timestamp]") {
    constexpr auto cols = columns_of<Sample>();
    STATIC_REQUIRE(cols[1].kind == type_kind::timestamp);
    STATIC_REQUIRE(cols[2].kind == type_kind::timestamp); // optional unwrapped
    CHECK_FALSE(cols[1].nullable);
    CHECK(cols[2].nullable);
}

TEST_CASE("the kind is matched structurally, not by one spelling", "[timestamp]") {
    constexpr auto cols = columns_of<Day>();
    STATIC_REQUIRE(cols[1].kind == type_kind::timestamp); // sys_seconds, not time_point
}

TEST_CASE("each engine renders the kind its own way", "[timestamp]") {
    CHECK(pg.type_name(type_kind::timestamp) == "TIMESTAMP");
    CHECK(lite.type_name(type_kind::timestamp) == "TEXT"); // SQLite has no date type

    CHECK_THAT(create_table_sql<Sample>(pg), ContainsSubstring(R"("recorded_at" TIMESTAMP NOT NULL)"));
    CHECK_THAT(create_table_sql<Sample>(lite), ContainsSubstring(R"("recorded_at" TEXT NOT NULL)"));
}

TEST_CASE("a timestamp binds as microsecond ISO text", "[timestamp]") {
    auto const s = insert_object(Sample{.recordedAt = moment, .value = 1.0}, pg);
    REQUIRE(s.params.size() == 3);
    CHECK(s.params[0] == "2026-09-14 13:21:55.123456");
    CHECK_FALSE(s.params[1].has_value()); // the optional was empty
}

TEST_CASE("a timestamp round-trips through a result set", "[timestamp]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "recorded_at", "processed_at", "value"},
                                         {{"1", "2026-09-14 13:21:55.123456", std::nullopt, "2.5"}}));

    auto const row = DAO<Sample>{fake}.load(1);

    REQUIRE(row.has_value());
    CHECK(row->recordedAt == moment);
    CHECK_FALSE(row->processedAt.has_value());
}

TEST_CASE("both separators parse, and a missing fraction is zero", "[timestamp]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "recorded_at", "processed_at", "value"},
                                         {{"1", "2026-09-14T13:21:55", "2026-09-14 13:21:55", "0"}}));

    auto const row = DAO<Sample>{fake}.load(1);

    REQUIRE(row.has_value());
    CHECK(row->recordedAt == sys_days{2026y / September / 14} + 13h + 21min + 55s);
    REQUIRE(row->processedAt.has_value());
    CHECK(*row->processedAt == row->recordedAt);
}

TEST_CASE("unparseable timestamp text is reported", "[timestamp]") {
    auto fake = fake_connection{};
    fake.queued.push_back(result_set::of({"id", "recorded_at", "processed_at", "value"},
                                         {{"1", "not-a-date", std::nullopt, "0"}}));
    CHECK_THROWS_AS(DAO<Sample>{fake}.load(1), db_error);
}
