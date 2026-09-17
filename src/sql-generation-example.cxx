#include <tiny-orm/ddl.hxx>
#include <tiny-orm/dml.hxx>
#include <tiny-orm/psql.hxx>
#include <tiny-orm/sqlite.hxx>

#include <optional>
#include <print>
#include <string>

using namespace tiny_orm;

struct [[=Table{.name = "bank_accounts"}]] Account {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    [[=Column{.name = "owner", .nullable = Nullable::no}]] std::string ownerName{};
    double balance{};
    std::optional<std::string> nickName{};
};

struct Transaction {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    [[=ForeignKey{.target = ^^Account, .on_delete = FkAction::cascade}]] long accountId{};
    double amount{};
    std::optional<std::string> memo{};
};

void show(std::string_view label, statement const& s) {
    std::println("-- {}", label);
    std::println("{}", s.sql);
    std::print("   params: [");
    for (auto i = 0uz; i < s.params.size(); ++i) {
        if (i) std::print(", ");
        std::print("{}", s.params[i] ? std::format("'{}'", *s.params[i]) : "NULL");
    }
    std::println("]\n");
}

/// The same entity, rendered by whichever dialect is handed in.
void render_all(dialect const& sql) {
    std::println("================ {} ================\n", sql.name());

    std::println("{}\n", create_table_sql<Account>(sql));
    std::println("{}\n", create_table_sql<Transaction>(sql));

    auto const ada = Account{.id = 7, .ownerName = "Ada", .balance = 1234.5};
    show("insert", insert_object(ada, sql));
    show("update by object", update_by_object(ada, sql));
    show("delete by key", delete_by_key<Account>(7L, sql));
    show("load by key", load_by_key<Account>(7L, sql));

    constexpr auto balance = field<^^Account::balance>;
    constexpr auto owner = field<^^Account::ownerName>;

    show("select many", from<Account>(sql)
                            .where(balance > 100.0 && owner.like("A%"))
                            .order_by(balance, sort::desc)
                            .limit(10)
                            .to_sql());

    std::println("{}", drop_table_sql<Transaction>(sql));
    std::println("{}\n", drop_table_sql<Account>(sql));
}

int main() {
    render_all(psql_dialect{});
    render_all(sqlite_dialect{});
}
