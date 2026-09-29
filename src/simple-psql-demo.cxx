#include <tiny-orm/connect.hxx>
#include <tiny-orm/psql.hxx>
#include <tiny-orm/annotations.hxx>
#include <tiny-orm/ddl.hxx>
#include <tiny-orm/dao.hxx>
#include <print>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace db = tiny_orm;
using std::string;
using std::optional;
using std::vector;


struct [[= db::Table{} ]] Person {
    [[=db::PrimaryKey{.auto_increment = true}]]
    long id{};
    [[= db::Column{.sql_type = "VARCHAR(32)"} ]]
    string name{};
    int age{};
    [[= db::Column{.default_value = "true"} ]]
    optional<bool> female{};
    optional<string> address{};
};

template<>
struct std::formatter<Person> : std::formatter<string> {
    auto format(Person const& p, auto& ctx) const {
        return std::formatter<string>::format(
            std::format("Person{{id={}, name={}, age={}, female={}, address={}}}",
                        p.id, p.name, p.age,
                        p.female ? (*p.female ? "yes" : "no") : "unknown",
                        p.address ? *p.address : "none"),
            ctx);
    }
};


int main() {
    try {
        constexpr auto URL = "postgresql://orm:orm@localhost:5432/tiny_orm_test";
        auto conn = db::connect(URL);
        std::println("-- Define table\n{}", db::create_table_sql<Person>(conn->sql_dialect()));

        auto dao = db::DAO<Person>{*conn};
        dao.create();

        auto anna = Person{.name = "Anna Conda", .age = 23, .female = true, .address = "17 Hacker Lane"};
        dao.insert(anna);
        auto const obj = dao.load(anna.id);
        std::println("obj: {}", *obj);
        dao.remove(anna.id);

        auto data = vector<Person>{
            {.name = "Per Silja", .age = 43, .address = "42 Cobol Blvd."},
            {.name = "Inge Vidare", .age = 53},
            {.name = "Åke R. Vidare", .age = 67, .female=false, .address="123 Perl Lane"},
            {.name = "Justin Time", .age = 37, .female = false},
            {.name = "Cata Logue", .age = 49, .female = true, .address="1 Rust Conduit"},
        };
        dao.insert_all(data);

        std::println("-- Query Result --");
        constexpr auto age = db::field<^^Person::age>;
        constexpr auto name = db::field<^^Person::name>;
        auto const result = dao.select()
                .where(age >= 40)
                .order_by(name)
                .all();
        auto cnt=1;
        for (auto&& p: result) std::println("{}: {}", cnt++, p);

        dao.drop();
    } catch (db::db_error const& x) {
        std::println("ERR: {}", x.what());
    } catch (...) {
        std::println("Unknown error");
    }
}
