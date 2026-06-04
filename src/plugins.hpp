#pragma once

#include <string>
#include <vector>
#include <filesystem>

// Interface plugins: user-installed extensions to the TUI itself (not new model
// tools — MCP and custom_tools already cover that). A plugin is a directory under
// ~/.config/egodeath/plugins/<name>/ with a plugin.json manifest and an
// executable. The host runs the executable (in the plugin dir, stdin /dev/null,
// EGODEATH_PLUGIN_* in the environment) and renders its stdout. Two surfaces:
//   panel   — a scrollable pane bound to an F-key, refreshed on an interval
//   command — a slash command whose output is printed to the chat
class Plugins {
public:
    struct Plugin {
        std::string name;
        std::filesystem::path dir;
        std::string exec;            // shell command, run with cwd = dir
        // panel
        bool has_panel = false;
        int panel_key = 0;           // F-key number (6..12); 0 = none
        std::string panel_title;
        int panel_refresh = 0;       // seconds between auto-refreshes; 0 = manual only
        // command
        bool has_command = false;
        std::string slash;           // e.g. "/git"
    };

    void load(const std::filesystem::path& root);
    const std::vector<Plugin>& all() const { return plugins_; }
    const Plugin* by_slash(const std::string& slash) const;
    const Plugin* by_name(const std::string& name) const;

    // Run a plugin in mode "panel" or "command" with args; returns captured stdout.
    std::string run(const Plugin& p, const std::string& mode, const std::string& args) const;
    std::string run_by_name(const std::string& name, const std::string& mode, const std::string& args) const;

    std::string list_text() const;

private:
    std::vector<Plugin> plugins_;
    static int parse_fkey(const std::string& s);
};
