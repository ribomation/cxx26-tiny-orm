#pragma once

#include <chrono>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tiny-orm/connection.hxx>
#include <tiny-orm/dialect.hxx>
#include <tiny-orm/mapping.hxx>
#include <tiny-orm/schema.hxx>

namespace tiny_orm {

    // `param`, `statement` and `result_set` live in connection.hxx: they are the
    // vocabulary shared with the backend, not specific to statement generation.

    namespace detail {

        /**
         * Fragments are assembled with two markers and rendered once at the end.
         *
         * A predicate such as `field<^^Account::balance> > 100.0` is built long
         * before any dialect is in scope, so neither identifier quoting nor
         * parameter numbering can happen there. Both are deferred: identifiers are
         * wrapped in a control character that cannot occur in a real identifier,
         * parameters are written as `?`, and `render` makes one pass over the
         * finished fragment.
         */
        inline constexpr char id_mark = '\x01';

        inline auto mark_id(std::string_view identifier) -> std::string {
            return std::string{id_mark} + std::string{identifier} + id_mark;
        }

        /// Quotes marked identifiers and numbers `?` markers, in one pass.
        inline auto render(dialect const& sql, std::string_view fragment) -> std::string {
            auto out = std::string{};
            auto n = 0;
            for (auto i = 0uz; i < fragment.size(); ++i) {
                if (fragment[i] == id_mark) {
                    auto const end = fragment.find(id_mark, i + 1);
                    out += sql.quote(fragment.substr(i + 1, end - i - 1));
                    i = end;
                } else if (fragment[i] == '?') {
                    out += sql.placeholder(++n);
                } else {
                    out += fragment[i];
                }
            }
            return out;
        }

        template<typename V>
        auto to_param(V const& v) -> param {
            if constexpr (requires { v.has_value(); *v; })
                return v.has_value() ? to_param(*v) : param{};
            else if constexpr (std::same_as<V, bool>)
                // "1"/"0" rather than "true"/"false", because parameters are bound as
                // text and the engines disagree about what happens next. PostgreSQL
                // coerces either spelling into a BOOLEAN column. SQLite does not coerce:
                // the column has INTEGER affinity, "true" is not convertible to an
                // integer, so it is stored *as text* -- and `WHERE flag` then evaluates a
                // non-numeric string as false, silently, for every row.
                return v ? "1" : "0";
            else if constexpr (std::convertible_to<V const&, std::string_view>)
                return std::string{std::string_view{v}};
            else if constexpr (requires { typename V::clock; })
                // Truncated to microseconds, which is what PostgreSQL's TIMESTAMP
                // stores; system_clock::time_point carries nanoseconds on libstdc++.
                return std::format("{:%F %T}", std::chrono::floor<std::chrono::microseconds>(v));
            else
                return std::format("{}", v);
        }

    } // namespace detail

    // =====================================================================
    // Predicates
    // =====================================================================

    /**
     * A WHERE fragment and its bound values.
     *
     * Templated on the entity so a predicate built from one entity's fields
     * cannot be handed to another entity's query -- that is a compile error
     * rather than a run-time surprise. The `sql` text still holds unrendered
     * markers; see `detail::render`.
     */
    template<typename Entity>
    struct predicate {
        std::string sql;
        std::vector<param> params;
    };

    template<typename E>
    auto operator&&(predicate<E> a, predicate<E> b) -> predicate<E> {
        a.sql = std::format("({} AND {})", a.sql, b.sql);
        a.params.insert(a.params.end(), b.params.begin(), b.params.end());
        return a;
    }
    template<typename E>
    auto operator||(predicate<E> a, predicate<E> b) -> predicate<E> {
        a.sql = std::format("({} OR {})", a.sql, b.sql);
        a.params.insert(a.params.end(), b.params.begin(), b.params.end());
        return a;
    }
    template<typename E>
    auto operator!(predicate<E> p) -> predicate<E> {
        p.sql = std::format("NOT ({})", p.sql);
        return p;
    }

    /**
     * A reference to one column, named by a reflection of the data member.
     *
     * `field<^^Account::balance>` carries enough to recover both the owning entity
     * and the column name, so nothing is repeated or spelled as a string.
     * Comparisons check the operand against the member's own type, so
     * `field<^^Account::balance> > "abc"` does not compile.
     */
    template<std::meta::info M>
    struct field_t {
        using entity = typename[:std::meta::parent_of(M):];
        /// The member's type with `std::optional` stripped: comparing against a
        /// nullable column still compares against the underlying value.
        using value_type = typename[:detail::unwrap(
            std::meta::dealias(std::meta::remove_cvref(std::meta::type_of(M)))):];

