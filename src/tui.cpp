#include "tui.hpp"
#include <locale.h>
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <vector>

namespace egodeath {

// Define available commands
struct Command {
    std::string name;
    std::string description;
};

static const std::vector<Command> available_commands = {
    {"/save", "Save current chat to file"},
    {"/load", "Load chat history from file"},
    {"/reset", "Reset conversation and clear history"},
    {"/clear", "Clear screen view (reset scroll)"},
    {"/toggle-reasoning", "Toggle reasoning display"},
    {"/exit", "Exit the application"},
};

// Helper to fuzzy match command name
static std::vector<int> _fuzzy_match_command(const std::string& pattern, const std::vector<Command>& cmds) {
    std::vector<int> matches;
    if (pattern.empty()) {
        for (size_t i = 0; i < cmds.size(); ++i) matches.push_back((int)i);
        return matches;
    }
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

ProTUI::ProTUI() {}
ProTUI::~ProTUI() { shutdown(); }

void ProTUI::init() {
    setlocale(LC_ALL, "");
    initscr(); start_color(); use_default_colors();
    cbreak(); noecho(); keypad(stdscr, TRUE);
    if (mouse_enabled_) {
        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
        printf("\033[?1003h");
    }
    nodelay(stdscr, FALSE);
    
    init_pair(1, COLOR_CYAN, -1);    
    init_pair(2, 46, -1);            
    init_pair(3, 226, -1);           
    init_pair(4, 255, -1);           
    init_pair(5, 0, 13);             
    init_pair(6, 160, -1);           
    init_pair(7, 196, -1);  // red for error
    init_pair(8, 214, -1);  // orange for search highlight
    init_pair(9, 27, -1);   // dark blue for command palette selection

    _setup_windows();
    running_ = true;
}

int ProTUI::_get_color_for_type(const std::string& type) const {
    if (type == "user") return 5;       // red
    if (type == "assistant") return 4;  // white
    if (type == "tool") return 2;       // green
    if (type == "system") return 1;     // cyan
    return 4;
}

void ProTUI::_setup_windows() {
    std::lock_guard<std::mutex> lock(mtx_);
    int y, x; getmaxyx(stdscr, y, x);
    int dash_h = 5, in_h = 3, reas_h = show_reasoning_ ? 5 : 0, status_h = 1;
    int hist_h = std::max(1, y - dash_h - in_h - reas_h - status_h);

    if (dash_win_) delwin(dash_win_);
    if (hist_win_) delwin(hist_win_);
    if (reas_win_) delwin(reas_win_);
    if (in_win_) delwin(in_win_);
    if (status_win_) delwin(status_win_);

    dash_win_ = newwin(dash_h, x, 0, 0);
    hist_win_ = newwin(hist_h, x, dash_h, 0);
    scrollok(hist_win_, TRUE);
    if (show_reasoning_) reas_win_ = newwin(reas_h, x, dash_h + hist_h, 0);
    status_win_ = newwin(status_h, x, dash_h + hist_h + reas_h, 0);
    in_win_ = newwin(in_h, x, y - in_h, 0);
    keypad(in_win_, TRUE);
    
    wrapped_history_.clear();
    for (const auto& raw : raw_history_) {
        auto wrapped = _wrap_text(raw.text, x - 2);
        for (const auto& line : wrapped) wrapped_history_.push_back({line, raw.color, raw.type});
    }
}

void ProTUI::shutdown() { 
    printf("\033[?1003l");
    if (!isendwin()) endwin(); 
    running_ = false; 
}

std::vector<std::string> ProTUI::_wrap_text(const std::string& text, int width) {
    std::vector<std::string> lines;
    if (width < 5) { lines.push_back(text); return lines; }
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); continue; }
        cur += c;
        if ((int)cur.length() >= width - 2) { lines.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

void ProTUI::append_history(const std::string& sender, const std::string& text, const std::string& type) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string full = sender.empty() ? text : fmt::format("{} › {}", sender, text);
    int color = _get_color_for_type(type);
    raw_history_.push_back({full, color, type});
    if (raw_history_.size() > 500) raw_history_.pop_front();
    int x = getmaxx(stdscr);
    auto wrapped = _wrap_text(full, x - 2);
    for (const auto& l : wrapped) wrapped_history_.push_back({l, color, type});
    if (wrapped_history_.size() > 2000) wrapped_history_.erase(wrapped_history_.begin(), wrapped_history_.begin() + 100);
    
    // Smart scrolling: reset scroll_offset_ to 0 (auto-scroll) unless user manually scrolled
    // Reset manual_scroll_ if user scrolls back to bottom
    if (!manual_scroll_) {
        scroll_offset_ = 0;
    }
    
    _render_history();
}

void ProTUI::append_last_history(const std::string& chunk) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (raw_history_.empty()) return;
    raw_history_.back().text += chunk;
    int x = getmaxx(stdscr);
    wrapped_history_.clear();
    for (const auto& item : raw_history_) {
        auto wrapped = _wrap_text(item.text, x - 2);
        for (const auto& l : wrapped) wrapped_history_.push_back({l, item.color, item.type});
    }
    
