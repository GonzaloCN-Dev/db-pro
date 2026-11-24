#include "changes.h"

namespace dbp {

namespace {
constexpr size_t kUndoLimit = 200;
}

size_t ChangeSet::affected_rows() const
{
    size_t n = s_.inserts.size() + s_.deletes.size();
    for (auto &e : s_.edits)
        if (!s_.deletes.count(e.first)) ++n;
    return n;
}

size_t ChangeSet::edit_cells() const
{
    size_t n = 0;
    for (auto &e : s_.edits) n += e.second.size();
    return n;
}

bool ChangeSet::is_edited(size_t r, size_t c) const
{
    auto it = s_.edits.find(r);
    return it != s_.edits.end() && it->second.count(c);
}

const Value &ChangeSet::current(const Rows &loaded, size_t r, size_t c) const
{
    if (r >= loaded.size()) return s_.inserts[r - loaded.size()][c];
    auto it = s_.edits.find(r);
    if (it != s_.edits.end()) {
        auto cit = it->second.find(c);
        if (cit != it->second.end()) return cit->second;
    }
    return loaded[r][c];
}

void ChangeSet::push_undo()
{
    undo_.push_back(s_);
    if (undo_.size() > kUndoLimit) undo_.erase(undo_.begin());
    redo_.clear();
}

void ChangeSet::set_cell(const Rows &loaded, size_t r, size_t c, Value v)
{
    if (r >= row_count(loaded)) return;
    if (current(loaded, r, c) == v) return;
    push_undo();
    if (r >= loaded.size()) {
        s_.inserts[r - loaded.size()][c] = std::move(v);
        return;
    }
    if (loaded[r][c] == v) {
        auto it = s_.edits.find(r);
        if (it != s_.edits.end()) {
            it->second.erase(c);
            if (it->second.empty()) s_.edits.erase(it);
        }
        return;
    }
    s_.edits[r][c] = std::move(v);
}

void ChangeSet::restore_cell(const Rows &loaded, size_t r, size_t c)
{
    if (r >= loaded.size() || !is_edited(r, c)) return;
    push_undo();
    auto it = s_.edits.find(r);
    it->second.erase(c);
    if (it->second.empty()) s_.edits.erase(it);
}

void ChangeSet::restore_row(const Rows &loaded, size_t r)
{
    if (r >= loaded.size()) return;
    if (!s_.edits.count(r) && !s_.deletes.count(r)) return;
    push_undo();
    s_.edits.erase(r);
    s_.deletes.erase(r);
}

void ChangeSet::mark_delete(const Rows &loaded, size_t r)
{
    if (r >= row_count(loaded)) return;
    push_undo();
    if (r >= loaded.size()) {
        s_.inserts.erase(s_.inserts.begin() + (r - loaded.size()));
        return;
    }
    s_.deletes.insert(r);
}

void ChangeSet::unmark_delete(size_t r)
{
    if (!s_.deletes.count(r)) return;
    push_undo();
    s_.deletes.erase(r);
}

void ChangeSet::add_insert(std::vector<Value> row)
{
    push_undo();
    s_.inserts.push_back(std::move(row));
}

std::vector<Value> ChangeSet::blank_row(const TableMeta &m)
{
    std::vector<Value> row;
    for (auto &c : m.columns) row.push_back(c.has_default || c.identity || c.generated ? Value::dflt() : Value::null());
    return row;
}

std::vector<Value> ChangeSet::duplicate_of(const TableMeta &m, const Rows &loaded, size_t r) const
{
    std::vector<Value> row;
    for (size_t c = 0; c < m.columns.size(); ++c) {
        const ColumnInfo &ci = m.columns[c];
        row.push_back(ci.identity || ci.generated ? Value::dflt() : current(loaded, r, c));
    }
    return row;
}

void ChangeSet::clear()
{
    s_ = State();
    undo_.clear();
    redo_.clear();
}

bool ChangeSet::undo()
{
    if (undo_.empty()) return false;
    redo_.push_back(s_);
    s_ = undo_.back();
    undo_.pop_back();
    return true;
}

bool ChangeSet::redo()
{
    if (redo_.empty()) return false;
    undo_.push_back(s_);
    s_ = redo_.back();
    redo_.pop_back();
    return true;
}

std::vector<Change> ChangeSet::build(const TableMeta &m, const Rows &loaded) const
{
    if (empty()) return {};
    if (!m.edit.editable) throw DbError(ErrKind::Validation, m.edit.blocked_reason.empty() ? "This table is not editable" : m.edit.blocked_reason);

    auto key_of = [&](size_t r) {
        std::vector<std::pair<std::string, Value>> key;
        for (auto &kc : m.edit.key_columns) {
            int ci = m.column_index(kc);
            if (ci < 0) throw DbError(ErrKind::Validation, "key column not found: " + kc);
            const Value &v = loaded[r][ci];
            if (v.is_null() || v.kind == Value::Kind::Missing) throw DbError(ErrKind::Validation, "row cannot be identified: key column " + kc + " is NULL");
            key.push_back({ kc, v });
        }
        if (key.empty()) throw DbError(ErrKind::Validation, "row cannot be identified: no key");
        return key;
    };

    std::vector<Change> out;
    for (auto &e : s_.edits) {
        if (s_.deletes.count(e.first)) continue;
        Change ch;
        ch.type = Change::Type::Update;
        ch.key = key_of(e.first);
        for (auto &ce : e.second) {
            const ColumnInfo &ci = m.columns[ce.first];
            if (ci.read_only()) throw DbError(ErrKind::Validation, "column " + ci.name + " cannot be edited");
            ch.set.push_back({ ci.name, ce.second, loaded[e.first][ce.first] });
        }
        out.push_back(std::move(ch));
    }
    for (size_t r : s_.deletes) {
        Change ch;
        ch.type = Change::Type::Delete;
        ch.key = key_of(r);
        out.push_back(std::move(ch));
    }
    for (auto &row : s_.inserts) {
        Change ch;
        ch.type = Change::Type::Insert;
        for (size_t c = 0; c < m.columns.size(); ++c) {
            if (m.columns[c].generated) continue;
            ch.values.push_back({ m.columns[c].name, row[c] });
        }
        out.push_back(std::move(ch));
    }
    return out;
}

} // namespace dbp