        static constexpr auto column = detail::column_name(M);

        template<typename V>
        auto compare(std::string_view op, V const& v) const -> predicate<entity> {
            static_assert(std::convertible_to<V const&, value_type>,
                          "tiny-orm: the compared value is not convertible to the column's type");
            return {std::format("{} {} ?", detail::mark_id(column.view()), op),
                    {detail::to_param(static_cast<value_type>(v))}};
        }

        template<typename V> auto operator==(V const& v) const { return compare("=", v); }
        template<typename V> auto operator!=(V const& v) const { return compare("<>", v); }
        template<typename V> auto operator<(V const& v) const { return compare("<", v); }
        template<typename V> auto operator<=(V const& v) const { return compare("<=", v); }
        template<typename V> auto operator>(V const& v) const { return compare(">", v); }
        template<typename V> auto operator>=(V const& v) const { return compare(">=", v); }

        auto like(std::string_view pattern) const -> predicate<entity> {
            return {std::format("{} LIKE ?", detail::mark_id(column.view())), {std::string{pattern}}};
        }
        auto is_null() const -> predicate<entity> {
            return {std::format("{} IS NULL", detail::mark_id(column.view())), {}};
        }
        auto is_not_null() const -> predicate<entity> {
            return {std::format("{} IS NOT NULL", detail::mark_id(column.view())), {}};
        }
        template<typename V>
        auto in(std::initializer_list<V> values) const -> predicate<entity> {
            auto p = predicate<entity>{};
            auto marks = std::string{};
            for (auto const& v : values) {
                if (!marks.empty()) marks += ", ";
                marks += "?";
                p.params.push_back(detail::to_param(static_cast<value_type>(v)));
            }
            p.sql = std::format("{} IN ({})", detail::mark_id(column.view()), marks);
            return p;
        }
    };

    template<std::meta::info M>
    inline constexpr auto field = field_t<M>{};

    enum class sort { asc, desc };

    // =====================================================================
    // Per-entity helpers
    // =====================================================================

    namespace detail {

        /// The single primary-key column of an entity, rejected at compile time if absent.
        template<typename T>
        consteval auto primary_key_column() -> column_info {
            for (auto const& c : columns_of<T>())
                if (c.primary_key) return c;
            throw std::meta::exception{
                "tiny-orm: entity '" + std::string{std::meta::identifier_of(^^T)}
                    + "' has no member annotated [[=PrimaryKey{...}]], so it cannot be "
                      "inserted, updated, deleted or fetched by key",
                ^^T};
        }

        inline auto column_list(std::span<const column_info> cols) -> std::string {
            auto out = std::string{};
            for (auto const& c : cols) {
                if (!out.empty()) out += ", ";
                out += mark_id(c.name.view());
            }
            return out;
        }

    } // namespace detail

    // =====================================================================
    // INSERT / UPDATE / DELETE / SELECT by primary key
    // =====================================================================

    /**
     * INSERT for one entity value.
     *
     * Identity columns are omitted so the server generates them. RETURNING is
     * appended only where the dialect has it; otherwise the caller has to fetch
     * the key with `dialect::last_insert_id_sql()`.
     */
    template<typename T>
    auto insert_object(T const& e, dialect const& sql) -> statement {
        constexpr auto key = detail::primary_key_column<T>();
        auto names = std::string{};
        auto marks = std::string{};
        auto params = std::vector<param>{};

        template for (constexpr auto m : detail::members_of<T>()) {
            constexpr auto c = detail::describe(m);
            if constexpr (!c.auto_increment) {
                if (!names.empty()) {
                    names += ", ";
                    marks += ", ";
                }
                names += detail::mark_id(c.name.view());
                marks += "?";
                params.push_back(detail::to_param(e.[:m:]));
            }
        }
        auto fragment = std::format("INSERT INTO {} ({}) VALUES ({})",
                                    detail::mark_id(table_name_of<T>().view()), names, marks);
        if (sql.supports_returning())
            fragment += std::format(" RETURNING {}", detail::mark_id(key.name.view()));
        fragment += ";";
        return {detail::render(sql, fragment), std::move(params)};
    }

