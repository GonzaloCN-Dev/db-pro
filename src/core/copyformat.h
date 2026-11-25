// Clipboard text for a block of rows. CSV/TSV/JSON reuse the exporter's cell rules so what is copied
// matches what is exported; INSERT statements use the dialect's identifier quoting.
#pragma once

#include <string>
#include <vector>

#include "core.h"
#include "sqlgen.h"

namespace dbp {

enum class CopyFormat { Tsv, Csv, Json, SqlInsert };

struct CopyResult {
    std::string text;
    bool truncated = false; // the byte cap was reached; text holds only the rows that fit
    size_t rows = 0;
};

// Copying is bounded: a clipboard is not an export. Rows beyond max_bytes are left out and reported.
CopyResult format_rows(const std::vector<ColumnInfo> &cols, const std::vector<std::vector<Value>> &rows, CopyFormat format,
                       const std::string &table, Dialect dialect, bool header, size_t max_bytes = 8u << 20);

} // namespace dbp
