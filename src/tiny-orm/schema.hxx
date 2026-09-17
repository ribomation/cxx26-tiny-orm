#pragma once

#include <chrono>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tiny-orm/annotations.hxx>
#include <tiny-orm/compiletime-string.hxx>

namespace tiny_orm {

    /**
     * A column's type, independent of any SQL dialect.
     *
     * The descriptor used to bake "DOUBLE PRECISION" into itself at compile time,
     * which no dialect could undo. It now records what the column *is*; rendering
     * that into engine-specific SQL is a `dialect`'s job.
     */
    enum class type_kind { boolean, int16, int32, int64, float32, float64, text, timestamp };

    /**
     * One fully resolved column: whatever reflection could derive from the data
     * member, with any annotation overrides already applied on top.
     *
     * The foreign key is flattened into the column rather than held as a nested
     * `std::optional`, because `std::optional` is not structural and a member of
     * that type would disqualify the whole descriptor from
     * `std::define_static_array`. An empty `references_table` is the "no foreign
     * key" signal.
     */
    struct column_info {
        compiletime_string name{};
        /// Derived from the member's C++ type; rendered by the dialect.
        type_kind kind = type_kind::text;
        /// Raw Column/PrimaryKey sql_type override. Empty means "render `kind`".
        compiletime_string sql_type{};
        bool nullable = false;
        compiletime_string default_value{};
        bool primary_key = false;
        bool auto_increment = false;
        compiletime_string references_table{}; ///< Empty when this is not a foreign key.
        compiletime_string references_column{};
        FkAction on_delete = FkAction::none;
    };

    static_assert(std::is_structural_v<column_info>);

    namespace detail {

        /// `fullName` -> `full_name`, `accountID` -> `account_id`.
        consteval auto to_snake(std::string_view s) -> std::string {
            auto const upper = [](char c) { return c >= 'A' && c <= 'Z'; };
            auto out = std::string{};
            for (auto i = 0uz; i < s.size(); ++i) {
                if (!upper(s[i])) { out += s[i]; continue; }
                // No separator inside a run of capitals, so `ID` stays `id`.
                if (i > 0 && !upper(s[i - 1])) out += '_';
                out += char(s[i] - 'A' + 'a');
            }
            return out;
        }

        /**
         * Naive English pluralisation, good enough for identifiers:
         * `transaction` -> `transactions`, `category` -> `categories`,
         * `address` -> `addresses`. Irregular nouns are not attempted -- that is
         * what `[[=Table{.name = "..."}]]` is for.
         */
        consteval auto to_plural(std::string_view s) -> std::string {
            auto const ends_with = [s](std::string_view suffix) {
                return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
            };
            auto const vowel = [](char c) {
                return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
            };
            if (ends_with("s") || ends_with("x") || ends_with("z")
                || ends_with("ch") || ends_with("sh"))
                return std::string{s} + "es";
            if (ends_with("y") && s.size() >= 2 && !vowel(s[s.size() - 2]))
                return std::string{s.substr(0, s.size() - 1)} + "ies";
            return std::string{s} + "s";
        }

        /**
         * True for any `std::chrono::sys_time<D>`, which includes
         * `std::chrono::system_clock::time_point` and `std::chrono::sys_seconds`.
         *
         * Matched structurally rather than against one spelling, so a member can
         * pick the precision it wants.
         */
        consteval bool is_timestamp(std::meta::info t) {
            if (!std::meta::has_template_arguments(t)) return false;
            if (std::meta::template_of(t) != std::meta::dealias(^^std::chrono::time_point))
                return false;
            auto const args = std::meta::template_arguments_of(t);
            return !args.empty()
                   && std::meta::dealias(args[0])
                          == std::meta::dealias(^^std::chrono::system_clock);
        }

        /// True for `std::optional<T>`, which is how a member declares itself nullable.
        consteval bool is_optional(std::meta::info t) {
            return std::meta::has_template_arguments(t)
                   && std::meta::template_of(t) == ^^std::optional;
        }

        /// Strips one layer of `std::optional`, leaving other types untouched.
        consteval auto unwrap(std::meta::info t) -> std::meta::info {
            return is_optional(t)
                       ? std::meta::dealias(
                             std::meta::remove_cvref(std::meta::template_arguments_of(t)[0]))
                       : t;
        }

        /**
         * Reads one annotation off a declaration, if present.
         *
         * `annotations_of_with_type` rather than a hand-rolled `type_of` comparison:
         * annotations come back const-qualified for class types, so the obvious
         * `type_of(a) == ^^A` silently never matches.
         *
         * The `std::optional` return is fine here -- this is an ordinary consteval
         * local, never an annotation payload or template argument, so the
         * structural-type rule does not apply.
         */
        template<typename A>
        consteval auto annotation_on(std::meta::info decl) -> std::optional<A> {
            for (auto a : std::meta::annotations_of_with_type(decl, std::meta::remove_cvref(^^A)))
                return std::meta::extract<A>(a);
            return {};
        }

