# cxx26-tiny-orm

An experiment with **C++26 static reflection** (P2996): how much of an ORM can be
derived from plain aggregate structs, with no macros, no code generation, and no
registration step?

**Status: complete.** The answer is *most of it*. What follows is the result, the
API it produced, and what it cost.

---

## The question

Traditional C++ ORMs pay for the mapping between classes and tables in one of
three currencies: intrusive macros (`REGISTER_FIELD(User, name)`), a code
generator run over annotated headers, or hand-written boilerplate per entity. All
three duplicate information the compiler already has.

C++26 reflection makes that information available to the program itself. So:

> Can a plain aggregate struct — no macros, no base class, no registration — be
> enough for a library to derive its table name, column list, DDL, parameterised
> CRUD statements, and the row ⇄ object marshalling, entirely at compile time?

## What the experiment established

**Yes, and further than expected.** An entity is an ordinary struct:

```cpp
struct [[=Table{.name = "bank_accounts"}]] Account {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    [[=Column{.name = "owner", .nullable = Nullable::no}]] std::string ownerName{};
    double balance{};
    std::optional<std::string> nickName{};      // nullable, because the type says so
};
```

From that alone the framework derives the table name (`snake_case` plus
pluralisation), column names, SQL types, nullability, keys and foreign keys —
and annotations only ever state what they *override*.

Four results are worth calling out:

- **One descriptor, three unrelated consumers.** The same `column_info` emits the
  `CREATE TABLE`, matches CSV header columns to members, and builds parameterised
  SQL. Nothing is declared twice. That reflection describes the *type* rather than
  serving SQL specifically is the strongest evidence the approach generalises.
- **The compile-time / run-time seam is real.** The entire run-time half — five
  of the thirteen framework headers — contains no reflection at all.
- **Mistakes are compile errors.** A field from the wrong entity, a value of the
  wrong type, an entity with no primary key, an `auto_increment` on a non-integer
  column: all rejected at compile time, by ordinary overload resolution or a named
  `static_assert`.
- **The limits cost workarounds, not features.** Everything that got in the way is
  written up in [`docs/`](docs/README.md); none of it proved fatal.

Roughly 2,500 lines of framework across 13 headers, 88 tests, two database
backends.

---

## Requirements

| | Needed for | Notes |
|---|---|---|
| **GCC 16.x** | everything | Reflection is experimental and behind `-freflection`. Developed against 16.1.0. |
| **CMake 4.1+** | everything | The version `CMakeLists.txt` asks for; verified against 4.2.3. |
| **libpq** | PostgreSQL backend | optional |
| **libsqlite3** | SQLite backend | optional |
| **Docker** | the PostgreSQL container | optional |

Both backends are optional: without either, the library still builds and the
whole unit-test suite still runs, because those tests use a fake connection.
CMake reports what is missing and how to install it.

### Installing the dependencies

```sh
# Debian / Ubuntu
sudo apt install libpq-dev libsqlite3-dev

# Fedora / RHEL
sudo yum install libpq-devel sqlite-devel

# Alpine
sudo apk add libpq-dev sqlite-dev
```

GCC 16 is not yet packaged by most distributions; a source build or a nightly
toolchain is needed. This project was developed against `/opt/gcc-16.1`.

Catch2 v3.16.0 is fetched automatically by CMake — nothing to install.

The toolchain reports:

```
__cpp_impl_reflection      202603L
__cpp_expansion_statements 202506L
```

The second matters as much as the first: `template for` is how the framework
walks an entity's members.

## Building

```sh
cmake -S . -B build -DCMAKE_CXX_COMPILER=/opt/gcc-16.1/bin/g++
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## The API

### Defining entities

Plain structs. Annotations are optional and state only overrides.

```cpp
struct [[=Table{.name = "txn"}]] Transaction {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    [[=ForeignKey{.target = ^^Account, .on_delete = FkAction::cascade}]] long accountId{};
    double amount{};
    std::chrono::system_clock::time_point bookedAt{};
    std::optional<std::string> memo{};
};
```

| Annotation | Overrides |
|---|---|
| `Table{.name}` | the table name, otherwise `snake_case` + pluralised |
| `Column{.name, .sql_type, .nullable, .default_value}` | column name, type, nullability, DEFAULT |
| `PrimaryKey{.sql_type, .auto_increment}` | marks the key; composes with `Column` |
| `ForeignKey{.target, .column, .on_delete}` | `target` is a *reflection of the entity type*, so the referenced table stays single-sourced |

C++ types map to `boolean`, `int16/32/64`, `float32/64`, `text` and `timestamp`;
`std::optional<T>` means nullable.

### Connecting

```cpp
auto db = tiny_orm::connect("postgresql://orm:orm@localhost:5432/tiny_orm_demo");
auto db = tiny_orm::connect("sqlite::memory:");
auto db = tiny_orm::connect("sqlite:/path/to/file.db");
```

### Data access

```cpp
auto accounts = DAO<Account>{*db};       // key type recovered by reflection

accounts.create();                       // CREATE TABLE from the struct
accounts.drop();

