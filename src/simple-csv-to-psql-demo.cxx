#include <tiny-orm/connect.hxx>
#include <tiny-orm/psql.hxx>
#include <tiny-orm/annotations.hxx>
#include <tiny-orm/ddl.hxx>
#include <tiny-orm/dao.hxx>
#include <tiny-orm/csv.hxx>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <optional>
#include <vector>
#include <ranges>
#include <format>
#include <print>

namespace db = tiny_orm;
namespace fs = std::filesystem;
using std::string;
using std::optional;
using std::vector;

// -- entity type --
struct [[= db::Table{} ]] Person {
    //id,first_name,last_name,email,gender,street,city,country
    [[=db::PrimaryKey{.auto_increment = true}]]
    long id{};
    string first_name{};
    string last_name{};
    int age{};
    string email{};
    optional<string> street{};
    optional<string> city{};
    string country{};
};

template<>
struct std::formatter<Person> : std::formatter<string> {
    auto format(Person const& p, auto& ctx) const {
        return std::formatter<string>::format(
            std::format("Person(id={}, name={} {}, age={}, email={}, address={}, {}, {})",
                        p.id,
                        p.first_name, p.last_name, p.age, p.email,
                        p.street.value_or("-"), p.city.value_or("-"), p.country),
            ctx);
    }
};


// -- cleanup --
template<typename F>
struct scope_exit {
    F fn;
    ~scope_exit() { fn(); }
};


// -- JSON --
template<typename>
inline constexpr auto is_optional = false;
template<typename T>
inline constexpr auto is_optional<std::optional<T>> = true;

template<typename T>
static auto as_json(T const& e) -> nlohmann::json {
    auto j = nlohmann::json::object();
    template for (constexpr auto m: db::detail::members_of<T>()) {
        auto const& v = e.[:m:];
        auto key = std::string{std::meta::identifier_of(m)};
        if constexpr (is_optional<std::remove_cvref_t<decltype(v)>>)
            j[key] = v ? nlohmann::json(*v) : nlohmann::json(nullptr);
        else
            j[key] = v;
    }
    return j;
}


int main(int argc, char* argv[]) {
    try {
        auto csv_file = fs::path{argc > 2 ? argv[2] : "data/person-data.csv"};
        if (not fs::exists(csv_file)) {
            std::println(stderr, "cannot find CSV filer {}", csv_file);
            return 1;
        }

        auto csv_opts = db::csv_options{.delimiter = ',', .has_header = true};
        auto lines = db::read_csv_file<Person>(csv_file, csv_opts);
        std::println("read {} lines from {}", lines.size(), csv_file);

        auto const N = 5;
        std::println("-- first {} Person objects --", N);
        for (auto cnt = 1; auto&& p: lines | std::views::take(N))
            std::println("{}) {}", cnt++, p);

        constexpr auto URL = "postgresql://orm:orm@localhost:5432/tiny_orm_test";
        auto conn = db::connect(URL);
        auto dao = db::DAO<Person>{*conn};
        auto guard = scope_exit{[&] noexcept {
            try { dao.drop(); } catch (...) {}
        }};

        dao.create();
        dao.insert_all(lines);

        std::println("-- query result: age 20 living in Denmark, as JSON --");
        constexpr auto age = db::field<^^Person::age>;
        constexpr auto country = db::field<^^Person::country>;
        auto result = dao.select()
                .where(age == 20)
                .where(country == "Denmark")
                .order_by(country)
                .all();

        auto json = nlohmann::json::array();
        for (auto&& p: result) json.push_back(as_json(p));
        std::println("{}", json.dump(2));
    } catch (std::exception const& e) {
        std::println(stderr, "\nfailed: {}", e.what());
        return 1;
    }
}
