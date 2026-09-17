#pragma once

/**
 * The OpenFlights reference data, as plain structs.
 *
 * The `.dat` files are headerless, so member declaration order must match the
 * file's column order -- that is what `csv_options{.has_header = false}` means.
 * A generated key, which the file has no column for, therefore goes *last*:
 * positional mapping simply runs out of columns and leaves it at zero, and the
 * server fills it in on insert.
 *
 * Data (c) OpenFlights contributors, Open Database License (ODbL).
 */

#include <tiny-orm/annotations.hxx>

#include <optional>
#include <string>

using namespace tiny_orm;

/// airports.dat -- 14 columns, id is OpenFlights' own and not generated.
struct [[=Table{.name = "airports"}]] Airport {
    [[=PrimaryKey{}]] long id{};
    std::string name{};
    std::string city{};
    std::string country{};
    std::optional<std::string> iata{};
    std::optional<std::string> icao{};
    double latitude{};
    double longitude{};
    int altitude{};
    [[=Column{.name = "utc_offset"}]] std::optional<double> utcOffset{};
    std::optional<std::string> dst{};
    [[=Column{.name = "tz_database"}]] std::optional<std::string> tzDatabase{};
    std::optional<std::string> type{};
    std::optional<std::string> source{};
};

/// airlines.dat -- 8 columns; `active` is Y/N in the file.
struct [[=Table{.name = "airlines"}]] Airline {
    [[=PrimaryKey{}]] long id{};
    std::string name{};
    std::optional<std::string> alias{};
    std::optional<std::string> iata{};
    std::optional<std::string> icao{};
    std::optional<std::string> callsign{};
    std::optional<std::string> country{};
    bool active{};
};

/// planes.dat -- 3 columns and no key of its own.
struct [[=Table{.name = "planes"}]] Plane {
    std::string name{};
    std::optional<std::string> iata{};
    std::optional<std::string> icao{};
    [[=PrimaryKey{.auto_increment = true}]] long id{};
};

/**
 * routes.dat -- 9 columns and no key of its own.
 *
 * The three id columns are nullable in the file and carry real foreign keys, so
 * a route referencing an airport the airports file does not contain will be
 * rejected by the database. The loader filters those out rather than dropping
 * the constraints; roughly 2% of routes are affected.
 */
struct [[=Table{.name = "routes"}]] Route {
    [[=Column{.name = "airline_code"}]] std::string airlineCode{};
    [[=ForeignKey{.target = ^^Airline, .on_delete = FkAction::cascade}]]
    std::optional<long> airlineId{};
    [[=Column{.name = "source_code"}]] std::string sourceCode{};
    [[=ForeignKey{.target = ^^Airport, .on_delete = FkAction::cascade}]]
    std::optional<long> sourceAirportId{};
    [[=Column{.name = "dest_code"}]] std::string destCode{};
    [[=ForeignKey{.target = ^^Airport, .on_delete = FkAction::cascade}]]
    std::optional<long> destAirportId{};
    std::optional<std::string> codeshare{};
    int stops{};
    std::optional<std::string> equipment{};
    [[=PrimaryKey{.auto_increment = true}]] long id{};
};