    // Smart scrolling: reset scroll_offset_ to 0 (auto-scroll) unless user manually scrolled
    if (!manual_scroll_) {
        scroll_offset_ = 0;
    }
    
    _render_history();
}

void ProTUI::toggle_reasoning() {
    std::lock_guard<std::mutex> lock(mtx_);
    show_reasoning_ = !show_reasoning_;
    _setup_windows();
}

void ProTUI::_update_search_results() {
    search_match_indices_.clear();
    if (search_query_.empty()) {
        return;
    }
    // Fuzzy match: find pattern chars in order
    for (size_t i = 0; i < wrapped_history_.size(); ++i) {
        const auto& line = wrapped_history_[i];
        std::string text_lower = line.text;
        std::transform(text_lower.begin(), text_lower.end(), text_lower.begin(), ::tolower);
        std::string query_lower = search_query_;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
        
        size_t query_pos = 0;
        for (size_t text_pos = 0; text_pos < text_lower.length() && query_pos < query_lower.length(); ++text_pos) {
            if (text_lower[text_pos] == query_lower[query_pos]) {
                query_pos++;
            }
        }
        if (query_pos == query_lower.length()) {
            search_match_indices_.push_back((int)i);
        }
    }
}

void ProTUI::_update_command_palette_results() {
    command_match_indices_.clear();
    command_selected_idx_ = 0;
    if (command_palette_query_.empty()) {
        for (size_t i = 0; i < available_commands.size(); ++i) command_match_indices_.push_back((int)i);
        return;
    }
    command_match_indices_ = _fuzzy_match_command(command_palette_query_, available_commands);
}

void ProTUI::_render_command_palette() {
    if (!status_win_) return;
    int y = getmaxy(status_win_);
    int x = getmaxx(status_win_);
    
    // Draw command palette overlay in status line
    werase(status_win_);
    
    // Palette header
    std::string header = "[COMMAND] " + command_palette_query_;
    if ((int)header.length() > x - 1) header = header.substr(0, x - 1);
    wattron(status_win_, COLOR_PAIR(9));
    mvwaddstr(status_win_, 0, 1, header.c_str());
    wattroff(status_win_, COLOR_PAIR(9));
    
    // List matching commands
    int start = 0;
    int visible_count = y - 1;  // Leave room for header
    int total = (int)command_match_indices_.size();
    
    if (command_selected_idx_ >= total) command_selected_idx_ = total > 0 ? total - 1 : 0;
    
    for (int i = 0; i < visible_count && start + i < total; ++i) {
        int cmd_idx = command_match_indices_[start + i];
        const auto& cmd = available_commands[cmd_idx];
        std::string line;
        if (start + i == command_selected_idx_) {
            line = fmt::format("> {}   {}", cmd.name, cmd.description);
            wattron(status_win_, COLOR_PAIR(9) | A_REVERSE);
        } else {
            line = fmt::format("  {}   {}", cmd.name, cmd.description);
            wattron(status_win_, COLOR_PAIR(4));
        }
        if ((int)line.length() > x - 2) line = line.substr(0, x - 2);
        mvwaddstr(status_win_, i + 1, 1, line.c_str());
        wattroff(status_win_, COLOR_PAIR(9) | A_REVERSE);
    }
    wnoutrefresh(status_win_);
}

