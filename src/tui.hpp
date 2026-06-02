#pragma once
#include <functional>
#include "common.hpp"
#include "sys_monitor.hpp"
#include <deque>
#include <atomic>
#include <mutex>
#include <condition_variable>

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
    HELP,                // help overlay (F1)
    AGENDA,              // task/calendar view (F2)
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
    void set_queued(int n);
    void activate_last_queued();
    static void sigint_received(); // called from SIGINT handler
    void set_cancel_callback(std::function<void()> cb) { cancel_callback_ = std::move(cb); }
    void set_effort_callback(std::function<void(const std::string&)> cb) { effort_callback_ = std::move(cb); }
    void set_initial_effort(const std::string& e) { reasoning_effort_ = e; }
    int request_tool_approval(const std::string& prompt); // worker thread: 0 deny, 1 once, 2 always
    void set_theme(const std::string& name);
    std::string current_theme() const { return theme_; }
    void cycle_theme();
    bool has_theme(const std::string& name) const;
    void clear_history();
    void set_save_callback(std::function<void()> cb) { save_callback_ = std::move(cb); }
    void set_load_callback(std::function<void()> cb) { load_callback_ = std::move(cb); }
    void set_mcp_callback(std::function<std::string()> cb) { mcp_callback_ = std::move(cb); }
    void set_agenda_provider(std::function<json()> cb) { agenda_provider_ = std::move(cb); }
    void set_agenda_action(std::function<std::string(const std::string&, long long, const std::string&)> cb) { agenda_action_ = std::move(cb); }
    void open_agenda();
    bool agenda_open() const { return input_mode_ == InputMode::AGENDA; }

    bool is_running() const { return running_; }
    bool is_history_empty() const;  // Check if history is empty (thread-safe)

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
    void _render_help();
    void _render_agenda();
    void _agenda_refetch();
    std::string _agenda_add_prompt();
    void _apply_theme(const std::string& name);
    void _load_themes();
    
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
    bool mouse_enabled_ = false;
    mutable std::mutex mtx_;
    
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
    int last_ch_ = 0;
    int reas_scroll_offset_ = 0;
    int queued_depth_ = 0;
    std::string pending_paste_;
    std::function<void()> cancel_callback_; // large paste staged for submission
    std::function<void(const std::string&)> effort_callback_;
    std::function<void()> save_callback_;
    std::function<void()> load_callback_;
    std::function<std::string()> mcp_callback_;
    std::function<json()> agenda_provider_;
    std::function<std::string(const std::string&, long long, const std::string&)> agenda_action_;
    json agenda_items_ = json::array();
    std::vector<long long> agenda_order_ids_;
    int agenda_sel_ = 0;
    std::string reasoning_effort_ = "medium";
    std::string theme_ = "dark";
    std::map<std::string, std::map<int, std::pair<int,int>>> themes_;
    std::vector<std::string> theme_order_;
    std::atomic<bool> approval_pending_{false};
    std::mutex approval_mtx_;
    std::condition_variable approval_cv_;
    std::string approval_prompt_;
    int approval_result_ = -1;
    int input_lines_ = 1;
    
    // Recent throughput for sparkline
    std::vector<double> recent_throughput_;

    int _get_color_for_type(const std::string& type) const;
};

} // namespace egodeath
