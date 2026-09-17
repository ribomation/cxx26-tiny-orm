#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <tiny-orm/connection.hxx>
#include <tiny-orm/psql.hxx>
#include <tiny-orm/sqlite.hxx>

namespace tiny_orm {

    /**
     * Opens a connection, choosing the backend from the URL scheme.
     *
     *     postgresql://user:pass@host:5432/dbname     (postgres:// also accepted)
     *     sqlite:/path/to/file.db
     *     sqlite::memory:
     *
     * Throws `db_error` for an unknown scheme, and for a known scheme whose backend
     * was not compiled in -- naming which, so the message says what to install.
     */
    inline auto connect(std::string_view url) -> std::unique_ptr<connection> {
        auto const starts_with = [url](std::string_view prefix) { return url.starts_with(prefix); };

        if (starts_with("postgresql://") || starts_with("postgres://")) {
#ifdef TINY_ORM_HAS_POSTGRES
            return std::make_unique<psql_connection>(std::string{url});
#else
            throw db_error{"tiny-orm: this build has no PostgreSQL backend; install libpq "
                           "(apt: libpq-dev, yum: libpq-devel, apk: libpq-dev) and re-run CMake"};
#endif
        }

        if (starts_with("sqlite:")) {
#ifdef TINY_ORM_HAS_SQLITE
            // Everything after "sqlite:" is handed to sqlite3_open unchanged, so
            // both ":memory:" and a plain path work.
            return std::make_unique<sqlite_connection>(std::string{url.substr(7)});
#else
            throw db_error{"tiny-orm: this build has no SQLite backend; install libsqlite3 "
                           "(apt: libsqlite3-dev, yum: sqlite-devel, apk: sqlite-dev) and "
                           "re-run CMake"};
#endif
        }

        throw db_error{"tiny-orm: unrecognised connection URL '" + std::string{url}
                       + "'; expected postgresql://..., postgres://... or sqlite:..."};
    }

} // namespace tiny_orm