void ProTUI::_render_history() {
    if (!hist_win_) return;
    werase(hist_win_);
    int h = getmaxy(hist_win_);
    
    // If in command palette mode, render command palette instead of history
    if (input_mode_ == InputMode::COMMAND_PALETTE) {
        _render_command_palette();
        return;
    }
    
    std::vector<Line>* display_list = &wrapped_history_;
    std::vector<int> indices;
    
    if (input_mode_ == InputMode::SEARCH && !search_query_.empty()) {
        indices = search_match_indices_;
    } else {
        // Full list
        int total = (int)wrapped_history_.size();
        int start = std::max(0, total - h - scroll_offset_);
        for (int i = 0; i < h && (start + i) < total; ++i) {
            indices.push_back(start + i);
        }
    }
    
    // If searching, render from filtered results
    if (input_mode_ == InputMode::SEARCH && !search_query_.empty()) {
        int start = std::max(0, scroll_offset_);
        int total = (int)indices.size();
        
        for (int i = 0; i < h && (start + i) < total; ++i) {
            int idx = indices[start + i];
            const auto& l = wrapped_history_[idx];
            
            // Check if current line is a search match (highlight)
            bool is_match = false;
            for (int m : search_match_indices_) {
                if (m == idx) { is_match = true; break; }
            }
            
            if (is_match) {
                wattron(hist_win_, COLOR_PAIR(8));
                mvwaddstr(hist_win_, i, 1, l.text.substr(0, getmaxx(hist_win_) - 2).c_str());
                wattroff(hist_win_, COLOR_PAIR(8));
            } else {
                wattron(hist_win_, COLOR_PAIR(l.color));
                mvwaddstr(hist_win_, i, 1, l.text.substr(0, getmaxx(hist_win_) - 2).c_str());
                wattroff(hist_win_, COLOR_PAIR(l.color));
            }
        }
        wnoutrefresh(hist_win_);
        
        // Draw search prompt in status line
        werase(status_win_);
        std::string search_prompt = fmt::format("[SEARCH] {} ({} matches) | ESC to cancel", search_query_, (int)search_match_indices_.size());
        if ((int)search_prompt.length() > getmaxx(status_win_)) {
            search_prompt = search_prompt.substr(0, getmaxx(status_win_) - 1);
        }
        wattron(status_win_, COLOR_PAIR(8));
        mvwaddstr(status_win_, 0, 1, search_prompt.c_str());
        wattroff(status_win_, COLOR_PAIR(8));
        wnoutrefresh(status_win_);
        return;
    }
    
    // Normal history rendering
    for (int i = 0; i < h && i < (int)indices.size(); ++i) {
        const auto& l = wrapped_history_[indices[i]];
        wattron(hist_win_, COLOR_PAIR(l.color));
        mvwaddstr(hist_win_, i, 1, l.text.substr(0, getmaxx(hist_win_) - 2).c_str());
        wattroff(hist_win_, COLOR_PAIR(l.color));
    }
    wnoutrefresh(hist_win_);
}

std::string ProTUI::_get_bar(double percent, int width) {
    std::string bar;
    int filled = (int)(std::clamp(percent, 0.0, 100.0) * width / 100.0);
    for (int i = 0; i < width; ++i) bar += (i < filled) ? "#" : "-";
    return bar;
}

