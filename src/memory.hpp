#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Memory {
public:
    struct RootScope {
        std::filesystem::path root;
        std::string scope;
    };

    Memory(std::filesystem::path global_root, std::filesystem::path project_root, std::string project_name);

    void save(const std::string& name, const std::string& description, const std::string& fact, const std::string& type = "unknown", const std::string& scope = "global");
    std::string recall(const std::string& query, const std::string& scope = "all");
    std::vector<json> list_entries();
    bool remove(const std::string& name);

    std::string system_prompt_addendum();

private:
    std::filesystem::path global_root;
    std::optional<std::filesystem::path> project_root;
    std::string project_name;

    std::vector<RootScope> roots_with_labels(bool project_first = true);
};
