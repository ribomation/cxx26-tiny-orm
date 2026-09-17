#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <tiny-orm/csv.hxx>

#include "model.hxx"

#include <sstream>
#include <string>

using Catch::Matchers::ContainsSubstring;

/// Mirrors the OpenFlights airports.dat shape: headerless, \N for null.
struct Airport {
    [[=PrimaryKey{}]] long id{};
    std::string name{};
    std::string city{};
    std::optional<std::string> iata{};
    double latitude{};
    int altitude{};
};

static auto stream(std::string text) -> std::istringstream { return std::istringstream{std::move(text)}; }

TEST_CASE("a header maps columns to members by name", "[csv]") {
    auto in = stream("id,owner,balance,nick_name\n"
                     "1,Ada,100.5,Countess\n"
                     "2,Grace,200.0,Amazing\n");
    auto const rows = read_csv<Account>(in);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == 1);
    CHECK(rows[0].ownerName == "Ada"); // the column is "owner", the member ownerName
    CHECK(rows[0].balance == 100.5);
    CHECK(rows[1].nickName == "Amazing");
}

TEST_CASE("header order does not matter", "[csv]") {
    auto in = stream("nick_name,balance,id,owner\n"
                     "Countess,100.5,1,Ada\n");
    auto const rows = read_csv<Account>(in);

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == 1);
    CHECK(rows[0].ownerName == "Ada");
    CHECK(rows[0].nickName == "Countess");
}

TEST_CASE("headers are matched trimmed and case-insensitively", "[csv]") {
    auto in = stream(" ID , Owner ,BALANCE,Nick_Name\n1,Ada,1.0,x\n");
    CHECK(read_csv<Account>(in).at(0).ownerName == "Ada");
}

TEST_CASE("columns the entity does not model are ignored", "[csv]") {
    auto in = stream("id,owner,balance,nick_name,created_at,source\n"
                     "1,Ada,100.5,Countess,2026-01-01,import\n");
    CHECK(read_csv<Account>(in).size() == 1);
}

TEST_CASE("a missing column is reported with the header that was seen", "[csv]") {
    auto in = stream("id,owner\n1,Ada\n");
    CHECK_THROWS_AS(read_csv<Account>(in), csv_error);

    auto in2 = stream("id,owner\n1,Ada\n");
    try {
        read_csv<Account>(in2);
    } catch (csv_error const& e) {
        CHECK_THAT(e.what(), ContainsSubstring("balance"));
        CHECK_THAT(e.what(), ContainsSubstring("nick_name"));
        CHECK_THAT(e.what(), ContainsSubstring("the header holds: id, owner"));
    }
}

TEST_CASE("missing columns can be allowed, leaving members at their default", "[csv]") {
    auto in = stream("id,owner\n1,Ada\n");
    auto const rows = read_csv<Account>(in, {.allow_missing_columns = true});

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].ownerName == "Ada");
    CHECK(rows[0].balance == 0.0);
    CHECK_FALSE(rows[0].nickName.has_value());
}

TEST_CASE("an empty field is NULL only where the member is optional", "[csv]") {
    auto in = stream("id,owner,balance,nick_name\n1,,0,\n");
    auto const rows = read_csv<Account>(in);

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].ownerName == "");            // NOT NULL text keeps the empty string
    CHECK_FALSE(rows[0].nickName.has_value()); // optional becomes NULL
}

TEST_CASE("a null marker overrides the type-driven rule", "[csv]") {
    auto in = stream("id,owner,balance,nick_name\n1,Ada,1.0,\\N\n");
    auto const rows = read_csv<Account>(in, {.null_marker = "\\N"});
    CHECK_FALSE(rows.at(0).nickName.has_value());
}

