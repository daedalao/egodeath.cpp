#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// A persisted registry of user/agent-defined tools, so common operations
// crystallize into named, reusable tools instead of being hand-written shell
// every turn. Two kinds:
//   template — a shell command string with {param} placeholders
//   recipe   — an ordered list of steps, each invoking an existing tool
//              (read_file, edit_file, exec_shell, …) or another custom tool
// The set persists to JSON and grows across sessions: that is the "learning".
class CustomTools {
public:
    struct Param { std::string name, description; bool required = false; };
    struct Def {
        std::string name, description, kind;  // kind: "template" | "recipe"
        std::string command;                  // template body
        json steps = json::array();           // recipe steps: [{tool, args}, …]
        std::vector<Param> params;
        std::string scope = "global";         // "global" or a project key
        long long uses = 0;
    };

    // Built-in tool names a custom tool may never shadow.
    static bool is_reserved(const std::string& name);

    void load(std::filesystem::path file, std::string project_key);

    bool has(const std::string& name) const;          // in-scope by name
    bool is_template(const std::string& name) const;
    std::optional<Def> get(const std::string& name) const;

    // OpenAI-style function schemas for every in-scope tool.
    std::vector<json> schemas() const;

    // Create or replace a tool. Returns a status line; sets err on rejection.
    std::string define(const Def& d, std::string& err);
    bool remove(const std::string& name);
    void bump_use(const std::string& name);

    json list_json() const;  // for the list_tools tool

private:
    std::filesystem::path file_;
    std::string project_key_ = "global";
    std::vector<Def> defs_;
    mutable std::mutex mtx_;

    bool in_scope(const Def& d) const;
    void save_locked() const;
    Def* find_locked(const std::string& name);
    const Def* find_locked(const std::string& name) const;
    static json def_to_json(const Def& d);
    static Def def_from_json(const json& j);
};
