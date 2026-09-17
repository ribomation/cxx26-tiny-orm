#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tiny-orm/connection.hxx>
#include <tiny-orm/ddl.hxx>
#include <tiny-orm/dialect.hxx>
#include <tiny-orm/dml.hxx>
#include <tiny-orm/mapping.hxx>

namespace tiny_orm {

    /// How a bulk insert is chopped into statements.
    struct bulk_options {
        /**
         * Rows per INSERT statement.
         *
         * Clamped down automatically when the row width would push the statement
         * past the engine's bind-parameter limit, so a wide entity is safe at the
         * default. Larger batches mean fewer round trips and more memory per
         * statement; 1000 is a reasonable middle for most row widths.
         */
        int batch_size = 1000;
    };

    /**
     * Runs raw SQL and materialises each row into `Row`.
     *
     * The escape hatch for everything the query builder deliberately does not do:
     * joins, GROUP BY, aggregates, window functions. `Row` is any plain struct --
     * it needs no annotations and is not an entity -- because materialisation
     * matches result columns to members by name, and does not care where the
     * struct came from.
     *
     * A free function rather than a member of `connection`, which is kept free of
     * reflection so backends stay simple.
     *
     * Member names are snake_cased the same way column names are, so SQL aliases
     * must match: a member `onTimePct` reads the column `on_time_pct`.
     *
     * ~~~{.cpp}
     * struct AirlineDelay { std::string airline{}; long flights{}; double avgDelay{}; };
     *
     * auto rows = select_into<AirlineDelay>(db, R"(
     *     SELECT a.name AS airline, count(*) AS flights, avg(f.delay) AS avg_delay
     *     FROM flights f JOIN airlines a ON a.id = f.airline_id
     *     GROUP BY a.name)");
     * ~~~
     *
     * Raw SQL is not portable: placeholders and functions are the engine's, not the
     * framework's.
     */
    template<typename Row>
    auto select_into(connection& db, std::string sql, std::vector<param> params = {})
        -> std::vector<Row> {
        auto const rs = db.execute(statement{std::move(sql), std::move(params)});
        auto out = std::vector<Row>{};
        out.reserve(rs.size());
        for (auto i = 0uz; i < rs.size(); ++i) out.push_back(detail::to_entity<Row>(rs, i));
        return out;
    }

    /// The first row of a raw query, if it returned any.
    template<typename Row>
    auto select_one_into(connection& db, std::string sql, std::vector<param> params = {})
        -> std::optional<Row> {
        auto const rows = select_into<Row>(db, std::move(sql), std::move(params));
        if (rows.empty()) return {};
        return rows.front();
    }

    /**
     * Builds a parameter list for a raw query.
     *
     * ~~~{.cpp}
     * select_into<Row>(db, "SELECT ... WHERE delay > $1 AND airline = $2", bind(15, "SAS"));
     * ~~~
     */
    template<typename... Vs>
    auto bind(Vs const&... vs) -> std::vector<param> {
        return {detail::to_param(vs)...};
    }

    /**
     * Stateless data access for one entity type.
     *
     * Holds a connection it does not own, so constructing one is free, several
     * DAOs naturally share a connection, and a transaction opened on that
     * connection spans all of them. The caller must keep the connection alive for
     * as long as the DAO.
     *
     * There is no persistence context: no identity map, no dirty checking, no
     * lazy loading. Entities are plain values -- you load a copy, change it, and
     * hand it back. Two loads of the same row give two unrelated objects.
     *
     * ~~~{.cpp}
     * auto accounts = DAO<Account>{db};
     * auto ada = Account{.ownerName = "Ada", .balance = 1000.0};
     * accounts.insert(ada);              // ada.id now holds the generated key
     * ada.balance += 250.0;
     * accounts.update(ada);
     * ~~~
     */
    template<typename T>
    class DAO {
        connection* db_;

    public:
        /**
         * The primary key's own type, recovered by reflection.
         *
         * No second template parameter to keep in sync, unlike JPA's
         * `CrudRepository<Account, Long>`: a mismatch is impossible rather than
         * merely detected.
         */
        using key_type = typename[:std::meta::type_of(detail::primary_key_member<T>()):];

        explicit DAO(connection& db) : db_{&db} {}

        // ---- schema -----------------------------------------------------

        void create(ddl_options opt = {}) {
            db_->execute(create_table_sql<T>(db_->sql_dialect(), opt));
        }
        void drop(drop_options opt = {}) {
            db_->execute(drop_table_sql<T>(db_->sql_dialect(), opt));
        }

