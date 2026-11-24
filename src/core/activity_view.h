#pragma once

#include "core.h"

#include <string>
#include <vector>

namespace dbp {

struct ActivityViewOptions {
    std::string search;
    bool blocked_only = false;
    int sort_column = -1;
    bool ascending = true;
};

// Returns original Activity::rows indices in the presentation order.
std::vector<size_t> activity_visible_rows(const Activity &activity, const ActivityViewOptions &options);

} // namespace dbp
