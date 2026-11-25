// Pasting spreadsheet text as new rows. Parsing and validation are pure: nothing touches the change set
// until every cell has been checked and the user has seen the result.
#pragma once

#include <string>
#include <vector>

#include "core.h"

namespace dbp {

constexpr size_t kMaxPasteRows = 2000;
constexpr size_t kMaxPasteBytes = 4u << 20;

// Tab-separated text as spreadsheets copy it: quoted cells may hold tabs, quotes ("") and line breaks.
// A trailing line break does not create an empty row. Returns false (and sets *err) when the text is
// too large, has too many rows, or has an unterminated quote.
bool parse_tsv(const std::string &text, std::vector<std::vector<std::string>> &out, std::string *err);

struct PasteIssue {
    size_t row = 0; // 0-based pasted row
    size_t col = 0; // index into the target column list
    std::string message;
};

struct PastePlan {
    std::vector<std::vector<Value>> values; // one entry per pasted row, one Value per target column
    std::vector<PasteIssue> issues;
    bool ok() const { return issues.empty() && !values.empty(); }
};

// Converts the cells with the same rules as typing into the grid. `targets` are real column indexes of
// `cols`. Extra pasted cells beyond targets.size() are an issue; missing ones are left to column defaults
// by the caller.
PastePlan plan_paste(const std::vector<ColumnInfo> &cols, const std::vector<size_t> &targets, const std::vector<std::vector<std::string>> &cells);

} // namespace dbp