TEST_CASE("quoted fields carry delimiters, quotes and newlines", "[csv]") {
    auto in = stream("id,owner,balance,nick_name\n"
                     R"(1,"Lovelace, Ada",1.0,"She said ""hi""")"
                     "\n"
                     R"(2,"two)"
                     "\n"
                     R"(lines",2.0,x)"
                     "\n");
    auto const rows = read_csv<Account>(in);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].ownerName == "Lovelace, Ada");
    CHECK(rows[0].nickName == "She said \"hi\"");
    CHECK(rows[1].ownerName == "two\nlines");
}

TEST_CASE("a headerless file is read positionally", "[csv]") {
    // OpenFlights airports.dat: id, name, city, iata, latitude, altitude
    auto in = stream(R"(507,"London Heathrow","London","LHR",51.4706,83)"
                     "\n"
                     R"(3797,"John F Kennedy Intl","New York",\N,40.639,13)"
                     "\n");
    auto const rows = read_csv<Airport>(in, {.has_header = false, .null_marker = "\\N"});

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == 507);
    CHECK(rows[0].name == "London Heathrow");
    CHECK(rows[0].iata == "LHR");
    CHECK(rows[0].latitude == 51.4706);
    CHECK(rows[0].altitude == 83);
    CHECK_FALSE(rows[1].iata.has_value());
}

TEST_CASE("a bad value names the record and the column", "[csv]") {
    auto in = stream("id,owner,balance,nick_name\n"
                     "1,Ada,1.0,x\n"
                     "2,Grace,not-a-number,y\n");
    try {
        read_csv<Account>(in);
        FAIL("expected a csv_error");
    } catch (csv_error const& e) {
        CHECK_THAT(e.what(), ContainsSubstring("record 2"));
        CHECK_THAT(e.what(), ContainsSubstring("'balance'"));
        CHECK_THAT(e.what(), ContainsSubstring("not-a-number"));
    }
}

TEST_CASE("blank lines and a missing final newline are tolerated", "[csv]") {
    auto in = stream("id,owner,balance,nick_name\n1,Ada,1.0,x\n\n2,Grace,2.0,y");
    CHECK(read_csv<Account>(in).size() == 2);
}

TEST_CASE("an empty stream yields nothing", "[csv]") {
    auto in = stream("");
    CHECK(read_csv<Account>(in).empty());
}

TEST_CASE("the streaming form never accumulates", "[csv]") {
    auto in = stream("id,owner,balance,nick_name\n1,Ada,1.0,x\n2,Grace,2.0,y\n3,Alan,3.0,z\n");
    auto seen = 0;
    auto total = 0.0;
    for_each_csv<Account>(in, [&](Account&& a) { ++seen; total += a.balance; });

    CHECK(seen == 3);
    CHECK(total == 6.0);
}

// --------------------------------------------------------------- comments

TEST_CASE("comment lines are skipped, header included", "[csv]") {
    auto in = stream("# Synthetic flight data. ENU [m], velocity [m/s].\n"
                     "id,owner,balance,nick_name\n"
                     "1,Ada,1.0,x\n"
                     "# a note between rows\n"
                     "2,Grace,2.0,y\n");
    auto const rows = read_csv<Account>(in, {.comment = '#'});

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].ownerName == "Ada");
    CHECK(rows[1].ownerName == "Grace");
}

TEST_CASE("consecutive comment lines are all skipped", "[csv]") {
    auto in = stream("# one\n# two\n# three\nid,owner,balance,nick_name\n1,Ada,1.0,x\n");
    CHECK(read_csv<Account>(in, {.comment = '#'}).size() == 1);
}

TEST_CASE("a comment with unbalanced quotes does not derail the parser", "[csv]") {
    // The whole line goes before parsing starts, so the lone quote never opens a field.
    auto in = stream(R"(# units: "m/s, degrees)"
                     "\n"
                     "id,owner,balance,nick_name\n1,Ada,1.0,x\n2,Grace,2.0,y\n");
    CHECK(read_csv<Account>(in, {.comment = '#'}).size() == 2);
}

TEST_CASE("comment handling is off unless asked for", "[csv]") {
    // '#' is a legal first character of a field, so it must not be assumed.
    auto in = stream("id,owner,balance,nick_name\n1,#1 pick,1.0,x\n");
    auto const rows = read_csv<Account>(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].ownerName == "#1 pick");
}

TEST_CASE("a headerless file with a preamble reads positionally", "[csv]") {
    auto in = stream("# id, name, city, iata, lat, alt\n"
                     R"(507,"London Heathrow","London","LHR",51.4706,83)"
                     "\n");
    auto const rows = read_csv<Airport>(in, {.has_header = false, .comment = '#'});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == "London Heathrow");
}
