// Streaming exporters. Rows arrive in bounded batches from Connector::stream_rows and are
// written straight to disk, so memory use does not depend on the size of the result.
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>

#include "core.h"

namespace dbp {

enum class ExportFormat { Csv, Tsv, Json, Ndjson, SqlInsert };

struct ExportOptions {
    ExportFormat format = ExportFormat::Csv;
    bool header = true;
    bool bom = true;           // UTF-8 BOM so Excel opens accents correctly (CSV/TSV)
    char delimiter = ',';      // CSV only
    std::string table_name;    // SqlInsert
    std::string null_text;     // CSV: how NULL is written; empty keeps NULL distinct only if set
};

const char *export_extension(ExportFormat f);

class RowWriter {
public:
    virtual ~RowWriter() = default;
    virtual void begin(const std::vector<ColumnInfo> &cols) = 0;
    virtual void rows(const std::vector<std::vector<Value>> &batch) = 0;
    virtual void end() = 0;
};

// Throws DbError(Other) when the file cannot be created or written.
std::unique_ptr<RowWriter> make_writer(const std::filesystem::path &path, const ExportOptions &o);

// CSV cell with formula-injection neutralisation (=, +, -, @ at the start of a text value).
std::string csv_escape(const std::string &s, char delim, bool neutralise_formulas);

} // namespace dbp
