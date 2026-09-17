# Finding 04 — Importing real data

*Recorded 2026-09-14 · OpenFlights reference data, synthetic flight telemetry*

CSV is not a format. It is a family of conventions that happen to share commas,
and every assumption you make about it is wrong for somebody's file. These are
the ones that bit while building the importer against two real sources.

The importer was written first against invented data and passed everything. Each
item below came from a real file afterwards.

## 1. A record is not a line

A quoted field may contain the delimiter, doubled quotes, **and newlines**. So
`std::getline` is the wrong primitive: it splits a two-line quoted value into two
malformed records, and does it silently.

```
1,"Lovelace, Ada",1.0,"She said ""hi"""
2,"two
lines",2.0,x
```

`read_record` therefore consumes characters and tracks quote state, returning
when it sees a newline *outside* a quoted field. Real data needs this: OpenFlights
names include `"Winnipeg / St. Andrews Airport"` and plenty of embedded commas.

## 2. Comment lines must go before parsing, not after

A flight-telemetry file from another project leads with a preamble:

```
# Synthetic flight simulator data. ENU [m], velocity [m/s], attitude [degrees].
time_s,x_m,y_m,z_m,vx_ms,vy_ms,vz_ms,roll_deg,pitch_deg,yaw_deg
```

The obvious implementation — parse the record, then discard it if the first field
starts with `#` — works on that line and fails on this one:

```
# units: "m/s, degrees
```

One unbalanced quote puts the parser into a quoted field, and it swallows every
following line until it finds another quote. The fix is to drop comment lines at
the character level, before a record begins.

Related: comment handling is **off by default**, because `#` is a legal first
character of a data field. A reader that assumes comments corrupts the file that
contains `#1 pick`.

## 3. Every producer invents its own null

CSV has no null, so each source picks something:

| Source | Writes |
|---|---|
| OpenFlights | `\N` — 2183 airports, 898 routes |
| PostgreSQL `COPY` | `\N` |
| Most spreadsheet exports | empty |
| Others seen in the wild | `NULL`, `NA`, `-` |

Hence a configurable `null_marker`. But the more interesting half is what needs
no configuration: **an empty field becomes NULL only where the member is a
`std::optional`.**

```cpp
auto const null = (!opt.null_marker.empty() && raw == opt.null_marker)
                  || (raw.empty() && is_optional_v<V>);
```

So a CSV cannot quietly turn a NOT NULL text column into a null, and a
legitimately empty string survives. The type already carries the answer; asking
the user to restate it per column would be asking them to repeat themselves.

## 4. Booleans do not agree with anything

`from_cell<bool>` accepted `t`, `true` and `1` — which is precisely the set you
arrive at by thinking about what a *database* returns.

Then `airlines.dat` turned out to write `Y` and `N`, and all 6162 airlines
silently loaded as inactive. No error: `"Y"` is simply not `"t"`.

Now `t/true/1/y/yes`, case-insensitively. There is no principled place to stop —
this is a list of observed spellings, not a specification.

## 5. Headerless files exist, and they dictate struct order

The OpenFlights `.dat` files carry no header, so fields map positionally and
**member declaration order must match the file's column order**.

That has one awkward consequence. A generated key has no column in the file, so
it must be declared *last*, where positional mapping simply runs out of columns
and leaves it at zero for the server to fill:

```cpp
struct [[=Table{.name = "routes"}]] Route {
    [[=Column{.name = "airline_code"}]] std::string airlineCode{};
    ...
    [[=PrimaryKey{.auto_increment = true}]] long id{};   // not in the file
};
```

It works, and it is the one place in the project where a struct's shape is
dictated by a file rather than by what reads well.

## 6. Real data is not referentially clean

Declaring foreign keys on `routes` and loading the file fails. The data contains
109 source and 112 destination airport IDs that `airports.dat` does not define,
plus 479 routes with no airline ID at all — **1347 routes, 2.0%**.

The tempting fix is to drop the constraints so the load succeeds. The loader
instead filters those rows and reports the count, because 2% is a property of the
data worth knowing rather than an obstacle worth hiding.

## 7. The documentation is stale before you read it

The OpenFlights page states "over 10,000" airports. The file has **7698**. The
page is not lying; it is dated, and the upstream files change without notice and
carry no version of their own.

So the fetch script writes a `MANIFEST` recording rows, bytes and fetch time. The
number you can cite is the one you measured.

## The rule

> An importer written against data you invented will pass every test you think
> to write. It has only been tested against your imagination.

Every item here survived a green test suite and was found by pointing the code at
a file somebody else produced. See also
[Finding 03](findings-03-second-backend.md), which is the same lesson one level
up: a dialect written against one engine is a guess about engines.
