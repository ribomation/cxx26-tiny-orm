#pragma once

/**
 * SQLite backend.
 *
 * Compiled only when CMake found libsqlite3; `TINY_ORM_HAS_SQLITE` is defined by
 * the build in that case. SQLite exists here mainly to keep the dialect
 * abstraction honest -- an interface with one implementation proves nothing --
 * and because `sqlite::memory:` gives the integration tests a real database with
 * no server, no container and no fixtures.
 */
#include <string>
#include <vector>

#include <tiny-orm/connection.hxx>
#include <tiny-orm/dialect.hxx>

#ifdef TINY_ORM_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace tiny_orm {

    /// Deliberately outside the libsqlite3 guard: rendering SQLite SQL needs no
    /// client library, so tests can assert on this dialect anywhere.
    class sqlite_dialect final : public dialect {
    public:
        [[nodiscard]] auto name() const -> std::string_view override { return "sqlite"; }

        [[nodiscard]] auto quote(std::string_view id) const -> std::string override {
            return std::format("\"{}\"", id);
        }
        /// SQLite numbers its parameters implicitly, so every placeholder is `?`.
        [[nodiscard]] auto placeholder(int) const -> std::string override { return "?"; }

        [[nodiscard]] auto type_name(type_kind k) const -> std::string_view override {
            switch (k) {
                case type_kind::boolean: return "INTEGER"; // no native boolean
                case type_kind::int16:
                case type_kind::int32:
                case type_kind::int64: return "INTEGER";
                case type_kind::float32:
                case type_kind::float64: return "REAL";
                case type_kind::text: return "TEXT";
                // SQLite has no date type; its own date functions read ISO-8601 text.
                case type_kind::timestamp: return "TEXT";
            }
            return "TEXT";
        }

        /**
         * Empty on purpose.
         *
         * A column declared exactly `INTEGER PRIMARY KEY` is an alias for the rowid
         * and already generates keys. The AUTOINCREMENT keyword only suppresses
         * rowid reuse, at the cost of an extra table, and SQLite rejects it anywhere
         * else -- so omitting it is both simpler and more widely valid.
         */
        [[nodiscard]] auto identity_clause() const -> std::string_view override { return {}; }

        [[nodiscard]] bool supports_returning() const override { return true; } // 3.35+
        [[nodiscard]] auto last_insert_id_sql() const -> std::string_view override {
            return "SELECT last_insert_rowid();";
        }

        /// SQLite rejects the keyword; dependent constraints go with the table anyway.
        [[nodiscard]] bool supports_drop_cascade() const override { return false; }

        /// SQLITE_MAX_VARIABLE_NUMBER, whose default is 32766 since SQLite 3.32.
        [[nodiscard]] auto max_bind_params() const -> int override { return 32766; }

        [[nodiscard]] bool is_integral_type(std::string_view raw) const override {
            for (auto const* known : {"INTEGER", "INT", "BIGINT", "SMALLINT"})
                if (raw == known) return true;
            return false;
        }
    };

#ifdef TINY_ORM_HAS_SQLITE

    /**
     * A connection over the SQLite C API.
     *
     * Unlike libpq there is no execute-with-parameters call, so each statement is
     * prepared, bound, stepped and finalised here. Values are bound and read as
     * text, matching `statement` and `result_set`; SQLite converts on the way in
     * and out according to the column's declared affinity.
     */
    class sqlite_connection final : public connection {
        sqlite3* db_ = nullptr;
        sqlite_dialect dialect_{};

    public:
        /// `path` may be a file name, or ":memory:" for a private in-memory database.
        explicit sqlite_connection(std::string const& path) {
            if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
                auto message = std::string{db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory"};
                sqlite3_close(db_);
                db_ = nullptr;
                throw db_error{"tiny-orm: cannot open SQLite database '" + path + "': " + message};
            }
            // Off by default, and the framework generates real foreign keys.
            sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
        }

        ~sqlite_connection() override {
            if (db_ != nullptr) sqlite3_close(db_);
        }

        [[nodiscard]] auto sql_dialect() const -> dialect const& override { return dialect_; }

        auto execute(statement const& s) -> result_set override {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, s.sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
                throw db_error{std::string{"tiny-orm: "} + sqlite3_errmsg(db_) + "  [sql: " + s.sql
                               + "]"};
            auto const guard = finalizer{stmt};

            for (auto i = 0uz; i < s.params.size(); ++i) {
                auto const n = static_cast<int>(i + 1);
                auto const& p = s.params[i];
                auto const rc = p ? sqlite3_bind_text(stmt, n, p->c_str(),
                                                      static_cast<int>(p->size()), SQLITE_TRANSIENT)
                                  : sqlite3_bind_null(stmt, n);
                if (rc != SQLITE_OK)
                    throw db_error{std::string{"tiny-orm: cannot bind parameter: "}
                                   + sqlite3_errmsg(db_)};
            }

            auto const fields = sqlite3_column_count(stmt);
            auto columns = std::vector<std::string>{};
            columns.reserve(static_cast<std::size_t>(fields));
            for (auto f = 0; f < fields; ++f) columns.emplace_back(sqlite3_column_name(stmt, f));

            auto rows = std::vector<std::vector<cell>>{};
            for (;;) {
                auto const rc = sqlite3_step(stmt);
                if (rc == SQLITE_DONE) break;
                if (rc != SQLITE_ROW)
                    throw db_error{std::string{"tiny-orm: "} + sqlite3_errmsg(db_) + "  [sql: "
                                   + s.sql + "]"};

                auto row = std::vector<cell>{};
                row.reserve(static_cast<std::size_t>(fields));
                for (auto f = 0; f < fields; ++f) {
                    if (sqlite3_column_type(stmt, f) == SQLITE_NULL) {
                        row.emplace_back();
                    } else {
                        auto const* text =
                            reinterpret_cast<char const*>(sqlite3_column_text(stmt, f));
                        row.emplace_back(text != nullptr ? text : "");
                    }
                }
                rows.push_back(std::move(row));
            }

            // A statement with no result columns reports how many rows it changed;
            // one with columns reports the rows it produced.
            if (fields == 0) return result_set::affected(sqlite3_changes64(db_));
            return result_set::of(std::move(columns), std::move(rows));
        }

        using connection::execute;

    private:
        /// Finalises a prepared statement on every exit path.
        class finalizer {
            sqlite3_stmt* stmt_;

        public:
            explicit finalizer(sqlite3_stmt* stmt) : stmt_{stmt} {}
            finalizer(finalizer const&) = delete;
            auto operator=(finalizer const&) -> finalizer& = delete;
            ~finalizer() { sqlite3_finalize(stmt_); }
        };
    };

#endif // TINY_ORM_HAS_SQLITE

} // namespace tiny_orm
