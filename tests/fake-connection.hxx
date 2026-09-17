#pragma once

#include <tiny-orm/connection.hxx>
#include <tiny-orm/psql.hxx>

#include <string>
#include <vector>

/**
 * A connection that executes nothing.
 *
 * Records every statement it was handed and replays whatever the test queued.
 * Because `transaction` is built on `connection::execute`, BEGIN, COMMIT and
 * ROLLBACK show up here as ordinary statements and can be asserted on.
 */
struct fake_connection final : tiny_orm::connection {
    /// PostgreSQL, so the expected SQL in these tests reads as the primary target.
    tiny_orm::psql_dialect dialect{};
    std::vector<tiny_orm::statement> executed{};
    std::vector<tiny_orm::result_set> queued{};
    std::size_t next = 0;
    bool fail_next = false;

    [[nodiscard]] auto sql_dialect() const -> tiny_orm::dialect const& override { return dialect; }

    auto execute(tiny_orm::statement const& s) -> tiny_orm::result_set override {
        executed.push_back(s);
        if (fail_next) {
            fail_next = false;
            throw tiny_orm::db_error{"fake: forced failure"};
        }
        if (next < queued.size()) return queued[next++];
        return tiny_orm::result_set::affected(1);
    }

    /// SQL of the statement at `i`, for readable assertions.
    [[nodiscard]] auto sql(std::size_t i) const -> std::string const& { return executed.at(i).sql; }

    [[nodiscard]] auto all_sql() const -> std::vector<std::string> {
        auto out = std::vector<std::string>{};
        for (auto const& s : executed) out.push_back(s.sql);
        return out;
    }
};
