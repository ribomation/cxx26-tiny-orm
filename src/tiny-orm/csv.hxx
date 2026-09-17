#pragma once

#include <filesystem>
#include <format>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <tiny-orm/mapping.hxx>
#include <tiny-orm/schema.hxx>

namespace tiny_orm {

    /// Anything that goes wrong reading CSV. Carries the record number, because a
    /// bad field in line 7412 of a 60k-line file is otherwise unfindable.
    class csv_error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct csv_options {
        char delimiter = ',';
        char quote = '"';

        /**
         * Whether the first record names the columns.
         *
         * With a header, fields are matched to members by name -- trimmed and
         * compared case-insensitively against the same column names the DDL uses.
         * Without one, they are taken positionally in member declaration order,
         * which is what headerless sources such as the OpenFlights `.dat` files
         * need.
         */
        bool has_header = true;

        /**
         * A field exactly equal to this is NULL whatever its type.
         *
         * CSV has no null, so every producer invents one. OpenFlights and
         * PostgreSQL's COPY both write `\N`; many exports write nothing at all,
         * which is handled separately -- see below.
         */
        std::string null_marker{};

        /**
         * A line starting with this character is skipped entirely, header included.
         *
         * `'\0'` disables the handling, which is the default: a data field may
         * legitimately begin with `#`, so a CSV reader should not assume comments
         * unless told. Set it for sources that carry a preamble -- the flight
         * telemetry file below leads with a `#` line describing its units.
         *
         * Deliberately a single character rather than a string. Detection peeks one
         * character without consuming it, which is exact; a multi-character prefix
         * could only be guessed at from the first character, and would sometimes eat
         * a real data line.
         */
        char comment = '\0';

        /// Leave members with no matching column at their default instead of failing.
        bool allow_missing_columns = false;
    };

    namespace detail {

        /**
         * Reads one CSV record, honouring quotes.
         *
         * A quoted field may contain the delimiter, a doubled quote, and newlines,
         * so a record is not the same thing as a line -- which is why this consumes
         * characters rather than using std::getline.
         *
         * Returns false at end of input.
         */
        inline bool read_record(std::istream& in, csv_options const& opt,
                                std::vector<std::string>& fields) {
            fields.clear();

            // Comment lines are dropped before the record starts, so a preamble is
            // invisible to everything downstream -- including header matching. Done
            // here rather than after parsing because a comment containing an odd
            // number of quotes would otherwise put the parser into a quoted field
            // and swallow the lines that follow.
            if (opt.comment != '\0') {
                auto const marker = std::char_traits<char>::to_int_type(opt.comment);
                while (in.peek() == marker) {
                    auto c = char{};
                    while (in.get(c) && c != '\n') {
                        // discard the rest of the line
                    }
                }
            }

            auto field = std::string{};
            auto quoted = false;
            auto any = false;
            auto c = char{};

            while (in.get(c)) {
                any = true;
                if (quoted) {
                    if (c != opt.quote) {
                        field += c;
                    } else if (in.peek() == opt.quote) {
                        in.get(c); // a doubled quote is one literal quote
                        field += opt.quote;
                    } else {
                        quoted = false;
                    }
                } else if (c == opt.quote && field.empty()) {
                    quoted = true;
                } else if (c == opt.delimiter) {
                    fields.push_back(std::move(field));
                    field.clear();
                } else if (c == '\n') {
                    fields.push_back(std::move(field));
                    return true;
                } else if (c != '\r') {
                    field += c;
                }
            }
            if (!any) return false;
            fields.push_back(std::move(field));
            return true;
        }

        /// Trimmed and lower-cased, so "Airport ID " and "airport_id" can be compared.
        inline auto normalise(std::string_view s) -> std::string {
            auto const first = s.find_first_not_of(" \t");
            if (first == std::string_view::npos) return {};
            auto const last = s.find_last_not_of(" \t");
            auto out = std::string{s.substr(first, last - first + 1)};
            for (auto& c : out)
                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            return out;
        }