    namespace detail {

        /// How many columns one row of T actually binds: all but the generated key.
        template<typename T>
        consteval auto bound_column_count() -> int {
            auto n = 0;
            for (auto const& c : columns_of<T>())
                if (!c.auto_increment) ++n;
            return n;
        }

    } // namespace detail

    /**
     * A multi-row INSERT.
     *
     * One statement of the form `INSERT INTO t (...) VALUES (...), (...), ...`,
     * which is what makes bulk loading worth doing: the cost of a round trip is
     * paid once instead of once per row. The caller is responsible for keeping the
     * row count within the dialect's parameter budget -- `DAO<T>::insert_all`
     * does that batching.
     */
    template<typename T>
    auto insert_many(std::span<const T> rows, dialect const& sql) -> statement {
        constexpr auto key = detail::primary_key_column<T>();
        auto names = std::string{};
        auto values = std::string{};
        auto params = std::vector<param>{};

        template for (constexpr auto m : detail::members_of<T>()) {
            constexpr auto c = detail::describe(m);
            if constexpr (!c.auto_increment) {
                if (!names.empty()) names += ", ";
                names += detail::mark_id(c.name.view());
            }
        }

        for (auto const& e : rows) {
            if (!values.empty()) values += ", ";
            values += "(";
            auto first = true;
            template for (constexpr auto m : detail::members_of<T>()) {
                constexpr auto c = detail::describe(m);
                if constexpr (!c.auto_increment) {
                    if (!first) values += ", ";
                    first = false;
                    values += "?";
                    params.push_back(detail::to_param(e.[:m:]));
                }
            }
            values += ")";
        }

        auto fragment = std::format("INSERT INTO {} ({}) VALUES {}",
                                    detail::mark_id(table_name_of<T>().view()), names, values);
        if (sql.supports_returning())
            fragment += std::format(" RETURNING {}", detail::mark_id(key.name.view()));
        fragment += ";";
        return {detail::render(sql, fragment), std::move(params)};
    }

    /** UPDATE of every non-key column, selected by primary key. */
    template<typename T>
    auto update_by_object(T const& e, dialect const& sql) -> statement {
        constexpr auto key = detail::primary_key_column<T>();
        auto sets = std::string{};
        auto params = std::vector<param>{};
        auto key_param = param{};

        template for (constexpr auto m : detail::members_of<T>()) {
            constexpr auto c = detail::describe(m);
            if constexpr (c.primary_key) {
                key_param = detail::to_param(e.[:m:]);
            } else {
                if (!sets.empty()) sets += ", ";
                sets += std::format("{} = ?", detail::mark_id(c.name.view()));
                params.push_back(detail::to_param(e.[:m:]));
            }
        }
        params.push_back(std::move(key_param));
        auto const fragment = std::format("UPDATE {} SET {} WHERE {} = ?;",
                                          detail::mark_id(table_name_of<T>().view()), sets,
                                          detail::mark_id(key.name.view()));
        return {detail::render(sql, fragment), std::move(params)};
    }

    /** DELETE by primary-key value. */
    template<typename T, typename K>
    auto delete_by_key(K const& k, dialect const& sql) -> statement {
        constexpr auto key = detail::primary_key_column<T>();
        auto const fragment = std::format("DELETE FROM {} WHERE {} = ?;",
                                          detail::mark_id(table_name_of<T>().view()),
                                          detail::mark_id(key.name.view()));
        return {detail::render(sql, fragment), {detail::to_param(k)}};
    }

    /** SELECT of one row by primary-key value. */
    template<typename T, typename K>
    auto load_by_key(K const& k, dialect const& sql) -> statement {
        constexpr auto key = detail::primary_key_column<T>();
        auto const fragment = std::format("SELECT {} FROM {} WHERE {} = ?;",
                                          detail::column_list(columns_of<T>()),
                                          detail::mark_id(table_name_of<T>().view()),
                                          detail::mark_id(key.name.view()));
        return {detail::render(sql, fragment), {detail::to_param(k)}};
    }

    // =====================================================================
    // SELECT of many rows
    // =====================================================================

