#include "exporter.h"

#include <nlohmann/json.hpp>

#include "sqlgen.h"

namespace dbp {

using nlohmann::json;

const char *export_extension(ExportFormat f)
{
    switch (f) {
    case ExportFormat::Csv: return ".csv";
    case ExportFormat::Tsv: return ".tsv";
    case ExportFormat::Json: return ".json";
    case ExportFormat::Ndjson: return ".ndjson";
    case ExportFormat::SqlInsert: return ".sql";
    }
    return "";
}

std::string csv_escape(const std::string &s, char delim, bool neutralise)
{
    std::string v = s;
    // A text cell starting with = + - @ is executed as a formula by spreadsheets.
    if (neutralise && !v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '@' || (v[0] == '-' && v.size() > 1 && !std::isdigit((unsigned char)v[1]) && v[1] != '.')))
        v = "'" + v;
    bool quote = v.find(delim) != std::string::npos || v.find('"') != std::string::npos || v.find('\n') != std::string::npos || v.find('\r') != std::string::npos;
    if (!quote) return v;
    std::string r = "\"";
    for (char c : v) {
        if (c == '"') r += '"';
        r += c;
    }
    return r + "\"";
}

namespace {

class FileSink {
public:
    explicit FileSink(const std::filesystem::path &p) : f_(p, std::ios::binary | std::ios::trunc)
    {
        if (!f_) throw DbError(ErrKind::Other, "Cannot create file: " + p.string());
        buf_.reserve(1 << 16);
    }
    void put(const std::string &s)
    {
        buf_ += s;
        if (buf_.size() >= (1 << 16)) flush();
    }
    void flush()
    {
        f_.write(buf_.data(), (std::streamsize)buf_.size());
        buf_.clear();
        if (!f_.good()) throw DbError(ErrKind::Other, "Write failed (disk full?)");
    }
    void close()
    {
        flush();
        f_.close();
    }

private:
    std::ofstream f_;
    std::string buf_;
};

json value_to_json(const Value &v)
{
    switch (v.kind) {
    case Value::Kind::Null:
    case Value::Kind::Missing:
    case Value::Kind::Default: return nullptr;
    case Value::Kind::Bool: return v.i != 0;
    case Value::Kind::Int: return v.i;
    case Value::Kind::Float: return v.d;
    case Value::Kind::Json: {
        auto j = json::parse(v.s, nullptr, false);
        return j.is_discarded() ? json(v.s) : j;
    }
    case Value::Kind::Bytes: return hex_preview(v.s, v.s.size());
    default: return v.s; // Decimal stays exact text
    }
}

class CsvWriter : public RowWriter {
public:
    CsvWriter(const std::filesystem::path &p, const ExportOptions &o) : sink_(p), o_(o)
    {
        delim_ = o.format == ExportFormat::Tsv ? '\t' : o.delimiter;
        if (o.bom) sink_.put("\xEF\xBB\xBF");
    }
    void begin(const std::vector<ColumnInfo> &cols) override
    {
        if (!o_.header) return;
        std::string line;
        for (size_t k = 0; k < cols.size(); ++k) line += (k ? std::string(1, delim_) : "") + csv_escape(cols[k].name, delim_, false);
        sink_.put(line + "\r\n");
    }
    void rows(const std::vector<std::vector<Value>> &batch) override
    {
        for (auto &row : batch) {
            std::string line;
            for (size_t k = 0; k < row.size(); ++k) {
                if (k) line += delim_;
                const Value &v = row[k];
                if (v.is_null() || v.kind == Value::Kind::Missing) line += o_.null_text;
                else if (v.kind == Value::Kind::Text) line += csv_escape(v.s, delim_, true);
                else line += csv_escape(v.display(), delim_, false);
            }
            sink_.put(line + "\r\n");
        }
    }
    void end() override { sink_.close(); }

private:
    FileSink sink_;
    ExportOptions o_;
    char delim_;
};

class JsonWriter : public RowWriter {
public:
    JsonWriter(const std::filesystem::path &p, bool lines) : sink_(p), lines_(lines) {}
    void begin(const std::vector<ColumnInfo> &cols) override
    {
        cols_ = cols;
        if (!lines_) sink_.put("[");
    }
    void rows(const std::vector<std::vector<Value>> &batch) override
    {
        for (auto &row : batch) {
            json o = json::object();
            for (size_t k = 0; k < row.size() && k < cols_.size(); ++k) o[cols_[k].name] = value_to_json(row[k]);
            std::string s = o.dump(-1, ' ', false, json::error_handler_t::replace);
            if (lines_) sink_.put(s + "\n");
            else { sink_.put(std::string(first_ ? "\n  " : ",\n  ") + s); first_ = false; }
        }
    }
    void end() override
    {
        if (!lines_) sink_.put(first_ ? "]\n" : "\n]\n");
        sink_.close();
    }

private:
    FileSink sink_;
    bool lines_;
    bool first_ = true;
    std::vector<ColumnInfo> cols_;
};

class SqlWriter : public RowWriter {
public:
    SqlWriter(const std::filesystem::path &p, const std::string &table) : sink_(p), table_(table.empty() ? "table" : table) {}
    void begin(const std::vector<ColumnInfo> &cols) override
    {
        head_ = "INSERT INTO " + quote_ident(Dialect::Postgres, table_) + " (";
        for (size_t k = 0; k < cols.size(); ++k) head_ += (k ? ", " : "") + quote_ident(Dialect::Postgres, cols[k].name);
        head_ += ") VALUES (";
    }
    void rows(const std::vector<std::vector<Value>> &batch) override
    {
        for (auto &row : batch) {
            std::string s = head_;
            for (size_t k = 0; k < row.size(); ++k) s += (k ? ", " : "") + render_literal(row[k]);
            sink_.put(s + ");\n");
        }
    }
    void end() override { sink_.close(); }

private:
    FileSink sink_;
    std::string table_, head_;
};

} // namespace

std::unique_ptr<RowWriter> make_writer(const std::filesystem::path &path, const ExportOptions &o)
{
    switch (o.format) {
    case ExportFormat::Csv:
    case ExportFormat::Tsv: return std::make_unique<CsvWriter>(path, o);
    case ExportFormat::Json: return std::make_unique<JsonWriter>(path, false);
    case ExportFormat::Ndjson: return std::make_unique<JsonWriter>(path, true);
    case ExportFormat::SqlInsert: return std::make_unique<SqlWriter>(path, o.table_name);
    }
    throw DbError(ErrKind::Unsupported, "unknown export format");
}

} // namespace dbp
