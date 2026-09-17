#include <catch2/catch_test_macros.hpp>

#include <tiny-orm/schema.hxx>

#include "model.hxx"

TEST_CASE("table names derive from the type identifier", "[schema]") {
    STATIC_REQUIRE(table_name_of<Transaction>() == compiletime_string{"transactions"});
    STATIC_REQUIRE(table_name_of<Category>() == compiletime_string{"categories"});
    STATIC_REQUIRE(table_name_of<Address>() == compiletime_string{"addresses"});
}

TEST_CASE("a Table annotation overrides the derived name", "[schema]") {
    STATIC_REQUIRE(table_name_of<Account>() == compiletime_string{"bank_accounts"});
}

TEST_CASE("column names are snake_case unless overridden", "[schema]") {
    constexpr auto cols = columns_of<Account>();
    STATIC_REQUIRE(cols.size() == 4);
    CHECK(cols[0].name.view() == "id");
    CHECK(cols[1].name.view() == "owner");     // overridden from ownerName
    CHECK(cols[2].name.view() == "balance");
    CHECK(cols[3].name.view() == "nick_name"); // derived from nickName
}

TEST_CASE("canonical type kinds come from the C++ member types", "[schema]") {
    constexpr auto cols = columns_of<Account>();
    STATIC_REQUIRE(cols[0].kind == type_kind::int64);
    STATIC_REQUIRE(cols[1].kind == type_kind::text);
    STATIC_REQUIRE(cols[2].kind == type_kind::float64);
    STATIC_REQUIRE(cols[3].kind == type_kind::text); // optional unwrapped
}

TEST_CASE("no raw sql_type override unless one was given", "[schema]") {
    constexpr auto cols = columns_of<Account>();
    for (auto const& c : cols) CHECK(c.sql_type.empty());
}

TEST_CASE("nullability derives from std::optional", "[schema]") {
    constexpr auto cols = columns_of<Account>();
    CHECK_FALSE(cols[2].nullable); // double        -> NOT NULL
    CHECK(cols[3].nullable);       // optional<...> -> nullable
}

TEST_CASE("a primary key is never nullable and carries its flags", "[schema]") {
    constexpr auto cols = columns_of<Account>();
    CHECK(cols[0].primary_key);
    CHECK(cols[0].auto_increment);
    CHECK_FALSE(cols[0].nullable);
}

TEST_CASE("a foreign key resolves through the target entity's own table name", "[schema]") {
    constexpr auto cols = columns_of<Transaction>();
    CHECK(cols[1].references_table.view() == "bank_accounts");
    CHECK(cols[1].references_column.view() == "id");
    CHECK(cols[1].on_delete == FkAction::cascade);
}