void ProTUI::_draw_dashboard() {
    if (!dash_win_) return;
    int x = getmaxx(dash_win_);
    sys_stats_ = sys_monitor_.get_stats();
    werase(dash_win_);
    wattron(dash_win_, COLOR_PAIR(1));
    box(dash_win_, 0, 0);
    mvwaddstr(dash_win_, 0, 2, " System Dashboard ");
    wattroff(dash_win_, COLOR_PAIR(1));

    mvwprintw(dash_win_, 1, 2, "RAM: %.1f/%.1fGB", sys_stats_.ram_used_gb, sys_stats_.ram_total_gb);
    wattron(dash_win_, COLOR_PAIR(2));
    mvwaddstr(dash_win_, 1, 22, _get_bar(sys_stats_.ram_percent, 15).c_str());
    wattroff(dash_win_, COLOR_PAIR(2));

    mvwprintw(dash_win_, 1, 40, "Llama CPU: %.1f%%", sys_stats_.llama_cpu_percent);
    wattron(dash_win_, COLOR_PAIR(3));
    mvwaddstr(dash_win_, 1, 58, _get_bar(std::min(100.0, sys_stats_.llama_cpu_percent), 15).c_str());
    wattroff(dash_win_, COLOR_PAIR(3));

    for (size_t i = 0; i < sys_stats_.gpus.size() && i < 2; ++i) {
        int row = 2 + (int)i;
        auto& g = sys_stats_.gpus[i];
        mvwprintw(dash_win_, row, 2, "GPU%d VRAM: %.1f/%.1fGB", (int)i, g.vram_used_gb, g.vram_total_gb);
        wattron(dash_win_, COLOR_PAIR(2));
        mvwaddstr(dash_win_, row, 22, _get_bar(g.vram_percent, 15).c_str());
        wattroff(dash_win_, COLOR_PAIR(2));
        mvwprintw(dash_win_, row, 40, "GPU%d UTIL: %d%%", (int)i, g.busy_percent);
        wattron(dash_win_, COLOR_PAIR(1));
        mvwaddstr(dash_win_, row, 58, _get_bar(g.busy_percent, 15).c_str());
        wattroff(dash_win_, COLOR_PAIR(1));
    }

    std::string perf = fmt::format("pp {:.1f} · gen {:.1f} t/s", metrics_.pp_speed.value_or(0.0), metrics_.gen_speed.value_or(0.0));
    mvwaddstr(dash_win_, 1, x - (int)perf.length() - 2, perf.c_str());
    wnoutrefresh(dash_win_);
}

void ProTUI::_draw_status() {
    if (!status_win_) return;
    
    // Skip drawing status if in search or command palette mode (handled in _render_history)
    if (input_mode_ == InputMode::SEARCH || input_mode_ == InputMode::COMMAND_PALETTE) return;
    
    int x = getmaxx(status_win_);
    werase(status_win_);
    
    std::string status_line;
    int color = 4; // default white
    
    switch (status_type_) {
        case StatusType::IDLE: {
            status_line = fmt::format("{}: ready", topic_);
            color = 4;
            break;
        }
        case StatusType::STREAMING: {
            std::string progress = fmt::format("Streaming... [{}]", activity_);
            if ((int)progress.length() > x) progress = progress.substr(0, x - 1);
            status_line = progress;
            color = 2; // green
            break;
        }
        case StatusType::TOOL: {
            if (!status_detail_.empty()) {
                status_line = fmt::format("[TOOL] {}", status_detail_);
            } else {
                status_line = fmt::format("[TOOL] {}", activity_);
            }
            if ((int)status_line.length() > x) status_line = status_line.substr(0, x - 1);
            color = 3; // yellow/orange
            break;
        }
        case StatusType::ERROR: {
            std::string msg = status_detail_.empty() ? "error" : status_detail_;
            status_line = fmt::format("[ERROR] {}", msg);
            if ((int)status_line.length() > x) status_line = status_line.substr(0, x - 1);
            color = 7; // red
            break;
        }
        case StatusType::PROCESSING: {
            if (!status_detail_.empty()) {
                status_line = fmt::format("[{}] {}", activity_, status_detail_);
            } else {
                status_line = fmt::format("{}...", activity_);
            }
            if ((int)status_line.length() > x) status_line = status_line.substr(0, x - 1);
            color = 6; // orange (color_pair 6)
            break;
        }
    }
    
    wattron(status_win_, COLOR_PAIR(color));
    mvwaddstr(status_win_, 0, 1, status_line.c_str());
    wattroff(status_win_, COLOR_PAIR(color));
    wnoutrefresh(status_win_);
}

void ProTUI::refresh_ui(bool force) {
    std::lock_guard<std::mutex> lock(mtx_);
    _draw_dashboard(); _draw_status(); _draw_borders(); _render_history(); doupdate();
}

