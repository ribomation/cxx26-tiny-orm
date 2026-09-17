#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tiny-orm/dialect.hxx>

namespace tiny_orm {

    /// Anything the database layer refuses to do.
    class db_error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * A NULL-able bound parameter.
     *
     * PostgreSQL's wire protocol passes parameters and returns results as text,
     * so text is the native carrier rather than a lossy convenience. `nullopt` is
     * a genuine SQL NULL, never the string "null".
     */
    using param = std::optional<std::string>;

    /// One cell of a result row; `nullopt` is SQL NULL.
    using cell = std::optional<std::string>;

    /**
     * A parameterised statement: SQL carrying $1, $2, ... placeholders, plus the
     * values to bind. Values are never interpolated into the SQL text.
     */
    struct statement {
        std::string sql;
        std::vector<param> params;
    };

    /**
     * What a connection hands back.
     *
     * Row-returning statements fill `columns` and `rows`; INSERT, UPDATE and
     * DELETE report `affected_rows` instead.
     */
    class result_set {
        std::vector<std::string> columns_{};
        std::vector<std::vector<cell>> rows_{};
        long affected_ = 0;

    public:
        result_set() = default;

        /// Rows returned by a SELECT.
        static auto of(std::vector<std::string> columns, std::vector<std::vector<cell>> rows)
            -> result_set {
            auto rs = result_set{};
            rs.columns_ = std::move(columns);
            rs.rows_ = std::move(rows);
            rs.affected_ = static_cast<long>(rs.rows_.size());
            return rs;
        }

        /// Row count reported by a statement that returns no rows.
        static auto affected(long n) -> result_set {
            auto rs = result_set{};
            rs.affected_ = n;
            return rs;
        }

        [[nodiscard]] auto size() const -> std::size_t { return rows_.size(); }
        [[nodiscard]] bool empty() const { return rows_.empty(); }
        [[nodiscard]] auto affected_rows() const -> long { return affected_; }
        [[nodiscard]] auto columns() const -> std::vector<std::string> const& { return columns_; }

        [[nodiscard]] auto index_of(std::string_view column) const -> std::optional<std::size_t> {
            for (auto i = 0uz; i < columns_.size(); ++i)
                if (columns_[i] == column) return i;
            return {};
        }

        /// Lookup by column name, as JDBC does.
        [[nodiscard]] auto at(std::size_t row, std::string_view column) const -> cell const& {
            auto const i = index_of(column);
            if (!i)
                throw db_error{"tiny-orm: result set has no column '" + std::string{column} + "'"};
            return at(row, *i);
        }

        [[nodiscard]] auto at(std::size_t row, std::size_t column) const -> cell const& {
            if (row >= rows_.size() || column >= rows_[row].size())
                throw db_error{"tiny-orm: result set index out of range"};
            return rows_[row][column];
        }
    };

    /// What a transaction's destructor should do if neither commit nor rollback was called.
    enum class tx_mode { auto_commit, auto_rollback };

    class transaction;

    /**
     * A database connection.
     *
     * Abstract so tests can substitute a double: `execute` is the only operation a
     * backend has to implement, and everything else -- including transactions --
     * is expressed in terms of it.
     */
    class connection {
    public:
        connection() = default;
        connection(connection const&) = delete;
        auto operator=(connection const&) -> connection& = delete;
        virtual ~connection() = default;

        virtual auto execute(statement const& s) -> result_set = 0;

        /// The SQL dialect this connection speaks. Driver and dialect always
        /// travel together, so the connection is where the dialect lives.
        [[nodiscard]] virtual auto sql_dialect() const -> dialect const& = 0;

        /// Convenience for SQL with no parameters.
        auto execute(std::string sql) -> result_set { return execute(statement{std::move(sql), {}}); }

        [[nodiscard]] auto begin(tx_mode mode) -> transaction;
    };

    /**
     * Scope-bound transaction.
     *
     * Implemented entirely through `connection::execute`, so a backend needs no
     * extra virtuals and a test double gets transactions for free -- a mock can
     * assert on the BEGIN, COMMIT and ROLLBACK it was asked for.
     */
    class transaction {
        connection* db_; ///< null once finished, or moved from
        tx_mode mode_;

        friend class connection;
        transaction(connection& db, tx_mode mode) : db_{&db}, mode_{mode} { db_->execute("BEGIN;"); }

    public:
        transaction(transaction const&) = delete;
        auto operator=(transaction const&) -> transaction& = delete;
        transaction(transaction&& other) noexcept
            : db_{std::exchange(other.db_, nullptr)}, mode_{other.mode_} {}
        auto operator=(transaction&&) -> transaction& = delete;

        /**
         * Applies the chosen mode if neither commit() nor rollback() was called.
         *
         * Any failure is swallowed, because a destructor has nowhere to report
         * one. That is the real cost of `tx_mode::auto_commit`: a commit failing on
         * scope exit disappears, whereas a failed rollback leaves the server to
         * clean up when the connection drops. Prefer `auto_rollback` with an
         * explicit commit() unless you have a reason not to.
         */
        ~transaction() {
            if (db_ == nullptr) return;
            try {
                db_->execute(mode_ == tx_mode::auto_commit ? "COMMIT;" : "ROLLBACK;");
            } catch (...) { // NOLINT: a destructor must not propagate
            }
        }

        void commit() { finish("COMMIT;"); }
        void rollback() { finish("ROLLBACK;"); }

        [[nodiscard]] bool active() const { return db_ != nullptr; }

    private:
        void finish(std::string_view sql) {
            if (db_ == nullptr) throw db_error{"tiny-orm: transaction is already finished"};
            auto* const db = std::exchange(db_, nullptr);
            db->execute(std::string{sql});
        }
    };

    inline auto connection::begin(tx_mode mode) -> transaction { return transaction{*this, mode}; }

} // namespace tiny_orm
