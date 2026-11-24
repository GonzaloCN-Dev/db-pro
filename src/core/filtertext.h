// Turns what a user types in a column filter box (plus the chosen operator) into a typed Condition.
#pragma once

#include <optional>

#include "core.h"

namespace dbp {

// Operators offered for a column, given its type and the engine's capabilities. First = default.
std::vector<Op> ops_for(const ColumnInfo &c, const Capabilities &caps);
// Short label for the compact operator button in the filter row.
const char *op_symbol(Op op);

// Parse user text into a typed value for the column. On failure returns nullopt and sets *err.
std::optional<Value> parse_typed(TypeClass cls, const std::string &text, std::string *err);

// Build a condition. Returns nullopt with *err set when the text is invalid for the operator;
// returns nullopt with empty *err when there is nothing to filter (blank text on an operator needing a value).
std::optional<Condition> make_condition(const ColumnInfo &c, Op op, const std::string &text, std::string *err);

// Evaluate a condition against already-loaded values (client-side, partial filtering).
bool matches_local(const Condition &c, const Value &v);

std::string trim(const std::string &s);

} // namespace dbp
