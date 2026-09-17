#pragma once

#include <tiny-orm/annotations.hxx>

#include <chrono>
#include <optional>
#include <string>

using namespace tiny_orm;

struct [[=Table{.name = "bank_accounts"}]] Account {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    [[=Column{.name = "owner", .nullable = Nullable::no}]] std::string ownerName{};
    double balance{};
    std::optional<std::string> nickName{};
};

struct Transaction {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    [[=ForeignKey{.target = ^^Account, .on_delete = FkAction::cascade}]] long accountId{};
    double amount{};
    std::optional<std::string> memo{};
};

/// No auto_increment, to exercise the non-generated-key path.
struct [[=Table{.name = "ledgers"}]] Ledger {
    [[=PrimaryKey{}]] long id{};
    std::string label{};
};

/// Exercises the timestamp kind, which is text on SQLite and TIMESTAMP on PostgreSQL.
struct [[=Table{.name = "readings"}]] Reading {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    std::chrono::system_clock::time_point takenAt{};
    std::optional<std::chrono::system_clock::time_point> settledAt{};
    double value{};
};

/// A boolean column, which the two engines store in very different ways.
struct [[=Table{.name = "switches"}]] Switch {
    [[=PrimaryKey{.auto_increment = true}]] long id{};
    std::string label{};
    bool active{};
};

struct Category {}; // name derivation only
struct Address {};
