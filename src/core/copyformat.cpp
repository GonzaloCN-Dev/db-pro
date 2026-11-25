#include "copyformat.h"

#include <nlohmann/json.hpp>

#include "exporter.h"

namespace dbp {

using nlohmann::json;

static json cell_json(const Value &v)
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
    default: return v.s;
    }
}

CopyResult format_rows(const std::vector<ColumnInfo> &cols, const std::vector<std::vector<Value>> &rows, CopyFormat format,
                       const std::string &table, Dialect dialect, bool header, size_t max_bytes)
{
    CopyResult out;
    const char delim = format == CopyFormat::Csv ? ',' : '\t';
    const char *eol = format == CopyFormat::Csv ? "\r\n" : "\n";
    std::string head;
    if (format == CopyFormat::Tsv || format == CopyFormat::Csv) {
        if (header) {
            for (size_t k = 0; k < cols.size(); ++k) head += (k ? std::string(1, delim) : "") + csv_escape(cols[k].name, delim, false);
            head += eol;
        }
    } else if (format == CopyFormat::SqlInsert) {
        head = "INSERT INTO " + quote_ident(dialect, table.empty() ? "table" : table) + " (";
        for (size_t k = 0; k < cols.size(); ++k) head += (k ? ", " : "") + quote_ident(dialect, cols[k].name);
        head += ") VALUES (";
    }

    std::string text = format == CopyFormat::Json ? "[" : (format == CopyFormat::SqlInsert ? "" : head);
    bool first = true;
    for (const auto &row : rows) {
        std::string line;
        if (format == CopyFormat::Json) {
            json o = json::object();
            for (size_t k = 0; k < row.size() && k < cols.size(); ++k) o[cols[k].name] = cell_json(row[k]);
            line = std::string(first ? "\n  " : ",\n  ") + o.dump(-1, ' ', false, json::error_handler_t::replace);
        } else if (format == CopyFormat::SqlInsert) {
            line = head;
            for (size_t k = 0; k < row.size(); ++k) line += (k ? ", " : "") + render_literal(row[k]);
            line += ");\n";
        } else {
            for (size_t k = 0; k < row.size(); ++k) {
                if (k) line += delim;
                const Value &v = row[k];
                if (v.is_null() || v.kind == Value::Kind::Missing) continue;
                if (v.kind == Value::Kind::Text) line += csv_escape(v.s, delim, format == CopyFormat::Csv);
                else line += csv_escape(v.display(), delim, false);
            }
            line += eol;
        }
        if (text.size() + line.size() > max_bytes) {
            out.truncated = true;
            break;
        }
        text += line;
        first = false;
        ++out.rows;
    }
    if (format == CopyFormat::Json) text += first ? "]" : "\n]";
    out.text = std::move(text);
    return out;
}

} // namespace dbp