std::string ProTUI::get_input() {
    std::string buf; int ch; int pos = 0;
    wtimeout(in_win_, 200); 
    while (true) {
        refresh_ui(); wmove(in_win_, 1, 4 + pos); doupdate();
        ch = wgetch(in_win_);
        if (ch == ERR) continue; 
        if (ch == '\n' || ch == '\r') break;
        
        // Quick action shortcuts
        if (ch == 21) { buf.clear(); pos = 0; continue; } // Ctrl+U: clear input
        if (ch == 12) { scroll_offset_ = 0; werase(hist_win_); manual_scroll_ = false; continue; } // Ctrl+L: clear screen view
        if (ch == 5) { // Ctrl+E: edit last user message
            std::string last_user = "";
            for (int i = (int)raw_history_.size() - 1; i >= 0; --i) {
                if (raw_history_[i].type == "user") {
                    auto& text = raw_history_[i].text;
                    size_t pos_prefix = text.find(" › ");
                    if (pos_prefix != std::string::npos) last_user = text.substr(pos_prefix + 3);
                    else last_user = text;
                    break;
                }
            }
            if (!last_user.empty()) { buf = last_user; pos = buf.length(); }
            continue;
        }
        
        // Command palette mode handling
        if (input_mode_ == InputMode::COMMAND_PALETTE) {
            if (ch == 27) { // ESC: cancel command palette
                input_mode_ = InputMode::NORMAL;
                command_palette_query_.clear();
                command_match_indices_.clear();
                command_selected_idx_ = 0;
                werase(in_win_); _draw_borders(); mvwprintw(in_win_, 1, 4, "%s", buf.c_str());
                continue;
            }
            if (ch == KEY_UP) {
                if (!command_match_indices_.empty()) {
                    command_selected_idx_ = (command_selected_idx_ - 1 + (int)command_match_indices_.size()) % (int)command_match_indices_.size();
                }
                refresh_ui(true);
                continue;
            }
            if (ch == KEY_DOWN) {
                if (!command_match_indices_.empty()) {
                    command_selected_idx_ = (command_selected_idx_ + 1) % (int)command_match_indices_.size();
                }
                refresh_ui(true);
                continue;
            }
            if (ch == '\n' || ch == '\r') {
                // Execute selected command
                if (!command_match_indices_.empty() && command_selected_idx_ < (int)command_match_indices_.size()) {
                    int idx = command_match_indices_[command_selected_idx_];
                    if (idx >= 0 && idx < (int)available_commands.size()) {
                        const auto& cmd = available_commands[idx];
                        // Execute command
                        std::string cmd_name = cmd.name;
                        if (cmd_name == "/reset") {
                            std::lock_guard<std::mutex> lock(mtx_);
                            raw_history_.clear();
                            wrapped_history_.clear();
                            clear_reasoning();
                            scroll_offset_ = 0;
                            set_status(StatusType::IDLE, "conversation reset");
                            refresh_ui(true);
                        } else if (cmd_name == "/clear") {
                            std::lock_guard<std::mutex> lock(mtx_);
                            scroll_offset_ = 0;
                            manual_scroll_ = false;
                            set_status(StatusType::PROCESSING, "view cleared");
                            refresh_ui(true);
                        } else if (cmd_name == "/toggle-reasoning") {
                            toggle_reasoning();
                            set_status(StatusType::PROCESSING, show_reasoning_ ? "reasoning shown" : "reasoning hidden");
                            refresh_ui(true);
                        } else if (cmd_name == "/exit") {
                            shutdown();
                            return "";  // Exit early
                        } else {
                            set_status(StatusType::PROCESSING, "command: " + cmd_name);
                            refresh_ui(true);
                        }
                    }
                }
                // Cancel palette and return to normal
                input_mode_ = InputMode::NORMAL;
                command_palette_query_.clear();
                command_match_indices_.clear();
                command_selected_idx_ = 0;
                refresh_ui(true);
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!command_palette_query_.empty()) command_palette_query_.pop_back();
                _update_command_palette_results();
                refresh_ui(true);
                continue;
            }
            if (ch >= 32 && ch <= 126) {
                command_palette_query_ += (char)ch;
                _update_command_palette_results();
                refresh_ui(true);
                continue;
            }
            // In command palette mode, ignore other keys but refresh
            continue;
        }
        
        // Fuzzy search mode handling
        if (input_mode_ == InputMode::SEARCH) {
            if (ch == 27) { // ESC: cancel search
                input_mode_ = InputMode::NORMAL;
                search_query_.clear();
                scroll_offset_ = 0;
                manual_scroll_ = false;
                werase(in_win_); _draw_borders(); mvwprintw(in_win_, 1, 4, "%s", buf.c_str());
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!search_query_.empty()) search_query_.pop_back();
                _update_search_results();
                refresh_ui(true);
                continue;
            }
            if (ch >= 32 && ch <= 126) {
                search_query_ += (char)ch;
                _update_search_results();
                refresh_ui(true);
                continue;
            }
            // In search mode, ignore other keys but refresh
            continue;
        }
        
        // Alt+R (ESC + 'r') toggle reasoning
        static int last_ch = 0;
        if (last_ch == 27 && ch == 'r') {
            toggle_reasoning();
            set_status(StatusType::PROCESSING, show_reasoning_ ? "reasoning shown" : "reasoning hidden");
            last_ch = 0;
            refresh_ui(true);
            continue;
        }
        last_ch = ch;
        
        // Ctrl+F: enter search mode
        if (ch == 6) { // Ctrl+F
            input_mode_ = InputMode::SEARCH;
            search_query_.clear();
            scroll_offset_ = 0;
            manual_scroll_ = false;
            refresh_ui(true);
            continue;
        }
        
        // Ctrl+P: enter command palette
        if (ch == 16) { // Ctrl+P
            input_mode_ = InputMode::COMMAND_PALETTE;
            command_palette_query_.clear();
            command_selected_idx_ = 0;
            _update_command_palette_results();
            refresh_ui(true);
            continue;
        }
        
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if (pos > 0) { buf.erase(--pos, 1); } }
        else if (ch == KEY_LEFT) { if (pos > 0) pos--; }
        else if (ch == KEY_RIGHT) { if (pos < (int)buf.length()) pos++; }
        else if (ch == KEY_PPAGE) { scroll_offset_ += 5; manual_scroll_ = true; }
        else if (ch == KEY_NPAGE) { scroll_offset_ = std::max(0, scroll_offset_ - 5); manual_scroll_ = true; }
        else if (ch == KEY_MOUSE) { _handle_mouse(); }
        else if (ch == KEY_RESIZE) { _setup_windows(); manual_scroll_ = false; }
        else if (ch == 7) { _toggle_mouse(); } // Ctrl-G
        else if (ch == 24) { add_command_to_history(buf); buf.clear(); pos = 0; } // Ctrl-X
        else if (ch == 18) { // Ctrl-R: recall previous
            std::string prev = get_previous_command();
            if (!prev.empty()) { buf = prev; pos = buf.length(); }
        }
        else if (ch == 16) { // Ctrl-P: recall next (now also palette trigger)
            // If already in command palette mode, this is handled above
            // Otherwise, just ignore (Ctrl+P is now palette)
        }
        else if (ch >= 32 && ch <= 126) { buf.insert(pos++, 1, (char)ch); }
        werase(in_win_); _draw_borders(); mvwprintw(in_win_, 1, 4, "%s", buf.c_str());
    }
    werase(in_win_); return buf;
}