        // ---- by primary key ---------------------------------------------

        /**
         * Inserts one entity, writing any server-generated key back into it.
         *
         * Takes a reference rather than returning the key so the common mistake --
         * inserting and then using the object with its key still unset -- cannot
         * happen. The cost is that a `const` entity cannot be inserted.
         */
        void insert(T& e) {
            auto const& sql = db_->sql_dialect();
            auto const rs = db_->execute(insert_object(e, sql));
            constexpr auto pk = detail::primary_key_member<T>();
            if constexpr (detail::describe(pk).auto_increment) {
                if (sql.supports_returning()) {
                    if (!rs.empty()) e.[:pk:] = detail::from_cell<key_type>(rs.at(0, 0));
                } else {
                    // No RETURNING: the key needs a second round trip. Valid only
                    // inside the same connection, which is why the dialect supplies
                    // the statement rather than the caller.
                    auto const back = db_->execute(std::string{sql.last_insert_id_sql()});
                    if (!back.empty()) e.[:pk:] = detail::from_cell<key_type>(back.at(0, 0));
                }
            }
        }

        /**
         * Inserts many entities, in as few statements as the engine allows.
         *
         * Rows are chopped into batches of `opt.batch_size`, each sent as a single
         * multi-row INSERT. The batch is clamped so one statement never exceeds the
         * dialect's bind-parameter budget: a 20-column entity on SQLite, for
         * instance, cannot carry more than 1638 rows however large a batch is asked
         * for.
         *
         * Generated keys are written back in order, which relies on RETURNING
         * yielding rows in VALUES order -- true of both supported engines, and
         * verified by the integration tests.
         *
         * No transaction is opened: a failure partway leaves earlier batches
         * committed. Wrap the call when that matters, which also makes the whole
         * load one commit:
         *
         * ~~~{.cpp}
         * auto tx = db.begin(tx_mode::auto_rollback);
         * flights.insert_all(rows);
         * tx.commit();
         * ~~~
         *
         * Returns the number of rows the server reported inserting.
         */
        auto insert_all(std::vector<T>& rows, bulk_options opt = {}) -> long {
            if (rows.empty()) return 0;

            auto const& sql = db_->sql_dialect();
            constexpr auto width = detail::bound_column_count<T>();
            static_assert(width > 0, "tiny-orm: an entity with no insertable column cannot be "
                                     "bulk inserted");

            auto const budget = std::max(1, sql.max_bind_params() / width);
            auto const batch = static_cast<std::size_t>(std::max(1, std::min(opt.batch_size, budget)));

            constexpr auto pk = detail::primary_key_member<T>();
            auto inserted = 0L;

            for (auto offset = 0uz; offset < rows.size(); offset += batch) {
                auto const count = std::min(batch, rows.size() - offset);
                auto const chunk = std::span<const T>{rows.data() + offset, count};
                auto const rs = db_->execute(insert_many<T>(chunk, sql));

                if constexpr (detail::describe(pk).auto_increment) {
                    if (sql.supports_returning()) {
                        for (auto i = 0uz; i < rs.size() && i < count; ++i)
                            rows[offset + i].[:pk:] = detail::from_cell<key_type>(rs.at(i, 0));
                    }
                }
                inserted += sql.supports_returning() ? static_cast<long>(rs.size())
                                                     : rs.affected_rows();
            }
            return inserted;
        }

        /// The row with this key, or nothing. A missing row is not an error.
        [[nodiscard]] auto load(key_type const& k) const -> std::optional<T> {
            auto const rs = db_->execute(load_by_key<T>(k, db_->sql_dialect()));
            if (rs.empty()) return {};
            return detail::to_entity<T>(rs, 0);
        }

        /// Returns the affected row count, so callers can notice a no-op update.
        auto update(T const& e) -> long {
            return db_->execute(update_by_object(e, db_->sql_dialect())).affected_rows();
        }

        /// Returns the affected row count; zero means there was nothing to delete.
        auto remove(key_type const& k) -> long {
            return db_->execute(delete_by_key<T>(k, db_->sql_dialect())).affected_rows();
        }

        [[nodiscard]] bool exists(key_type const& k) const {
            return select().where(field<detail::primary_key_member<T>()> == k).count() > 0;
        }

        [[nodiscard]] auto count() const -> long { return select().count(); }

        // ---- queries ----------------------------------------------------

        /// A query bound to this DAO's connection, so its terminals can execute.
        [[nodiscard]] auto select() const -> query<T> {
            return query<T>{db_, &db_->sql_dialect()};
        }
    };

} // namespace tiny_orm
