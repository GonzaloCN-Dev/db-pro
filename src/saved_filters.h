// Named column-filter sets, saved per connection and table. Only the filter boxes' state is stored (column
// name, operator label and typed text), never SQL: applying one rebuilds typed conditions and validates them
// against the table's current columns again.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace dbp {

constexpr size_t kMaxSavedFilters = 30;
constexpr size_t kMaxSavedFilterName = 60;

struct SavedFilterItem {
    std::string column;
    std::string op; // operator label as shown to the user (Op label), resolved against the column on use
    std::string text;
};

struct SavedFilter {
    std::string name;
    std::vector<SavedFilterItem> items;
};

class SavedFilters {
public:
    explicit SavedFilters(std::string dir = {});
    void load();
    bool save() const;
    static std::string make_key(const std::string &profile_id, const std::string &database, const std::string &schema, const std::string &table);
    const std::vector<SavedFilter> &list(const std::string &key) const;
    // Replaces a filter with the same name. Returns false when the name is empty or the limit is reached.
    bool put(const std::string &key, SavedFilter f);
    void remove(const std::string &key, size_t index);

private:
    std::string dir_;
    std::map<std::string, std::vector<SavedFilter>> items_;
};

} // namespace dbp