auto ada = Account{.ownerName = "Ada Lovelace", .balance = 1000.0};
accounts.insert(ada);                    // ada.id now holds the generated key

auto found  = accounts.load(ada.id);     // std::optional<Account>
auto n      = accounts.update(ada);      // affected row count
auto gone   = accounts.remove(ada.id);   // affected row count
bool here   = accounts.exists(ada.id);
long total  = accounts.count();

accounts.insert_all(rows);               // batched multi-row INSERT
accounts.insert_all(rows, {.batch_size = 500});
```

No persistence context: no identity map, no dirty checking, no lazy loading.
Entities are values — load a copy, change it, hand it back.

### Queries

Columns are named by a reflection of the member, so nothing is repeated as a
string and mistakes do not compile.

```cpp
constexpr auto balance = field<^^Account::balance>;
constexpr auto owner   = field<^^Account::ownerName>;
constexpr auto nick    = field<^^Account::nickName>;

auto rich = accounts.select()
                .where(balance > 1000.0 && owner.like("A%"))
                .order_by(balance, sort::desc)
                .limit(10)
                .all();

auto one   = accounts.select().where(owner == "Ada").first();   // std::optional
auto many  = accounts.select().where(nick.is_null()).count();
auto some  = accounts.select().where(owner.in({"Ada", "Grace"})).all();

accounts.select().where(balance > 100.0).to_sql();   // inspect without executing
```

Operators: `== != < <= > >=`, `.like()`, `.in()`, `.is_null()`, `.is_not_null()`,
combined with `&&`, `||`, `!`.

### Raw SQL, typed

For joins, `GROUP BY`, aggregates and window functions — everything the builder
deliberately does not do. `Row` is any plain struct; columns match members by
name.

```cpp
struct OwnerTotal { std::string owner{}; long accounts{}; double totalBalance{}; };

auto totals = select_into<OwnerTotal>(*db, R"(
    SELECT "owner" AS owner, count(*) AS accounts, sum("balance") AS total_balance
    FROM bank_accounts GROUP BY "owner" HAVING count(*) > 1
    ORDER BY total_balance DESC)");

auto hits = select_into<Row>(*db, "... WHERE delay > $1 AND name = $2", bind(15, "SAS"));
```

SQL aliases must be `snake_case`: members are snake_cased the same way columns
are, so `totalBalance` reads `total_balance`.

### Transactions

RAII, with the failure mode chosen explicitly.

```cpp
{
    auto tx = db->begin(tx_mode::auto_rollback);   // destructor rolls back
    txns.insert(coffee);
    accounts.update(ada);
    tx.commit();
}
```

`tx_mode::auto_commit` commits on scope exit instead; the destructor cannot
report a failure, so `auto_rollback` plus an explicit `commit()` is the safer
pairing.

### CSV import

The same descriptor that emits the DDL matches the CSV header.

```cpp
auto rows = read_csv_file<Account>("accounts.csv");
auto rows = read_csv_file<Airport>(path, {.has_header = false,      // positional
                                          .null_marker = "\\N",
                                          .comment = '#'});

for_each_csv<Flight>(in, [&](Flight&& f) { ... });   // streaming, for large files
```

Whether an empty field means NULL is decided by the member's type, not by
configuration.

---

## Demo

```sh
./scripts/fetch-openflights.sh                  # ~4 MB into data/openflights/
docker compose -f docker/docker-compose.yml up -d
./build/openflights-app                         # PostgreSQL, per the compose file
./build/openflights-app "sqlite::memory:"       # same binary, no server
```

Creates four tables from the structs, loads 7.7k airports / 6.2k airlines / 66k
routes, then answers questions through both `select_into` and the typed query
builder:

```
  read  airports.dat     7698 rows    162 ms
  load  routes          66316 rows   9240 ms
  skip                   1347 routes with dangling references (2.0%)

Busiest airports by departing routes
  Hartsfield Jackson Atlanta International Airport  United States   915 routes to 217 airports
  Chicago O'Hare International Airport              United States   558 routes to 206 airports

Highest airports, via the typed query builder
  Daocheng Yading Airport                China             14472 ft