        /**
         * For each member in declaration order, the field index that feeds it, or
         * -1 when the header has no such column.
         *
         * CSV columns matching no member are ignored: a real export usually carries
         * more than the entity models.
         */
        template<typename T>
        auto field_indices(std::vector<std::string> const& header, csv_options const& opt)
            -> std::vector<int> {
            auto indices = std::vector<int>{};
            auto missing = std::string{};

            auto position = 0;
            template for (constexpr auto m : members_of<T>()) {
                constexpr auto c = describe(m);
                if (!opt.has_header) {
                    indices.push_back(position);
                } else {
                    auto found = -1;
                    for (auto i = 0uz; i < header.size(); ++i)
                        if (normalise(header[i]) == normalise(c.name.view())) {
                            found = static_cast<int>(i);
                            break;
                        }
                    if (found < 0 && !opt.allow_missing_columns) {
                        if (!missing.empty()) missing += ", ";
                        missing += c.name.view();
                    }
                    indices.push_back(found);
                }
                ++position;
            }

            if (!missing.empty()) {
                auto seen = std::string{};
                for (auto const& h : header) {
                    if (!seen.empty()) seen += ", ";
                    seen += h;
                }
                throw csv_error{std::format(
                    "tiny-orm: CSV has no column for {} of '{}'; the header holds: {}", missing,
                    std::meta::identifier_of(^^T), seen)};
            }
            return indices;
        }

        /**
         * Builds one entity from one record.
         *
         * The null rule is decided by reflection rather than configuration: an empty
         * field becomes NULL when the member is a `std::optional`, and stays an
         * empty string when it is not. So a CSV cannot accidentally turn a NOT NULL
         * text column into a null, and a genuinely empty string survives.
         */
        template<typename T>
        auto to_row(std::vector<std::string> const& fields, std::vector<int> const& indices,
                    csv_options const& opt, long record) -> T {
            auto e = T{};
            auto next = 0uz;

            template for (constexpr auto m : members_of<T>()) {
                constexpr auto c = describe(m);
                using V = typename[:std::meta::type_of(m):];

                auto const at = indices[next++];
                if (at < 0 || static_cast<std::size_t>(at) >= fields.size()) continue;

                auto const& raw = fields[static_cast<std::size_t>(at)];
                auto const null = (!opt.null_marker.empty() && raw == opt.null_marker)
                                  || (raw.empty() && is_optional_v<V>);
                try {
                    e.[:m:] = from_cell<V>(null ? cell{} : cell{raw});
                } catch (std::exception const& bad) {
                    throw csv_error{std::format("tiny-orm: record {}, column '{}': {} (value: '{}')",
                                                record, c.name.view(), bad.what(), raw)};
                }
            }
            return e;
        }

    } // namespace detail

    /**
     * Reads a CSV stream into entities, one callback per row.
     *
     * The streaming form: nothing is accumulated, so a file larger than memory can
     * be loaded a batch at a time.
     *
     * ~~~{.cpp}
     * auto batch = std::vector<Flight>{};
     * for_each_csv<Flight>(in, [&](Flight&& f) {
     *     batch.push_back(std::move(f));
     *     if (batch.size() == 5000) { flights.insert_all(batch); batch.clear(); }
     * });
     * if (!batch.empty()) flights.insert_all(batch);
     * ~~~
     */
    template<typename T, typename Fn>
    void for_each_csv(std::istream& in, Fn&& fn, csv_options opt = {}) {
        auto fields = std::vector<std::string>{};
        auto header = std::vector<std::string>{};

        if (opt.has_header) {
            if (!detail::read_record(in, opt, fields)) return; // empty input
            header = fields;
        }
        auto const indices = detail::field_indices<T>(header, opt);

        auto record = 0L;
        while (detail::read_record(in, opt, fields)) {
            ++record;
            if (fields.size() == 1 && fields[0].empty()) continue; // blank line
            fn(detail::to_row<T>(fields, indices, opt, record));
        }
    }

    /**
     * Reads a whole CSV stream into a vector.
     *
     * Columns are matched to members by the same names the DDL uses, so a struct
     * that describes a table also describes the CSV for it -- the descriptor is
     * reused, not re-specified.
     */
    template<typename T>
    auto read_csv(std::istream& in, csv_options opt = {}) -> std::vector<T> {
        auto rows = std::vector<T>{};
        for_each_csv<T>(in, [&rows](T&& row) { rows.push_back(std::move(row)); }, opt);
        return rows;
    }

    /// Reads a CSV file into a vector.
    template<typename T>
    auto read_csv_file(std::filesystem::path const& path, csv_options opt = {}) -> std::vector<T> {
        auto in = std::ifstream{path};
        if (!in) throw csv_error{"tiny-orm: cannot open CSV file '" + path.string() + "'"};
        return read_csv<T>(in, opt);
    }

} // namespace tiny_orm
