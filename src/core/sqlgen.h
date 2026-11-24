// Typed filters -> parameterised SQL. User text only ever travels as bound parameters;
// identifiers are validated against table metadata and delimited per dialect.
#pragma once

#include "core.h"

namespace dbp {

enum class Dialect { Sqlite, Postgres, MySql, MsSql };

struct Built {
    std::string sql;
    std::vector<Value> params;
};

std::string quote_ident(Dialect d, const std::string &name);
std::string qualified_table(Dialect d, const TableRef &t);
bool dialect_has_regex(Dialect d);

// Escapes %, _ (and the escape char '!') for LIKE ... ESCAPE '!'.
std::string like_escape(const std::string &s);

Built build_select(Dialect d, const TableMeta &m, const PageRequest &req, bool fetch_one_extra = true);
Built build_count(Dialect d, const TableMeta &m, const FilterGroup &f);
Built build_distinct(Dialect d, const TableMeta &m, const DistinctRequest &r);
// Statements for a change set, in execution order. Updates carry optimistic-concurrency checks.
struct Stmt {
    Built q;
    Change::Type type;
    bool must_affect_one = false; // zero affected rows => Conflict
};
std::vector<Stmt> build_changes(Dialect d, const TableMeta &m, const std::vector<Change> &changes);

// Display only (previews). Never executed.
std::string inline_params(Dialect d, const std::string &sql, const std::vector<Value> &params);
std::string render_literal(const Value &v);

} // namespace dbp