        /**
         * Maps a member's C++ type to a canonical column type.
         *
         * Returns nothing rather than throwing, so an explicit
         * `Column{.sql_type = "..."}` can still carry a type the framework does
         * not otherwise know. `std::optional<T>` is unwrapped first: nullability
         * is a separate column property, not part of the type.
         */
        consteval auto try_type_kind_of(std::meta::info member) -> std::optional<type_kind> {
            auto const t =
                unwrap(std::meta::dealias(std::meta::remove_cvref(std::meta::type_of(member))));
            if (t == ^^bool) return type_kind::boolean;
            if (t == ^^short) return type_kind::int16;
            if (t == ^^int) return type_kind::int32;
            if (t == ^^long) return type_kind::int64;
            if (t == ^^long long) return type_kind::int64;
            if (t == ^^float) return type_kind::float32;
            if (t == ^^double) return type_kind::float64;
            // dealias: ^^std::string reflects the *alias*, which never compares equal
            // to the member's actual std::__cxx11::basic_string<char>.
            if (t == std::meta::dealias(^^std::string)) return type_kind::text;
            if (t == std::meta::dealias(^^std::string_view)) return type_kind::text;
            if (is_timestamp(t)) return type_kind::timestamp;
            return {};
        }

        /// True for the integer kinds, which are the only ones an identity column
        /// may use. constexpr rather than consteval: the compile-time validator and
        /// the run-time dialect check both call it.
        constexpr bool is_integral_kind(type_kind k) {
            return k == type_kind::int16 || k == type_kind::int32 || k == type_kind::int64;
        }

        /// Table name for an entity: the annotation if given, else snake_case + plural.
        consteval auto table_name(std::meta::info type) -> compiletime_string {
            if (auto const t = annotation_on<Table>(type); t && !t->name.empty()) return t->name;
            return to_plural(to_snake(std::meta::identifier_of(type)));
        }

        /// Column name of a member: the annotation if given, else snake_case.
        consteval auto column_name(std::meta::info member) -> compiletime_string {
            if (auto const c = annotation_on<Column>(member); c && !c->name.empty()) return c->name;
            return to_snake(std::meta::identifier_of(member));
        }

        /// Primary-key column of an entity, for a foreign key that does not name one.
        consteval auto primary_key_name(std::meta::info type) -> compiletime_string {
            for (auto m :
                 std::meta::nonstatic_data_members_of(type, std::meta::access_context::unprivileged()))
                if (annotation_on<PrimaryKey>(m)) return column_name(m);
            return "id";
        }

        /**
         * The entity's data members, as an expansion-statement range.
         *
         * Deliberately returned inline rather than stored: `std::meta::info` is a
         * consteval-only type, so a `constexpr` variable holding one -- or holding
         * a `column_info` that contained one -- cannot appear in a run-time
         * function at all. Producing the range in the `template for` header keeps
         * every reflection value inside constant evaluation, where it belongs.
         */
        template<typename T>
        consteval auto members_of() {
            return std::define_static_array(
                std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));
        }

        consteval auto describe(std::meta::info m) -> column_info {
            auto const type = std::meta::dealias(std::meta::remove_cvref(std::meta::type_of(m)));
            auto const col = annotation_on<Column>(m);
            auto const pk = annotation_on<PrimaryKey>(m);
            auto const fk = annotation_on<ForeignKey>(m);

            auto out = column_info{};
            out.name = column_name(m);

            // SQL type, in priority order: an explicit Column override, then an
            // explicit PrimaryKey override, and failing both, the member's C++ type.
            auto const derived = try_type_kind_of(m);
            if (col && !col->sql_type.empty())
                out.sql_type = col->sql_type;
            else if (pk && !pk->sql_type.empty())
                out.sql_type = pk->sql_type;

            if (out.sql_type.empty() && !derived.has_value())
                throw std::meta::exception{"tiny-orm: no SQL type mapping for member '"
                                               + std::string{std::meta::identifier_of(m)}
                                               + "' of type '"
                                               + std::string{std::meta::display_string_of(
                                                     std::meta::type_of(m))}
                                               + "'; give it an explicit "
                                                 "[[=Column{.sql_type = \"...\"}]]",
                                           m};
            out.kind = derived.value_or(type_kind::text);

            // Nullability. A member with no Column annotation states nothing, which
            // is the same as stating `derive`.
            auto const stated = col ? col->nullable : Nullable::derive;
            switch (stated) {
                case Nullable::yes: out.nullable = true; break;
                case Nullable::no: out.nullable = false; break;
                case Nullable::derive: out.nullable = is_optional(type); break;
            }

            if (col) {
                out.default_value = col->default_value;
            }
            if (pk) {
                out.primary_key = true;
                out.auto_increment = pk->auto_increment;
                out.nullable = false; // a primary key is never nullable
            }
            if (fk) {
                out.references_table = table_name(fk->target);
                out.references_column =
                    fk->column.empty() ? primary_key_name(fk->target) : fk->column;
                out.on_delete = fk->on_delete;
            }
            return out;
        }

    } // namespace detail

    /**
     * The columns of an entity, resolved at compile time.
     *
     * Only publicly accessible members are mapped -- hence
     * `access_context::unprivileged()` rather than `current()`.
     *
     * ~~~{.cpp}
     * constexpr auto cols = columns_of<Account>();
     * for (auto const& c : cols) ...     // ordinary run-time loop
     * ~~~
     */
    template<typename T>
    consteval auto columns_of() -> std::span<const column_info> {
        auto v = std::vector<column_info>{};
        for (auto m :
             std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))
            v.push_back(detail::describe(m));
        return std::define_static_array(v);
    }

    /// The table name of an entity, resolved at compile time.
    template<typename T>
    consteval auto table_name_of() -> compiletime_string {
        return detail::table_name(^^T);
    }

} // namespace tiny_orm
