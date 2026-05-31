// Command palette implementation for egodeath TUI
// Implements Ctrl+P → palette, ESC to cancel, Up/Down to navigate, Enter to execute

#include "tui.hpp"
#include <algorithm>
#include <vector>

namespace egodeath {

// Define available commands
struct Command {
    std::string name;
    std::string description;
    bool (*handler)(ProTUI* tui);
};

static std::vector<Command> available_commands = {
    {"/save", "Save current chat to file", nullptr},  // placeholder
    {"/load", "Load chat history from file", nullptr},
    {"/reset", "Reset conversation and clear history", nullptr},
    {"/clear", "Clear screen view (reset scroll)", nullptr},
    {"/toggle-reasoning", "Toggle reasoning display", nullptr},
    {"/exit", "Exit the application", nullptr},
};

// Helper to fuzzy match command name
static std::vector<int> _fuzzy_match_command(const std::string& pattern, const std::vector<Command>& cmds) {
    std::vector<int> matches;
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);
    
    for (size_t i = 0; i < cmds.size(); ++i) {
        std::string name_lower = cmds[i].name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        
        size_t pos = 0;
        bool matched = true;
        for (char c : pattern_lower) {
            pos = name_lower.find(c, pos);
            if (pos == std::string::npos) { matched = false; break; }
            ++pos;
        }
        if (matched) matches.push_back((int)i);
    }
    return matches;
}

// Input mode enum update: add COMMAND_PALETTE
enum class InputMode {
    NORMAL,              // standard input mode (default)
    SEARCH,              // fuzzy search mode (Ctrl+F)
    COMMAND_PALETTE,     // command palette mode (Ctrl+P)
};

// Extended ProTUI fields (add to header)
// input_mode_ already exists
// new fields needed:
//   std::vector<int> command_match_indices_;
//   int command_selected_idx_ = 0;
//   std::string command_palette_query_;

} // namespace egodeath
