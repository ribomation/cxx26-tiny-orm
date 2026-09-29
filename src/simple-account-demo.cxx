#include <tiny-orm/schema.hxx>
#include <tiny-orm/ddl.hxx>
#include <tiny-orm/dml.hxx>
#include <tiny-orm/psql.hxx>

#include <optional>
#include <print>
#include <string>

namespace orm = tiny_orm;
using std::optional;
using std::string;
using namespace std::string_literals;


struct [[=orm::Table{}]] Account {
    [[=orm::PrimaryKey{.auto_increment = true}]]
    long id{};

    [[=orm::Column{.sql_type = "CHAR(32)"}]]
    string iban{};

    [[=orm::Column{.nullable = orm::Nullable::yes, .default_value = "0.0"}]]
    double balance{};

    optional<string> nickname{};
};

void show_sql(std::string_view label, orm::statement const& stmt) {
    std::println("\n-- {}", label);
    std::println("{}", stmt.sql);
    std::print("   params: [");
    for (auto k = 0uz; k < stmt.params.size(); ++k) {
        if (k > 0uz) std::print(", ");
        auto p = stmt.params[k];
        if (p) {
            std::print("'{}'", *p);
        } else {
            std::print("NULL");
        }
    }
    std::println("]");
}

int main() {
    auto dialect = orm::psql_dialect{};

    std::println("-- Define table\n{}", orm::create_table_sql<Account>(dialect));

    auto acc = Account{.iban = "ABC-1234-5678"s, .balance = 1500.0, .nickname = "vacation"s};
    show_sql("Create", orm::insert_object(acc, dialect));

    auto pk = 123L;
    show_sql("Read", orm::load_by_key<Account>(pk, dialect));

    acc.balance *= 10;
    acc.nickname = "vehicle"s;
    show_sql("Update", orm::update_by_key(pk, acc, dialect));

    constexpr auto balance = orm::field<^^Account::balance>;
    show_sql("Query", orm::from<Account>(dialect)
             .where(balance > 1000.0)
             .order_by(balance, orm::sort::desc)
             .limit(5)
             .to_sql()
    );

    show_sql("Delete", orm::delete_by_key<Account>(pk, dialect));

    std::println("\n-- Erase table\n{}", orm::drop_table_sql<Account>(dialect));
}
