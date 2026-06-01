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

    Memory(std::filesystem::path global_root, std::filesystem::path project_root, std::string project_name);

    void save(const std::string& name, const std::string& description, const std::string& fact, const std::string& type = "unknown", const std::string& scope = "global");
    std::string recall(const std::string& query, const std::string& scope = "all", size_t max_chars = 200000);
    std::vector<json> list_entries();
    bool remove(const std::string& name);

    std::string system_prompt_addendum();

private:
    std::filesystem::path global_root;
    std::optional<std::filesystem::path> project_root;
    std::string project_name;

    mutable std::mutex file_mutex_;  // Protects concurrent access to file system operations

    std::vector<RootScope> roots_with_labels(bool project_first = true);
    
    // Atomic write helper
    void atomic_write_file(const std::filesystem::path& path, const std::string& content);
    
    // Helper to extract content from frontmatter-separated markdown file
    std::string read_file_content(const std::filesystem::path& path);
    
    // Filter recall results by relevance (basic keyword matching on name/description)
    bool matches_query(const json& metadata, const std::string& query);
};