void ProTUI::add_command_to_history(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!cmd.empty()) {
        if (command_history_.empty() || command_history_.back() != cmd) {
            command_history_.push_back(cmd);
            if (command_history_.size() > 100) command_history_.pop_front();
        }
        command_history_pos_ = command_history_.size();
    }
}

std::string ProTUI::get_previous_command() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (command_history_.empty()) return "";
    if (command_history_pos_ == 0) return "";
    command_history_pos_--;
    return command_history_[command_history_pos_];
}

std::string ProTUI::get_next_command() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (command_history_.empty()) return "";
    if (command_history_pos_ >= command_history_.size() - 1) {
        command_history_pos_ = command_history_.size();
        return "";
    }
    command_history_pos_++;
    return command_history_[command_history_pos_];
}

void ProTUI::reset_command_history() {
    std::lock_guard<std::mutex> lock(mtx_);
    command_history_pos_ = command_history_.size();
}

void ProTUI::_toggle_mouse() {
    mouse_enabled_ = !mouse_enabled_;
    if (mouse_enabled_) {
        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
        printf("\033[?1003h");
    } else {
        mousemask(0, NULL);
        printf("\033[?1003l");
    }
}

void ProTUI::_handle_mouse() {
    MEVENT ev; if (getmouse(&ev) == OK) {
        if (ev.bstate & BUTTON4_PRESSED) { scroll_offset_ += 3; manual_scroll_ = true; }
        if (ev.bstate & BUTTON5_PRESSED) { scroll_offset_ = std::max(0, scroll_offset_ - 3); manual_scroll_ = true; }
        
        // Smart scrolling: if user scrolls to bottom, reset manual_scroll_
        int total = (int)wrapped_history_.size();
        if (scroll_offset_ <= 0 || (total > 0 && scroll_offset_ >= total - 10)) {
            manual_scroll_ = false;
        }
        
        // Click-to-copy: detect single click (not drag) on history window
        if (ev.bstate & BUTTON1_CLICKED && ev.y >= 0 && ev.y <= getmaxy(hist_win_)) {
            int clicked_line = scroll_offset_ + ev.y;
            if (clicked_line >= 0 && clicked_line < total) {
                std::string selected = wrapped_history_[clicked_line].text;
                // Copy via xclip if available (Linux)
                std::string copy_cmd = "echo '" + selected + "' | xclip -selection clipboard 2>/dev/null";
                int ret = system(copy_cmd.c_str());
                if (ret != 0) {
                    // Fallback: print to status line
                    set_status(StatusType::PROCESSING, "copied to clipboard");
                }
            }
        }
    }
}

