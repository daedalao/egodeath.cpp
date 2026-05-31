// Enhanced status support for TUI
// Add this near top of tui.hpp (after struct Line definition, before private:)

enum class StatusType {
    IDLE,       // default idle state
    STREAMING,  // model is streaming tokens
    TOOL,       // tool execution in progress
    ERROR,      // error state
    PROCESSING, // general processing (e.g., loading, parsing)
};

// Extend ProTUI class with new status API (add to public methods in tui.hpp)
// In tui.hpp, after existing methods:
//   void set_status(const std::string& status);
// Add:
//   void set_status(StatusType type, const std::string& detail = "");

// And in private members:
//   StatusType status_type_ = StatusType::IDLE;
//   std::string status_detail_ = "";