```

About 2% of routes reference airports the airports file does not contain; the
loader filters those rather than dropping the foreign keys.

A second executable prints the generated SQL for both dialects without touching a
database:

```sh
./build/sql-generation-example
```

## Databases

Two, kept apart because both the demo and the integration tests create and drop
tables:

- `tiny_orm_demo` — the OpenFlights demo
- `tiny_orm_test` — the integration tests

Both on `localhost:5432`, user `orm`, password `orm` — development-only
credentials, created by `initdb` on first start.

```sh
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml exec db psql -U orm -d tiny_orm_demo
docker compose -f docker/docker-compose.yml down      # keeps the volume
docker compose -f docker/docker-compose.yml down -v   # wipes it
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```

- **`tests/unit`** — 83 cases, no database at all; everything runs against a
  recording fake connection.
- **`tests/integration`** — one shared scenario file run against every backend
  present: SQLite in memory and on disk always, PostgreSQL when the container is
  up. It skips itself with instructions when it is not.

---

## Layout

```
CMakeLists.txt              build definition; sets C++26 and -freflection
src/tiny-orm/               the framework headers
src/openflights-app.cxx     the demo application
src/openflights-schema.hxx  its entity definitions
src/sql-generation-example.cxx  prints generated SQL, no database needed
tests/unit/                 no database required
tests/integration/          real engines
scripts/                    data fetching
docker/                     PostgreSQL instance for the run-time half
docs/                       findings, including the dead ends
```

### The framework, by phase

The split the experiment is built around is visible in the headers: five of the
thirteen contain no reflection at all, and the ones that do are concentrated in
the compile-time half.

**Compile time — reflection turns entity structs into descriptors**

| Header | What it does |
|---|---|
| `compiletime-string.hxx` | A string that survives constant evaluation: structural, so it can be a template argument or an annotation payload. The foundation everything else stands on — see [Finding 01](docs/findings-01-compile-time-strings.md). |
| `annotations.hxx` | `Table`, `Column`, `PrimaryKey`, `ForeignKey` and their enums. Every field optional: an annotation states only what it *overrides*. |
| `schema.hxx` | The heart. Walks an entity's members and resolves each into a `column_info` — name, canonical `type_kind`, nullability, keys, foreign keys — applying annotations on top of reflected defaults. |

**The seam — templates whose bodies span both phases**

| Header | What it does |
|---|---|
| `mapping.hxx` | Both directions of the value boundary: `to_entity` writes result columns into members through splices, `from_cell` parses text into a member's type. |
| `ddl.hxx` | `CREATE TABLE` and `DROP TABLE`, with primary and foreign keys as named table-level constraints. |
| `dml.hxx` | `INSERT`/`UPDATE`/`DELETE`/`SELECT`, multi-row insert, and the composable query builder. |
| `dao.hxx` | `DAO<T>`, `select_into`, `insert_all`. The key type is recovered by reflecting the `PrimaryKey` member. |
| `csv.hxx` | Reads CSV into entities, matching header columns to members by the same names the DDL uses. |

**Run time — no reflection at all**

| Header | What it does |
|---|---|
| `connection.hxx` | `statement`, `result_set`, `param`, `db_error`, the abstract `connection`, and `transaction` — built purely on `execute`, so a backend needs no extra virtuals. |
| `dialect.hxx` | The nine things that differ between engines: quoting, placeholders, type rendering, identity clause, `RETURNING`, last-insert-id, drop-cascade, bind-parameter limit, integral overrides. |
| `psql.hxx` | PostgreSQL over libpq. `statement` is already `PQexecParams`' shape. |
| `sqlite.hxx` | SQLite over its C API. Keeps the dialect abstraction honest and gives tests a real engine with no server. |
| `connect.hxx` | `connect(url)` — picks a backend by scheme, and says what to install when one was not compiled in. |

Both backends keep their *dialect* outside the `#ifdef` guard: rendering an
engine's SQL needs no client library, so every SQL assertion in the test suite
runs on a machine with neither installed.

---

## Findings

Written up in [`docs/`](docs/README.md) — that index says which to read first.

- [Finding 01 — Carrying strings through compile time](docs/findings-01-compile-time-strings.md)
- [Finding 02 — Reflection gotchas](docs/findings-02-reflection-gotchas.md)
- [Finding 03 — What the second backend found](docs/findings-03-second-backend.md)
- [Finding 04 — Importing real data](docs/findings-04-importing-real-data.md)

01 and 02 are about C++26 and would differ on another compiler or in another
year. 03 and 04 are not about C++26 at all.

## What was deliberately not built

Scope was bounded by the question. Left out on purpose:

- **A persistence context** — no identity map, dirty checking or lazy loading.
  Value semantics fit C++ better, and the plumbing would have demonstrated nothing
  about reflection.
- **`GROUP BY` in the query builder** — aggregates break the `DAO<T>`-returns-`T`
  contract; `select_into` covers the same ground.
- **Joins in the builder**, inheritance mapping, a third backend, and compile-time
  parsed finder names (`findByOwnerAndBalanceGreaterThan` as a string NTTP, parsed
  and type-checked at compile time). The last of those is the most interesting
  thing left undone.

## Authors

- **Jens Riboe** — author.
- **Claude Opus 5** (Anthropic) — co-author. The framework was built in an
  interactive session with Claude Code; commits carry a `Co-Authored-By` trailer
  naming which model wrote them.

## References

- [P2996 — Reflection for C++26](https://wg21.link/p2996)
- [P3394 — Annotations for reflection](https://wg21.link/p3394)
- [P3491 — `define_static_{string,object,array}`](https://wg21.link/p3491)
- [GCC C++ standards status](https://gcc.gnu.org/projects/cxx-status.html) — the
  Reflection row, recording P2996R13 landing in GCC 16 behind `-freflection`
- [GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html#cxx) — the release
  notes for that support
- [OpenFlights data](https://openflights.org/data.php) — demo data, Open Database
  License (ODbL)
