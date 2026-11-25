// Local SQL history and favourites, persisted as JSON next to the other app data.
// Only the statement text is kept: never results, connection details or passwords.
#pragma once

#include <string>
#include <vector>

namespace dbp {

struct HistoryEntry {
    std::string text;
    bool favorite = false;
    long long last_used = 0; // unix seconds
    int runs = 1;
};

class QueryHistory {
public:
    static constexpr size_t kMaxPlain = 200;     // non-favourite entries kept
    static constexpr size_t kMaxFavorites = 200;
    static constexpr size_t kMaxChars = 20000;   // longer statements are not stored

    // dir empty = the real per-user data directory (tests pass a temp dir).
    explicit QueryHistory(std::string dir = {});

    void load();
    bool save() const;

    // Records a successfully executed statement. Returns false when it was skipped (blank, too long or
    // looks like it carries a secret). A repeat moves to the top and bumps `runs`.
    bool record(const std::string &text, long long now);

    bool set_favorite(size_t index, bool on);
    bool remove(size_t index);
    void clear_plain(); // removes everything except favourites
    void clear_all();

    // Newest first. `needle` is a case-insensitive substring filter. Returns indexes into entries().
    std::vector<size_t> view(const std::string &needle, bool favorites_only) const;

    const std::vector<HistoryEntry> &entries() const { return entries_; }

    static bool looks_secret(const std::string &text);
    static std::string preview(const std::string &text, size_t max_chars = 120); // single line, collapsed whitespace

private:
    void trim();
    std::string dir_;
    std::vector<HistoryEntry> entries_; // kept sorted newest first
};

} // namespace dbp
