#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <nlohmann/json.hpp>
#include <mutex>
#include <atomic>

using json = nlohmann::json;

class Memory {
public:
    struct RootScope {
        std::filesystem::path root;
        std::string scope;
    };

    // One parsed memory file: frontmatter metadata + body.
    struct Entry {
        std::string name, scope, category, description, body, path;
        bool pinned = false;
        long long created = 0, last_used = 0, uses = 0;
    };

    // Per-scope cap on the number of non-pinned memories. Beyond this, the
    // least-recently-used / least-used entries are evicted on save. Pinned
    // memories never count against the cap and are never evicted.
    static constexpr int kScopeCap = 50;
    // Soft ceiling on how many characters the prompt digest may consume.
    static constexpr size_t kDigestChars = 3000;

    Memory(std::filesystem::path global_root, std::filesystem::path project_root, std::string project_name);

    void save(const std::string& name, const std::string& description, const std::string& fact,
              const std::string& category = "unknown", const std::string& scope = "global",
              bool pinned = false);
    std::string recall(const std::string& query, const std::string& scope = "all", size_t max_chars = 200000);
    std::vector<json> list_entries();
    bool remove(const std::string& name);

    // UI/agent curation.
    bool set_category(const std::string& name, const std::string& category);
    bool set_pinned(const std::string& name, bool pinned);

    // Compact, categorized, bounded index injected into the system prompt. Lists
    // one line per memory (name + description) rather than full bodies, so the
    // model can see what it knows and pull details on demand via recall_memory.
    std::string system_prompt_addendum();

private:
    std::filesystem::path global_root;
    std::optional<std::filesystem::path> project_root;
    std::string project_name;

    mutable std::mutex file_mutex_;  // Protects concurrent access to file system operations

    std::vector<RootScope> roots_with_labels(bool project_first = true);

    void atomic_write_file(const std::filesystem::path& path, const std::string& content);

    // Parse a single .md file into an Entry (frontmatter + body). Returns false if
    // the file can't be read.
    bool parse_file(const std::filesystem::path& path, const std::string& scope, Entry& out);
    // Serialize and atomically write an Entry back to disk.
    void write_entry(const std::filesystem::path& path, const Entry& e);
    // Collect every memory across all scopes.
    std::vector<Entry> collect_all();
    // Find a memory file by name across scopes.
    std::optional<std::filesystem::path> locate(const std::string& name, std::string* scope = nullptr);
    // Bump uses + last_used for an entry on disk (called when explicitly recalled).
    void bump_use(const std::filesystem::path& path);
    // Evict least-valuable non-pinned memories in a scope once over kScopeCap.
    void enforce_limit(const std::filesystem::path& root);
};
