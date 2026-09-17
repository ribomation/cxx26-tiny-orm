#pragma once

#include <meta>

#include <tiny-orm/compiletime-string.hxx>

namespace tiny_orm {

    /**
     * Annotation types, attached to declarations with `[[=...]]`.
     *
     * Every field is optional: an annotation only ever states what it
     * *overrides*. Anything left default is derived by reflection instead --
     * a table name from the type's identifier, a column name from the member's
     * identifier, an SQL type from the member's C++ type. That is the point of
     * the experiment, so the annotations exist only for the cases reflection
     * cannot know about.
     *
     * All of these are aggregates with public members, which is not a style
     * choice: an annotation payload must be a *structural* type, so private
     * members would make them unusable. See `compiletime-string.hxx` for the
     * full rule.
     *
     * Read them back with `annotations_of_with_type`, which normalises the
     * const-qualification that `annotations_of` would otherwise expose:
     *
     * ~~~{.cpp}
     * for (auto a : std::meta::annotations_of_with_type(^^T, ^^tiny_orm::Table))
     *     return std::meta::extract<tiny_orm::Table>(a);
     * ~~~
     */

    /**
     * Overrides the table name for an entity type.
     *
     * Without it the table name is the type's identifier in snake_case and
     * pluralised: `struct Transaction` becomes `transactions`.
     *
     * ~~~{.cpp}
     * struct [[=Table{.name = "bank_accounts"}]] Account { ... };
     * ~~~
     */
    struct Table {
        /// SQL table name. Empty means "derive from the type identifier".
        compiletime_string name{};
    };

    /**
     * Tri-state override for a column's nullability.
     *
     * Deliberately not a `bool`. Every other annotation field uses "empty" as
     * its "not specified" signal, but a `bool nullable = true` has no such
     * state: the descriptor cannot tell a member left alone from one explicitly
     * set to the default, so merely mentioning an unrelated field like
     * `default_value` would silently make the column nullable.
     *
     * `std::optional<bool>` would express it, but `std::optional` is not a
     * structural type and so cannot appear in an annotation payload.
     */
    enum class Nullable {
        /// Derive from the C++ type: `std::optional<T>` is nullable, anything else is NOT NULL.
        derive,
        yes,
        no
    };

    /**
     * Overrides how a data member maps to a column.
     *
     * ~~~{.cpp}
     * struct Account {
     *     [[=Column{.name = "owner", .nullable = Nullable::no}]] std::string ownerName;
     * };
     * ~~~
     */
    struct Column {
        /// SQL column name. Empty means "derive from the member identifier".
        compiletime_string name{};
        /// SQL type. Empty means "derive from the member's C++ type".
        compiletime_string sql_type{};
        /// Nullability; by default taken from the member's C++ type.
        Nullable nullable = Nullable::derive;
        /// Literal text for a DEFAULT clause; empty means no DEFAULT.
        compiletime_string default_value{};
    };

    /**
     * Marks a member as the primary key.
     *
     * Carries no name of its own -- the column name still comes from `Column`
     * or from the member identifier, so the two annotations compose.
     *
     * ~~~{.cpp}
     * struct Account { [[=PrimaryKey{.auto_increment = true}]] long id; };
     * ~~~
     */
    struct PrimaryKey {
        /// Overrides the key's SQL type; empty means "derive from the C++ type".
        compiletime_string sql_type{};
        /// Generate the key server-side (SERIAL / GENERATED ... AS IDENTITY).
        bool auto_increment = false;
    };

    /// What the database should do to referencing rows when the target row is deleted.
    enum class FkAction {
        /// Omit the ON DELETE clause and let the server apply its default.
        none,
        no_action,
        restrict_, ///< Trailing underscore: `restrict` is a keyword in C and a GCC extension.
        cascade,
        set_null,
        set_default
    };

    /**
     * Marks a member as a foreign key into another entity.
     *
     * `target` is a reflection of the referenced *entity type*, not of a column,
     * so the referenced table name is looked up the same way as any other --
     * keeping one source of truth for it. `std::meta::info` is itself a
     * structural type, which is what allows it to sit in an annotation payload.
     *
     * ~~~{.cpp}
     * struct [[=Table{.name = "txn"}]] Transaction {
     *     [[=ForeignKey{.target = ^^Account, .on_delete = FkAction::cascade}]]
     *     long accountId;
     * };
     * ~~~
     */
    struct ForeignKey {
        /// Reflection of the referenced entity type, e.g. `^^Account`.
        std::meta::info target{};
        /// Referenced column in the target table; empty means its primary key.
        compiletime_string column{};
        /// ON DELETE behaviour.
        FkAction on_delete = FkAction::none;
    };

    static_assert(std::is_structural_v<Table>);
    static_assert(std::is_structural_v<Column>);
    static_assert(std::is_structural_v<PrimaryKey>);
    static_assert(std::is_structural_v<ForeignKey>);

} // namespace tiny_orm