void ProTUI::_draw_borders() {
    auto draw_box = [](WINDOW* w, const char* label) {
        if (!w) return; wattron(w, COLOR_PAIR(1)); box(w, 0, 0);
        if (label) mvwprintw(w, 0, 2, " %s ", label); wattroff(w, COLOR_PAIR(1));
    };
    if (reas_win_) {
        werase(reas_win_); draw_box(reas_win_, show_reasoning_ ? "Thinking" : "[R] Thinking");
        int h, w; getmaxyx(reas_win_, h, w);
        auto wrapped = _wrap_text(current_reasoning_, w - 4);
        int start = std::max(0, (int)wrapped.size() - (h - 2));
        for (int i = 0; i < h - 2 && (start + i) < (int)wrapped.size(); ++i) {
            mvwaddstr(reas_win_, i + 1, 2, wrapped[start + i].substr(0, w-4).c_str());
        }
        wnoutrefresh(reas_win_);
    }
    draw_box(in_win_, "Input"); wnoutrefresh(in_win_);
}

void ProTUI::update_reasoning(const std::string& text, bool append) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (append) current_reasoning_ += text; else current_reasoning_ = text;
}
void ProTUI::clear_reasoning() { std::lock_guard<std::mutex> lock(mtx_); current_reasoning_.clear(); }
void ProTUI::set_metrics(const Metrics& m) { std::lock_guard<std::mutex> lock(mtx_); metrics_ = m; }
void ProTUI::set_activity(const std::string& a) { std::lock_guard<std::mutex> lock(mtx_); activity_ = a; }
void ProTUI::set_topic(const std::string& t) { std::lock_guard<std::mutex> lock(mtx_); topic_ = t; }
void ProTUI::set_streaming(bool streaming) { 
    std::lock_guard<std::mutex> lock(mtx_); 
    streaming_ = streaming;
    if (streaming_) status_type_ = StatusType::STREAMING;
}
void ProTUI::set_status(const std::string& status) { 
    std::lock_guard<std::mutex> lock(mtx_); 
    status_ = status; 
}
void ProTUI::set_status(StatusType type, const std::string& detail) { 
    std::lock_guard<std::mutex> lock(mtx_); 
    status_type_ = type;
    status_detail_ = detail;
}

} // namespace egodeath
