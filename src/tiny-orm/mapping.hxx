#pragma once

#include <charconv>
#include <chrono>
#include <sstream>
#include <optional>
#include <string>
#include <type_traits>

#include <tiny-orm/connection.hxx>
#include <tiny-orm/schema.hxx>

namespace tiny_orm::detail {

    template<typename>
    inline constexpr bool always_false = false;

    template<typename>
    inline constexpr bool is_sys_time_v = false;
    template<typename D>
    inline constexpr bool is_sys_time_v<std::chrono::sys_time<D>> = true;

    template<typename>
    inline constexpr bool is_optional_v = false;
    template<typename U>
    inline constexpr bool is_optional_v<std::optional<U>> = true;

    /**
     * Parses one result cell into a member's type.
     *
     * The inverse of `to_param`. `std::optional<U>` absorbs a NULL; any other type
     * meeting one rejects the row rather than silently yielding a default, since a
     * NULL in a NOT NULL column means the schema and the entity disagree.
     */
    template<typename V>
    auto from_cell(cell const& c) -> V {
        if constexpr (is_optional_v<V>) {
            if (!c.has_value()) return V{};
            return V{from_cell<typename V::value_type>(c)};
        } else if constexpr (std::is_same_v<V, std::string>) {
            if (!c.has_value()) throw db_error{"tiny-orm: NULL in a non-optional string column"};
            return *c;
        } else if constexpr (std::is_same_v<V, bool>) {
            if (!c.has_value()) throw db_error{"tiny-orm: NULL in a non-optional bool column"};
            // PostgreSQL prints t/f, SQLite 1/0, and CSV producers write whatever they
            // like -- OpenFlights uses Y/N. Accept the common spellings, case-insensitively.
            auto lowered = std::string{};
            for (auto ch : *c) lowered += (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch;
            return lowered == "t" || lowered == "true" || lowered == "1" || lowered == "y"
                   || lowered == "yes";
        } else if constexpr (is_sys_time_v<V>) {
            if (!c.has_value()) throw db_error{"tiny-orm: NULL in a non-optional timestamp column"};
            // PostgreSQL separates date and time with a space; ISO-8601 text from a
            // CSV often uses T. Normalise so one format string handles both.
            auto text = *c;
            if (text.size() > 10 && text[10] == 'T') text[10] = ' ';
            auto in = std::istringstream{text};
            auto out = V{};
            std::chrono::from_stream(in, "%F %T", out);
            if (in.fail()) throw db_error{"tiny-orm: cannot parse '" + *c + "' as a timestamp"};
            return out;
        } else if constexpr (std::is_arithmetic_v<V>) {
            if (!c.has_value()) throw db_error{"tiny-orm: NULL in a non-optional numeric column"};
            auto out = V{};
            auto const first = c->data();
            auto const last = c->data() + c->size();
            auto const [ptr, ec] = std::from_chars(first, last, out);
            if (ec != std::errc{} || ptr != last)
                throw db_error{"tiny-orm: cannot parse '" + *c + "' as a numeric column"};
            return out;
        } else {
            static_assert(always_false<V>, "tiny-orm: no conversion from a result cell to this type");
        }
    }

    /**
     * The entity's primary-key data member.
     *
     * Separate from `primary_key_column` because `std::meta::info` is a
     * consteval-only type: a `column_info` carrying one could not appear in a
     * run-time function at all. This value is only ever used in constant contexts
     * -- as a splice operand or a template argument.
     */
    template<typename T>
    consteval auto primary_key_member() -> std::meta::info {
        template for (constexpr auto m : members_of<T>()) {
            if constexpr (describe(m).primary_key) return m;
        }
        throw std::meta::exception{"tiny-orm: entity '"
                                       + std::string{std::meta::identifier_of(^^T)}
                                       + "' has no member annotated [[=PrimaryKey{...}]]",
                                   ^^T};
    }

    /**
     * Builds an entity from one result row, looking columns up by name.
     *
     * The mirror image of the parameter binding in `insert_object`: the same
     * expansion over the members, the same splice, assigning instead of reading.
     */
    template<typename T>
    auto to_entity(result_set const& rs, std::size_t row) -> T {
        auto e = T{};
        template for (constexpr auto m : members_of<T>()) {
            constexpr auto c = describe(m);
            using V = typename[:std::meta::type_of(m):];
            e.[:m:] = from_cell<V>(rs.at(row, c.name.view()));
        }
        return e;
    }

} // namespace tiny_orm::detail
