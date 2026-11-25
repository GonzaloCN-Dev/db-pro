#include "paste.h"

#include "filtertext.h"

namespace dbp {

bool parse_tsv(const std::string &text, std::vector<std::vector<std::string>> &out, std::string *err)
{
    out.clear();
    if (text.size() > kMaxPasteBytes) { if (err) *err = "paste.too-big"; return false; }
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false, was_quoted = false, any = false;
    auto end_cell = [&] { row.push_back(std::move(cell)); cell.clear(); was_quoted = false; };
    auto end_row = [&] {
        end_cell();
        out.push_back(std::move(row));
        row.clear();
        any = false;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else quoted = false;
            } else cell += c;
            continue;
        }
        if (c == '"' && cell.empty() && !was_quoted) { quoted = true; was_quoted = true; any = true; continue; }
        if (c == '\t') { end_cell(); any = true; continue; }
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            end_row();
            if (out.size() > kMaxPasteRows) { if (err) *err = "paste.too-many"; return false; }
            continue;
        }
        cell += c;
        any = true;
    }
    if (quoted) { if (err) *err = "paste.bad-quote"; return false; }
    if (any || !cell.empty() || !row.empty()) end_row();
    if (out.size() > kMaxPasteRows) { if (err) *err = "paste.too-many"; return false; }
    return true;
}

PastePlan plan_paste(const std::vector<ColumnInfo> &cols, const std::vector<size_t> &targets, const std::vector<std::vector<std::string>> &cells)
{
    PastePlan plan;
    for (size_t r = 0; r < cells.size(); ++r) {
        std::vector<Value> row;
        if (cells[r].size() > targets.size())
            plan.issues.push_back({ r, targets.size(), "paste.extra-cells" });
        for (size_t k = 0; k < targets.size() && k < cells[r].size(); ++k) {
            const ColumnInfo &col = cols[targets[k]];
            const std::string &text = cells[r][k];
            Value v;
            if (col.read_only()) {
                plan.issues.push_back({ r, k, "paste.readonly-col" });
            } else if (text.empty() && !is_textual_class(col.cls)) {
                if (!col.nullable && !col.has_default && !col.identity) plan.issues.push_back({ r, k, "paste.empty-not-null" });
                v = col.nullable ? Value::null() : Value::dflt();
            } else if (is_textual_class(col.cls)) {
                v = Value::text(text);
            } else {
                std::string err;
                auto parsed = parse_typed(col.cls, text, &err);
                if (!parsed) plan.issues.push_back({ r, k, err.empty() ? "paste.bad-value" : err });
                else v = *parsed;
            }
            row.push_back(std::move(v));
        }
        plan.values.push_back(std::move(row));
    }
    return plan;
}

} // namespace dbp
