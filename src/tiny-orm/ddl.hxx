#pragma once

#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <tiny-orm/connection.hxx>
#include <tiny-orm/dialect.hxx>
#include <tiny-orm/schema.hxx>

namespace tiny_orm {

    /**
     * DDL generation for PostgreSQL.
     *
     * This is the run-time half of the framework. Nothing here reflects on
     * anything: `columns_of<T>()` has already resolved the entity into a
     * `column_info` array in static storage at compile time, and these functions
     * only walk that array and assemble text. The compile-time / run-time seam of
     * the whole design sits exactly at this boundary.
     */

    struct ddl_options {
        /// Emit CREATE TABLE IF NOT EXISTS.
        bool if_not_exists = true;
        /**
         * Wrap identifiers in the dialect's quotes.
         *
         * On by default because engines reserve words a banking schema will
         * plausibly use as column names -- `user` and `order` among them -- and an
         * unquoted one is a syntax error. Turn it off for more readable output when
         * you know the names are safe.
         */
        bool quote_identifiers = true;
    };

    struct drop_options {
        /// Emit DROP TABLE IF EXISTS.
        bool if_exists = true;
        /// Append CASCADE, dropping dependent objects such as foreign keys.
        /// Ignored by dialects that do not accept the keyword.
        bool cascade = false;
        bool quote_identifiers = true;
    };

    namespace detail {

        constexpr auto to_sql(FkAction a) -> std::string_view {
            switch (a) {
                case FkAction::none: return {}; // omit the clause; let the server decide
                case FkAction::no_action: return "NO ACTION";
                case FkAction::restrict_: return "RESTRICT";
                case FkAction::cascade: return "CASCADE";
                case FkAction::set_null: return "SET NULL";
                case FkAction::set_default: return "SET DEFAULT";
            }
            return {};
        }

        inline auto join(std::vector<std::string> const& parts, std::string_view sep) -> std::string {
            auto out = std::string{};
            for (auto i = 0uz; i < parts.size(); ++i) {
                if (i > 0) out += sep;
                out += parts[i];
            }
            return out;
        }

        /**
         * The entity's columns, rejected at compile time if any of them ask for
         * something PostgreSQL will not accept.
         *
         * Checking the resolved `sql_type` rather than the member's C++ type is
         * deliberate: it also catches a `Column{.sql_type = "TEXT"}` override
         * placed on a member that `PrimaryKey{.auto_increment = true}` marks as an
         * identity column. Without this the mistake survives compilation and
         * surfaces only when the server rejects the DDL.
         */
        template<typename T>
        consteval auto validated_columns_of() -> std::span<const column_info> {
            auto const columns = columns_of<T>();
            for (auto const& c : columns)
                if (c.auto_increment && c.sql_type.empty() && !is_integral_kind(c.kind))
                    throw std::meta::exception{
                        "tiny-orm: column '" + std::string{c.name.view()}
                            + "' is declared auto_increment, but its C++ type is not an "
                              "integer type; identity columns must be integral",
                        ^^T};
            return columns;
        }

    } // namespace detail

    /**
     * CREATE TABLE for an entity.
     *
     * The primary key and every foreign key are emitted as table-level
     * constraints rather than inline column modifiers: one uniform rule instead of
     * a single-column special case, composite keys work unchanged, and the
     * constraints get stable names that a later migration can drop by name.
     *
     * ~~~{.cpp}
     * auto sql = create_table_sql<Account>();
     * auto raw = create_table_sql<Account>({.if_not_exists = false, .quote_identifiers = false});
     * ~~~
     */
    template<typename T>
    auto create_table_sql(dialect const& sql, ddl_options opt = {}) -> std::string {
        constexpr auto columns = detail::validated_columns_of<T>();
        constexpr auto table = table_name_of<T>();

        auto const quoted = [&sql, opt](std::string_view id) -> std::string {
            return opt.quote_identifiers ? sql.quote(id) : std::string{id};
        };

        auto items = std::vector<std::string>{};

        for (auto const& c : columns) {
            // A raw sql_type override cannot be validated at compile time, so the
            // dialect gets the last word on whether it may carry an identity clause.
            if (c.auto_increment && !sql.allows_identity(c))
                throw db_error{"tiny-orm: column '" + std::string{c.name.view()}
                               + "' is declared auto_increment but its SQL type '"
                               + std::string{sql.type_for(c)} + "' is not an integer type in "
                               + std::string{sql.name()}};

            auto line = std::format("  {} {}", quoted(c.name.view()), sql.type_for(c));
            if (c.auto_increment && !sql.identity_clause().empty())
                line += std::format(" {}", sql.identity_clause());
            if (!c.nullable) line += " NOT NULL";
            if (!c.default_value.empty()) line += std::format(" DEFAULT {}", c.default_value);
            items.push_back(line);
        }

        auto keys = std::vector<std::string>{};
        for (auto const& c : columns)
            if (c.primary_key) keys.push_back(quoted(c.name.view()));
        if (!keys.empty()) items.push_back(std::format("  PRIMARY KEY ({})", detail::join(keys, ", ")));

        for (auto const& c : columns) {
            if (c.references_table.empty()) continue;
            auto line = std::format("  CONSTRAINT {} FOREIGN KEY ({}) REFERENCES {} ({})",
                                    quoted(std::format("fk_{}_{}", table, c.name)),
                                    quoted(c.name.view()),
                                    quoted(c.references_table.view()),
                                    quoted(c.references_column.view()));
            if (auto const action = detail::to_sql(c.on_delete); !action.empty())
                line += std::format(" ON DELETE {}", action);
            items.push_back(line);
        }

        return std::format("CREATE TABLE {}{} (\n{}\n);",
                           opt.if_not_exists ? "IF NOT EXISTS " : "",
                           quoted(table.view()),
                           detail::join(items, ",\n"));
    }

    /**
     * DROP TABLE for an entity.
     *
     * Note that dropping several entities needs reverse dependency order -- a
     * table referenced by a foreign key cannot be dropped before the table holding
     * it, short of `CASCADE`, which silently discards the constraint. Ordering a
     * whole set is not attempted here.
     */
    template<typename T>
    auto drop_table_sql(dialect const& sql, drop_options opt = {}) -> std::string {
        constexpr auto table = table_name_of<T>();
        auto const name = opt.quote_identifiers ? sql.quote(table.view())
                                                : std::string{table.view()};
        return std::format("DROP TABLE {}{}{};",
                           opt.if_exists ? "IF EXISTS " : "",
                           name,
                           opt.cascade && sql.supports_drop_cascade() ? " CASCADE" : "");
    }

} // namespace tiny_orm
