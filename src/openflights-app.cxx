/**
 * Loads the OpenFlights reference data into a database.
 *
 *   ./scripts/fetch-openflights.sh
 *   ./build/openflights-demo [connection-url] [data-dir]
 *
 * Defaults to the container in docker/docker-compose.yml. Any URL `connect`
 * understands works, so `sqlite::memory:` runs the whole thing without a server.
 *
 * Data (c) OpenFlights contributors, Open Database License (ODbL).
 */

#include <tiny-orm/connect.hxx>
#include <tiny-orm/csv.hxx>
#include <tiny-orm/dao.hxx>

#include "openflights-schema.hxx"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

    using clock_type = std::chrono::steady_clock;

    auto ms_since(clock_type::time_point start) -> long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start)
            .count();
    }

    /// Headerless, backslash-N for null -- the OpenFlights convention.
    constexpr auto dat_format = csv_options{.has_header = false, .null_marker = "\\N"};

    /// Reads one .dat file and reports what it cost.
    template<typename T>
    auto read(std::filesystem::path const& dir, std::string_view file) -> std::vector<T> {
        auto const start = clock_type::now();
        auto rows = read_csv_file<T>(dir / file, dat_format);
        std::println("  read  {:<14} {:>6} rows  {:>5} ms", file, rows.size(), ms_since(start));
        return rows;
    }

    /// Bulk loads one table and reports what it cost.
    template<typename T>
    void load(connection& db, std::vector<T>& rows, std::string_view what) {
        auto dao = DAO<T>{db};
        auto const start = clock_type::now();
        auto tx = db.begin(tx_mode::auto_rollback);
        auto const n = dao.insert_all(rows);
        tx.commit();
        std::println("  load  {:<14} {:>6} rows  {:>5} ms", what, n, ms_since(start));
    }

} // namespace

int main(int argc, char* argv[]) {
    auto const url = std::string{argc > 1 ? argv[1]
                                          : "postgresql://orm:orm@localhost:5432/tiny_orm_demo"};
    auto const dir = std::filesystem::path{argc > 2 ? argv[2] : "data/openflights"};

    if (!std::filesystem::exists(dir / "airports.dat")) {
        std::println(stderr, "no data in {} -- run ./scripts/fetch-openflights.sh first",
                     dir.string());
        return 1;
    }

    try {
        auto db = connect(url);
        std::println("connected to {} ({})\n", url, db->sql_dialect().name());

        // ---- schema. Dropped children first, created parents first. -------
        auto routes_dao = DAO<Route>{*db};
        auto planes_dao = DAO<Plane>{*db};
        auto airlines_dao = DAO<Airline>{*db};
        auto airports_dao = DAO<Airport>{*db};

        routes_dao.drop({.cascade = true});
        planes_dao.drop({.cascade = true});
        airlines_dao.drop({.cascade = true});
        airports_dao.drop({.cascade = true});

        airports_dao.create();
        airlines_dao.create();
        planes_dao.create();
        routes_dao.create();
        std::println("schema created from the structs\n");

        // ---- reference tables --------------------------------------------
        auto airports = read<Airport>(dir, "airports.dat");
        auto airlines = read<Airline>(dir, "airlines.dat");
        auto planes = read<Plane>(dir, "planes.dat");
        auto routes = read<Route>(dir, "routes.dat");
        std::println("");

        load(*db, airports, "airports");
        load(*db, airlines, "airlines");
        load(*db, planes, "planes");

        // ---- routes, minus the rows the foreign keys would reject ---------
        // Real data is not referentially clean: some routes name airports that
        // airports.dat does not contain. Filtering here keeps the constraints
        // rather than dropping them to make the load succeed.
        auto airport_ids = std::unordered_set<long>{};
        for (auto const& a : airports) airport_ids.insert(a.id);
        auto airline_ids = std::unordered_set<long>{};
        for (auto const& a : airlines) airline_ids.insert(a.id);

        auto const before = routes.size();
        auto loadable = std::vector<Route>{};
        loadable.reserve(before);
        for (auto& r : routes) {
            auto const ok = r.airlineId && airline_ids.contains(*r.airlineId)
                            && r.sourceAirportId && airport_ids.contains(*r.sourceAirportId)
                            && r.destAirportId && airport_ids.contains(*r.destAirportId);
            if (ok) loadable.push_back(std::move(r));
        }
        auto const dropped = before - loadable.size();
        std::println("  skip  {:<14} {:>6} routes with dangling references ({:.1f}%)", "",
                     dropped, 100.0 * double(dropped) / double(before));
        load(*db, loadable, "routes");

        std::println("\ntables: {} airports, {} airlines, {} planes, {} routes\n",
                     airports_dao.count(), airlines_dao.count(), planes_dao.count(),
                     routes_dao.count());

        // ---- questions the query builder cannot ask -----------------------
        struct Busiest {
            std::string airport{};
            std::string country{};
            long departures{};
            long destinations{};
        };
        std::println("Busiest airports by departing routes");
        for (auto const& r : select_into<Busiest>(*db, R"(
                SELECT a.name                          AS airport,
                       a.country                       AS country,
                       count(*)                        AS departures,
                       count(DISTINCT r.dest_airport_id) AS destinations
                FROM routes r
                JOIN airports a ON a.id = r.source_airport_id
                GROUP BY a.name, a.country
                ORDER BY departures DESC
                LIMIT 10)")) {
            std::println("  {:<38} {:<16} {:>5} routes to {:>4} airports", r.airport, r.country,
                         r.departures, r.destinations);
        }

        struct Carrier {
            std::string airline{};
            std::string country{};
            long routes{};
        };
        std::println("\nAirlines by route count");
        for (auto const& c : select_into<Carrier>(*db, R"(
                SELECT al.name    AS airline,
                       al.country AS country,
                       count(*)   AS routes
                FROM routes r
                JOIN airlines al ON al.id = r.airline_id
                WHERE al.active
                GROUP BY al.name, al.country
                ORDER BY routes DESC
                LIMIT 10)")) {
            std::println("  {:<38} {:<16} {:>6}", c.airline, c.country, c.routes);
        }

        struct CountryRow {
            std::string country{};
            long airports{};
            double meanAltitude{};
        };
        std::println("\nCountries by airport count");
        for (auto const& c : select_into<CountryRow>(*db, R"(
                SELECT country          AS country,
                       count(*)         AS airports,
                       avg(altitude)    AS mean_altitude
                FROM airports
                GROUP BY country
                ORDER BY airports DESC
                LIMIT 10)")) {
            std::println("  {:<24} {:>5} airports   mean altitude {:>8.0f} ft", c.country,
                         c.airports, c.meanAltitude);
        }

        // ---- and one the builder can ---------------------------------------
        std::println("\nHighest airports, via the typed query builder");
        constexpr auto altitude = field<^^Airport::altitude>;
        constexpr auto country = field<^^Airport::country>;
        for (auto const& a : airports_dao.select()
                                 .where(altitude > 12000 && country.is_not_null())
                                 .order_by(altitude, sort::desc)
                                 .limit(5)
                                 .all()) {
            std::println("  {:<38} {:<16} {:>6} ft", a.name, a.country, a.altitude);
        }

        return 0;
    } catch (std::exception const& e) {
        std::println(stderr, "\nfailed: {}", e.what());
        return 1;
    }
}
