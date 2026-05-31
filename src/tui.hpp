#pragma once
#include "common.hpp"
#include "sys_monitor.hpp"
#include <deque>

namespace egodeath {

enum class StatusType {
    IDLE,       // default idle state
    STREAMING,  // model is streaming tokens
    TOOL,       // tool execution in progress
    ERROR,      // error state
    PROCESSING, // general processing (e.g., loading, parsing)
};

enum class InputMode {
    NORMAL,              // standard input mode (default)
    SEARCH,              // fuzzy search mode (Ctrl+F)
    COMMAND_PALETTE,     // command palette mode (Ctrl+P)
};

class ProTUI {
public:
    ProTUI();
    ~ProTUI();

    void init();
    void shutdown();
    void refresh_ui(bool force = false);
    
    std::string get_input();
    void append_history(const std::string& sender, const std::string& text, const std::string& type = "assistant");
    void append_last_history(const std::string& chunk);
    void update_reasoning(const std::string& text, bool append = true);
    void clear_reasoning();
    
    void set_metrics(const Metrics& m);
    void set_activity(const std::string& activity);
    void set_topic(const std::string& topic);
    
    // Progress and state methods
    void set_streaming(bool streaming);
    void set_status(const std::string& status);  // legacy, sets detail for current type
    void set_status(StatusType type, const std::string& detail = "");
    
    // Command history support
    void add_command_to_history(const std::string& cmd);
    std::string get_previous_command();
    std::string get_next_command();
    void reset_command_history();
    void toggle_reasoning();  // Added: public toggle for Alt+R

    bool is_running() const { return running_; }

private:
    void _setup_windows();
    void _draw_dashboard();
    void _draw_borders();
    void _draw_status();
    void _render_history();
    void _handle_mouse();
    void _toggle_mouse();
    std::vector<std::string> _wrap_text(const std::string& text, int width);
    std::string _get_bar(double percent, int width);
    std::string _get_sparkline(const std::vector<double>& values, int width);
    std::vector<int> _fuzzy_match(const std::string& pattern, const std::string& text) const;
    void _update_search_results();
    void _update_command_palette_results();
    void _render_command_palette();
    
    WINDOW *dash_win_ = nullptr;
    WINDOW *hist_win_ = nullptr;
    WINDOW *reas_win_ = nullptr;
    WINDOW *in_win_ = nullptr;
    WINDOW *status_win_ = nullptr;

    struct Line { 
        std::string text; 
        int color; 
        std::string type = "assistant";
    };
    std::deque<Line> raw_history_;
    std::vector<Line> wrapped_history_;
    
    std::string current_reasoning_;
    std::string topic_ = "egodeath";
    std::string activity_ = "idle";
    std::string status_ = "ready";
    Metrics metrics_;
    SystemStats sys_stats_;
    SysMonitor sys_monitor_;
    
    int scroll_offset_ = 0;
    bool running_ = false;
    bool show_reasoning_ = true;
    bool mouse_enabled_ = true;
    std::mutex mtx_;
    
    // Command history
    std::deque<std::string> command_history_;
    size_t command_history_pos_ = 0;
    
    // Streaming state
    bool streaming_ = false;
    
    // Enhanced status state
    StatusType status_type_ = StatusType::IDLE;
    std::string status_detail_ = "";
    
    // Search state
    InputMode input_mode_ = InputMode::NORMAL;
    std::string search_query_;
    std::vector<int> search_match_indices_;
    
    // Command palette state
    std::vector<int> command_match_indices_;
    int command_selected_idx_ = 0;
    std::string command_palette_query_;
    
    // Smart scrolling state
    bool manual_scroll_ = false;
    int input_lines_ = 1;
    
    // Recent throughput for sparkline
    std::vector<double> recent_throughput_;

    int _get_color_for_type(const std::string& type) const;
};

} // namespace egodeath