    /**
     * A composable SELECT.
     *
     * ~~~{.cpp}
     * constexpr auto balance = field<^^Account::balance>;
     * auto rows = accounts.select()
     *                 .where(balance > 100.0)
     *                 .order_by(balance, sort::desc)
     *                 .limit(10)
     *                 .all();
     * ~~~
     *
     * `where` takes a `predicate<T>`, so a field belonging to a different entity
     * will not compile. Repeated `where` calls are ANDed together.
     */
    template<typename T>
    class query {
        connection* db_ = nullptr; ///< null when the query is only being assembled
        dialect const* sql_ = nullptr;
        std::optional<predicate<T>> where_{};
        std::vector<std::string> order_{};
        std::optional<int> limit_{};
        std::optional<int> offset_{};

        enum class projection { columns, count_star };

        [[nodiscard]] auto assemble(projection what) const -> statement {
            auto fragment =
                std::format("SELECT {} FROM {}",
                            what == projection::count_star ? std::string{"COUNT(*)"}
                                                           : detail::column_list(columns_of<T>()),
                            detail::mark_id(table_name_of<T>().view()));
            auto params = std::vector<param>{};
            if (where_) {
                fragment += std::format(" WHERE {}", where_->sql);
                params = where_->params;
            }
            // ORDER BY, LIMIT and OFFSET are meaningless against COUNT(*), which
            // collapses to a single row regardless; counting reports how many rows
            // match, not how many a paged query would return.
            if (what == projection::columns) {
                if (!order_.empty()) {
                    fragment += " ORDER BY ";
                    for (auto i = 0uz; i < order_.size(); ++i) {
                        if (i > 0) fragment += ", ";
                        fragment += order_[i];
                    }
                }
                if (limit_) {
                    fragment += " LIMIT ?";
                    params.push_back(std::format("{}", *limit_));
                }
                if (offset_) {
                    fragment += " OFFSET ?";
                    params.push_back(std::format("{}", *offset_));
                }
            }
            fragment += ";";
            return {detail::render(*sql_, fragment), std::move(params)};
        }

        [[nodiscard]] auto run(statement const& s) const -> result_set {
            if (db_ == nullptr)
                throw db_error{"tiny-orm: this query has no connection; it was built with "
                               "from<T>() rather than DAO<T>::select()"};
            return db_->execute(s);
        }

    public:
        query(connection* db, dialect const* sql) : db_{db}, sql_{sql} {}

        auto where(predicate<T> p) -> query& {
            where_ = where_ ? (*where_ && p) : p;
            return *this;
        }
        template<std::meta::info M>
        auto order_by(field_t<M> f, sort dir = sort::asc) -> query& {
            order_.push_back(std::format("{} {}", detail::mark_id(f.column.view()),
                                         dir == sort::desc ? "DESC" : "ASC"));
            return *this;
        }
        auto limit(int n) -> query& {
            limit_ = n;
            return *this;
        }
        auto offset(int n) -> query& {
            offset_ = n;
            return *this;
        }

        /// The SELECT this query would run. Works without a connection.
        [[nodiscard]] auto to_sql() const -> statement { return assemble(projection::columns); }

        /// The COUNT(*) this query would run. Works without a connection.
        [[nodiscard]] auto count_sql() const -> statement { return assemble(projection::count_star); }

        [[nodiscard]] auto all() const -> std::vector<T> {
            auto const rs = run(assemble(projection::columns));
            auto out = std::vector<T>{};
            out.reserve(rs.size());
            for (auto i = 0uz; i < rs.size(); ++i) out.push_back(detail::to_entity<T>(rs, i));
            return out;
        }

        /// The first matching row, if any. Asks the server for one row only.
        [[nodiscard]] auto first() const -> std::optional<T> {
            auto one = *this;
            one.limit_ = 1;
            auto const rs = run(one.assemble(projection::columns));
            if (rs.empty()) return {};
            return detail::to_entity<T>(rs, 0);
        }

        [[nodiscard]] auto count() const -> long {
            auto const rs = run(assemble(projection::count_star));
            if (rs.empty()) return 0;
            return detail::from_cell<long>(rs.at(0, 0));
        }
    };

    /**
     * A query with no connection, for generating SQL without a database.
     *
     * `DAO<T>::select()` is the everyday entry point; this one is for tests and
     * for inspecting generated SQL. Calling a terminal operation on it throws.
     */
    template<typename T>
    auto from(dialect const& sql) -> query<T> {
        return query<T>{nullptr, &sql};
    }

} // namespace tiny_orm
