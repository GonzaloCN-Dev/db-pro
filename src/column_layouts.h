// Per-table column layout (hidden columns and display order), persisted next to the other app data.
// Columns are stored by NAME, never by index, so a schema change cannot hide or move the wrong column.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace dbp {

struct ColumnLayout {
    std::vector<std::string> order;  // preferred display order (may miss or list unknown names)
    std::vector<std::string> hidden;
};

struct ColumnView {
    std::vector<size_t> order;  // display order: indexes into the real column list, every column exactly once
    std::vector<bool> hidden;   // indexed by real column index; never all true
};

// Resolves a stored layout against the table's current columns. Unknown names are ignored, new columns
// go to the end, and at least one column always stays visible.
ColumnView resolve_layout(const ColumnLayout &layout, const std::vector<std::string> &columns);

// Builds the layout to store from a resolved view.
ColumnLayout layout_from_view(const ColumnView &view, const std::vector<std::string> &columns);

class ColumnLayouts {
public:
    explicit ColumnLayouts(std::string dir = {});
    void load();
    bool save() const;
    // key = connection id + table identity; use make_key.
    ColumnLayout get(const std::string &key) const;
    void set(const std::string &key, const ColumnLayout &layout); // an empty layout removes the entry
    static std::string make_key(const std::string &profile_id, const std::string &database, const std::string &schema, const std::string &table);

private:
    std::string dir_;
    std::map<std::string, ColumnLayout> items_;
};

} // namespace dbp
