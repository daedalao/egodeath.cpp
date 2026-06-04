#pragma once

#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <filesystem>
#include <nlohmann/json.hpp>

struct sqlite3;

namespace egodeath {
using json = nlohmann::json;

// A single calendar/task record. Tasks generally carry a `due` date; events
// carry `start_ts`/`end_ts`. Both live in one table so the agenda view and the
// agent tools share one model.
struct Item {
    long long id = 0;
    std::string kind = "task";     // "task" | "event"
    std::string title;
    std::string notes;
    std::string status = "open";   // "open" | "done" | "cancelled"
    std::string priority;          // "" | "low" | "med" | "high"
    std::string due;               // ISO date/datetime (tasks)
    std::string start_ts;          // ISO datetime (events)
    std::string end_ts;
    std::string created_ts;
    std::string updated_ts;
    std::string project;  // "" = global, else a directory key (per-directory list)

    // The chronological anchor used for sorting/grouping (event start, else due).
    std::string when() const { return kind == "event" && !start_ts.empty() ? start_ts : due; }
    json to_json() const;
    std::string one_line() const; // compact human-readable form
};

// An observed shell command and how often it has been run, used by the
// learn-from-repetition feature to suggest crystallizing it into a tool/memory.
struct CmdStat {
    std::string raw, norm, source, project, last_ts;
    int count = 0;
    bool dismissed = false;
};

class Store {
public:
    explicit Store(std::filesystem::path db_path);
    ~Store();
    bool ok() const { return db_ != nullptr; }
    const std::string& error() const { return last_error_; }

    // Mutations. Return the new row id (add) or success (others); on failure set err.
    long long add(const Item& it, std::string& err);
    bool set_status(long long id, const std::string& status, std::string& err);
    bool update(long long id, const json& fields, std::string& err);
    bool remove(long long id, std::string& err);

    // Queries. Empty filter string means "any". due_from/due_to bound the `when`
    // anchor (inclusive); empty means unbounded. limit <= 0 means no limit.
    std::vector<Item> list(const std::string& kind, const std::string& status,
                           const std::string& from, const std::string& to, int limit,
                           const std::vector<std::string>& projects = {});
    std::optional<Item> get(long long id);

    // Observed-command log (learn-from-repetition).
    void record_command(const std::string& raw, const std::string& source, const std::string& project);
    std::vector<CmdStat> top_commands(int min_count, bool include_dismissed,
                                      const std::vector<std::string>& projects, int limit);
    bool dismiss_command(const std::string& cmd);

private:
    sqlite3* db_ = nullptr;
    std::mutex mtx_;
    std::string last_error_;
    void exec(const char* sql);
};

} // namespace egodeath
