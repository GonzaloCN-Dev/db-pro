// Change Tracking Engine: pending edits/inserts/deletes over a loaded page, with undo/redo.
// Independent of the UI. Rows are addressed by index only while loaded; the identity sent
// to the server always comes from the key column values of the ORIGINAL row.
#pragma once

#include <map>
#include <set>

#include "core.h"

namespace dbp {

class ChangeSet {
public:
    using Rows = std::vector<std::vector<Value>>;

    bool empty() const { return s_.edits.empty() && s_.inserts.empty() && s_.deletes.empty(); }
    // Distinct rows affected (edited rows that are not deleted + inserts + deletes).
    size_t affected_rows() const;
    size_t edit_cells() const;
    size_t insert_count() const { return s_.inserts.size(); }
    size_t delete_count() const { return s_.deletes.size(); }

    // Total visible rows = loaded + new.
    size_t row_count(const Rows &loaded) const { return loaded.size() + s_.inserts.size(); }
    bool is_new(const Rows &loaded, size_t r) const { return r >= loaded.size(); }
    bool is_deleted(size_t r) const { return s_.deletes.count(r) > 0; }
    bool is_edited(size_t r, size_t c) const;
    bool row_has_edits(size_t r) const { return s_.edits.count(r) > 0; }

    const Value &current(const Rows &loaded, size_t r, size_t c) const;
    const Value &original(const Rows &loaded, size_t r, size_t c) const { return r < loaded.size() ? loaded[r][c] : s_.inserts[r - loaded.size()][c]; }

    // Setting a loaded cell back to its original value drops the edit.
    void set_cell(const Rows &loaded, size_t r, size_t c, Value v);
    void restore_cell(const Rows &loaded, size_t r, size_t c);
    void restore_row(const Rows &loaded, size_t r);
    void mark_delete(const Rows &loaded, size_t r);
    void unmark_delete(size_t r);
    void add_insert(std::vector<Value> row);
    // Copy of a visible row prepared for insertion: identity/generated columns become DEFAULT.
    std::vector<Value> duplicate_of(const TableMeta &m, const Rows &loaded, size_t r) const;
    static std::vector<Value> blank_row(const TableMeta &m);

    void clear();
    bool undo();
    bool redo();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }

    // Throws DbError(Validation) when a row cannot be identified or a column is not writable.
    std::vector<Change> build(const TableMeta &m, const Rows &loaded) const;

private:
    struct State {
        std::map<size_t, std::map<size_t, Value>> edits;
        std::vector<std::vector<Value>> inserts;
        std::set<size_t> deletes;
    };
    void push_undo();
    State s_;
    std::vector<State> undo_, redo_;
};

} // namespace dbp
