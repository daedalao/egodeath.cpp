#include "tui.hpp"
#include <chrono>
#include <locale.h>
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <sstream>
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
    {"/effort", "Cycle reasoning effort: low/medium/high"},
    {"/theme", "Cycle color theme"},
    {"/mcp", "List MCP servers and tools"},
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

// ── Ctrl+C double-press tracking ─────────────────────────────────────────
static std::atomic<int>       s_sigint_count{0};
static std::atomic<long long> s_first_sigint_ns{0};
static std::atomic<long long> s_last_sigint_ns{0};

void ProTUI::sigint_received() {
    long long now = std::chrono::steady_clock::now().time_since_epoch().count();
    int old = s_sigint_count.fetch_add(1);
    if (old == 0) s_first_sigint_ns.store(now);
    s_last_sigint_ns.store(now);
}

ProTUI::ProTUI() {}
ProTUI::~ProTUI() { shutdown(); }

void ProTUI::init() {
    setlocale(LC_ALL, "");
    initscr(); start_color(); use_default_colors();
    cbreak(); noecho(); keypad(stdscr, TRUE); nonl();
    if (mouse_enabled_) {
        mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
    }
    define_key("\033[200~", KEY_F(60));
    define_key("\033[201~", KEY_F(61));
    fputs("\033[?2004h", stdout);
    fflush(stdout);
    nodelay(stdscr, FALSE);
    
    init_pair(1, COLOR_CYAN, -1);    
    init_pair(2, 46, -1);            
    init_pair(3, 226, -1);           
    init_pair(4, 255, -1);           
    init_pair(5, 0, 252);            // user: black on light gray
    init_pair(6, 160, -1);           
    init_pair(7, 196, -1);  // red for error
    init_pair(8, 214, -1);  // orange for search highlight
    init_pair(9, 27, -1);   // dark blue for command palette selection
    init_pair(10, 242, -1); // user_queued: dim gray, no background
    init_pair(11, 46, 235);  // code block: bright green on dark gray

    _load_themes();
    _apply_theme(theme_);
    _setup_windows();
    running_ = true;
}

void ProTUI::_apply_theme(const std::string& name) {
    auto it = themes_.find(name);
    if (it == themes_.end()) it = themes_.find("dark");
    if (it == themes_.end()) return;
    theme_ = it->first;
    for (const auto& [idx, fb] : it->second)
        init_pair((short)idx, (short)fb.first, (short)fb.second);
}

void ProTUI::_load_themes() {
    themes_.clear(); theme_order_.clear();
    auto add = [&](const std::string& nm, std::map<int, std::pair<int,int>> p) {
        themes_[nm] = std::move(p); theme_order_.push_back(nm);
    };
    add("dark",   {{1,{COLOR_CYAN,-1}},{2,{46,-1}},{3,{226,-1}},{4,{255,-1}},{5,{0,252}},{6,{160,-1}},{7,{196,-1}},{8,{214,-1}},{9,{27,-1}},{10,{242,-1}},{11,{46,235}}});
    add("matrix", {{1,{34,-1}},{2,{46,-1}},{3,{82,-1}},{4,{120,-1}},{5,{0,46}},{6,{22,-1}},{7,{196,-1}},{8,{190,-1}},{9,{28,-1}},{10,{238,-1}},{11,{46,233}}});
    add("amber",  {{1,{130,-1}},{2,{178,-1}},{3,{220,-1}},{4,{222,-1}},{5,{0,214}},{6,{166,-1}},{7,{196,-1}},{8,{208,-1}},{9,{94,-1}},{10,{240,-1}},{11,{220,235}}});
    add("mono",   {{1,{245,-1}},{2,{250,-1}},{3,{252,-1}},{4,{253,-1}},{5,{0,250}},{6,{244,-1}},{7,{203,-1}},{8,{247,-1}},{9,{240,-1}},{10,{240,-1}},{11,{252,236}}});

    namespace fs = std::filesystem;
    fs::path cdir = std::getenv("XDG_CONFIG_HOME")
        ? fs::path(std::getenv("XDG_CONFIG_HOME")) / "egodeath"
        : fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config" / "egodeath";
    std::ifstream tf(cdir / "themes.json");
    if (!tf) return;
    json doc;
    try { std::stringstream ss; ss << tf.rdbuf(); doc = json::parse(ss.str()); } catch (...) { return; }
    if (!doc.is_object()) return;

    static const std::map<std::string,int> roles = {
        {"border",1},{"tool",2},{"accent",3},{"text",4},{"user",5},
        {"alt",6},{"error",7},{"highlight",8},{"selection",9},{"dim",10},{"code",11}
    };
    for (auto& [nm, def] : doc.items()) {
        if (!def.is_object()) continue;
        std::map<int, std::pair<int,int>> p = themes_["dark"]; // inherit dark as base
        for (auto& [role, val] : def.items()) {
            auto ri = roles.find(role);
            if (ri == roles.end()) continue;
            if (val.is_array() && val.size() >= 2 && val[0].is_number_integer() && val[1].is_number_integer())
                p[ri->second] = { val[0].get<int>(), val[1].get<int>() };
        }
        bool isNew = !themes_.count(nm);
        themes_[nm] = std::move(p);
        if (isNew) theme_order_.push_back(nm);
    }
}

void ProTUI::set_theme(const std::string& name) {
    _apply_theme(name);
    refresh_ui(true);
}

void ProTUI::cycle_theme() {
    if (theme_order_.empty()) return;
    int idx = 0;
    for (int i = 0; i < (int)theme_order_.size(); ++i) if (theme_order_[i] == theme_) idx = i;
    set_theme(theme_order_[(idx + 1) % theme_order_.size()]);
}

bool ProTUI::has_theme(const std::string& name) const { return themes_.count(name) > 0; }

void ProTUI::clear_history() {
    std::lock_guard<std::mutex> lock(mtx_);
    raw_history_.clear();
    wrapped_history_.clear();
    scroll_offset_ = 0;
}

int ProTUI::request_tool_approval(const std::string& prompt) {
    std::unique_lock<std::mutex> lk(approval_mtx_);
    approval_prompt_ = prompt;
    approval_result_ = -1;
    approval_pending_.store(true);
    approval_cv_.wait(lk, [this]() { return !approval_pending_.load() || !running_; });
    return approval_result_ < 0 ? 0 : approval_result_;
}

int ProTUI::_get_color_for_type(const std::string& type) const {
    if (type == "user")        return 5;  // black on light gray
    if (type == "user_queued") return 10; // dim gray
    if (type == "assistant")   return 4;  // white
    if (type == "tool")        return 2;  // green
    if (type == "diff")        return 10; // per-line override in _render_history
    if (type == "system")      return 1;  // cyan
    return 4;
}

void ProTUI::_setup_windows() {
    int y, x; getmaxyx(stdscr, y, x);
    
    // Get dynamic height for dashboard based on detected GPUs
    SystemStats stats = sys_monitor_.get_stats();
    int num_gpus = stats.gpus.size();
    int dash_h = 3 + num_gpus;
    
    int in_h = input_lines_ + 2;
    int reas_h = show_reasoning_ ? 5 : 0, status_h = 1;
    int cx0 = rsv_left_;
    int cw = std::max(10, x - rsv_left_ - rsv_right_);
    int cy_avail = std::max(6, y - rsv_bottom_);
    int hist_h = std::max(1, cy_avail - dash_h - in_h - reas_h - status_h);

    if (dash_win_) delwin(dash_win_);
    if (hist_win_) delwin(hist_win_);
    if (reas_win_) delwin(reas_win_);
    if (in_win_) delwin(in_win_);
    if (status_win_) delwin(status_win_);

    dash_win_ = newwin(dash_h, cw, 0, cx0);
    hist_win_ = newwin(hist_h, cw, dash_h, cx0);
    scrollok(hist_win_, TRUE);
    if (show_reasoning_) reas_win_ = newwin(reas_h, cw, dash_h + hist_h, cx0);
    status_win_ = newwin(status_h, cw, dash_h + hist_h + reas_h, cx0);
    in_win_ = newwin(in_h, cw, cy_avail - in_h, cx0);
    keypad(in_win_, TRUE);
    wtimeout(in_win_, 200);
    
    wrapped_history_.clear();
    for (const auto& raw : raw_history_) {
        auto wrapped = _wrap_text(raw.text, cw - 2);
        for (const auto& line : wrapped) wrapped_history_.push_back({line, raw.color, raw.type});
    }
}

void ProTUI::shutdown() { 
    fputs("\033[?2004l", stdout);
    fflush(stdout);
    mousemask(0, NULL);
    if (!isendwin()) endwin(); 
    running_ = false; 
}

std::vector<std::string> ProTUI::_wrap_text(const std::string& text, int width) {
    std::vector<std::string> result;
    if (width < 1) { result.push_back(text); return result; }

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string seg = text.substr(pos, nl - pos);

        if (seg.empty()) {
            result.push_back("");
        } else {
            size_t s = 0;
            while (s < seg.size()) {
                if ((int)(seg.size() - s) <= width) {
                    result.push_back(seg.substr(s));
                    break;
                }
                // Find last space within width to break on a word boundary
                int brk = -1;
                for (int i = width - 1; i >= 0; i--) {
                    if (seg[s + i] == ' ') { brk = i; break; }
                }
                if (brk <= 0) {
                    // No word boundary found — hard break at width
                    result.push_back(seg.substr(s, width));
                    s += width;
                } else {
                    result.push_back(seg.substr(s, brk));
                    s += brk + 1; // skip the space
                }
            }
        }

        if (nl >= text.size()) break;
        pos = nl + 1;
    }

    return result;
}

// Strip ANSI escape sequences and stray C0 control bytes so untrusted content
// (e.g. fetched web pages, shell output) can't corrupt the ncurses display.
static std::string _sanitize_display(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 0x1b) { // ESC: drop a CSI/escape sequence
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) i++;
            }
            continue;
        }
        if (ch == 0x7f) continue;                       // DEL
        if (ch < 0x20 && ch != '\n' && ch != '\t') continue; // other C0 controls
        out += (char)ch;
    }
    return out;
}

void ProTUI::append_history(const std::string& sender, const std::string& text, const std::string& type) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string ctext = _sanitize_display(text);
    std::string full = sender.empty() ? ctext : fmt::format("{} › {}", sender, ctext);
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
    int x = getmaxx(stdscr);
    int width = x - 2;
    // Remove only the wrapped lines belonging to the last raw entry
    int old_count = (int)_wrap_text(raw_history_.back().text, width).size();
    int remove_from = (int)wrapped_history_.size() - old_count;
    if (remove_from >= 0)
        wrapped_history_.erase(wrapped_history_.begin() + remove_from, wrapped_history_.end());
    // Append chunk and re-wrap just the last entry
    raw_history_.back().text += _sanitize_display(chunk);
    auto& last = raw_history_.back();
    for (const auto& l : _wrap_text(last.text, width))
        wrapped_history_.push_back({l, last.color, last.type});
    if (wrapped_history_.size() > 2000)
        wrapped_history_.erase(wrapped_history_.begin(), wrapped_history_.begin() + 100);
    if (!manual_scroll_) scroll_offset_ = 0;
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

void ProTUI::open_editor(const std::string& path) {
    if (path.empty()) { set_status(StatusType::IDLE, "no file to edit (try /edit <path>)"); refresh_ui(true); return; }
    int y, x; getmaxyx(stdscr, y, x);
    int ex = 0, ey = 0, ew = x, eh = y;
    const std::string& dock = editor_dock_;
    if (dock == "bottom")      { eh = std::max(8, (int)(y * 0.45)); ew = x; ex = 0; ey = y - eh; rsv_bottom_ = eh; }
    else if (dock == "left")   { ew = std::max(30, (int)(x * 0.5)); eh = y; ex = 0; ey = 0; rsv_left_ = ew; }
    else if (dock == "right")  { ew = std::max(30, (int)(x * 0.5)); eh = y; ey = 0; ex = x - ew; rsv_right_ = ew; }
    // "full" reserves nothing and covers the whole screen.

    if (dock != "full") { _setup_windows(); refresh_ui(true); } // draw chat in its reduced area first
    editor_active_.store(true);
    editor_.run(path, ex, ey, ew, eh);
    editor_active_.store(false);
    rsv_left_ = rsv_right_ = rsv_bottom_ = 0;
    curs_set(0);
    _setup_windows();
    refresh_ui(true);
}

void ProTUI::open_agenda() {
    input_mode_ = InputMode::AGENDA;
    agenda_sel_ = 0;
    _agenda_refetch();
    refresh_ui(true);
}

void ProTUI::_agenda_refetch() {
    if (agenda_provider_) {
        try { agenda_items_ = agenda_provider_(); } catch (...) { agenda_items_ = json::array(); }
    }
    if (!agenda_items_.is_array()) agenda_items_ = json::array();
}

std::string ProTUI::_agenda_add_prompt() {
    if (!in_win_) return "";
    werase(in_win_);
    wattron(in_win_, COLOR_PAIR(1));
    box(in_win_, 0, 0);
    wattroff(in_win_, COLOR_PAIR(1));
    mvwaddstr(in_win_, 1, 2, "new task (Enter to add, empty to cancel): ");
    wnoutrefresh(in_win_); doupdate();
    curs_set(1); echo(); wtimeout(in_win_, -1);
    char buf[256] = {0};
    wgetnstr(in_win_, buf, sizeof(buf) - 1);
    noecho(); curs_set(0); wtimeout(in_win_, 200);
    std::string s = buf;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

void ProTUI::_render_agenda() {
    if (!hist_win_) return;
    werase(hist_win_);
    int h = getmaxy(hist_win_), w = getmaxx(hist_win_);
    auto vlen = [](const std::string& s) { int n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++; return n; };

    // Partition into tasks then events; record the selectable id order.
    std::vector<int> tasks, events;
    for (int i = 0; i < (int)agenda_items_.size(); ++i)
        (agenda_items_[i].value("kind", "task") == "event" ? events : tasks).push_back(i);
    agenda_order_ids_.clear();
    for (int i : tasks)  agenda_order_ids_.push_back(agenda_items_[i].value("id", (long long)0));
    for (int i : events) agenda_order_ids_.push_back(agenda_items_[i].value("id", (long long)0));
    if (agenda_sel_ >= (int)agenda_order_ids_.size()) agenda_sel_ = (int)agenda_order_ids_.size() - 1;
    if (agenda_sel_ < 0) agenda_sel_ = 0;

    int r = 0;
    auto sep = [&](const std::string& t) {
        if (r >= h) return;
        wattron(hist_win_, COLOR_PAIR(1));
        std::string s = "\u2500\u2500 " + t + " ";
        s = std::string("\xe2\x94\x80\xe2\x94\x80 ") + t + " ";
        while (vlen(s) < w - 1) s += "\xe2\x94\x80";
        mvwaddstr(hist_win_, r++, 0, s.c_str());
        wattroff(hist_win_, COLOR_PAIR(1));
    };

    wattron(hist_win_, A_BOLD | COLOR_PAIR(3));
    mvwaddstr(hist_win_, r++, 0, "AGENDA");
    wattroff(hist_win_, A_BOLD | COLOR_PAIR(3));

    int sidx = 0;
    auto draw_item = [&](int arrIdx) {
        if (r >= h) return;
        const auto& it = agenda_items_[arrIdx];
        bool selected = (sidx == agenda_sel_);
        std::string kind = it.value("kind", "task");
        std::string status = it.value("status", "open");
        std::string box = status == "done" ? "[x]" : status == "cancelled" ? "[-]" : "[ ]";
        std::string pri = it.value("priority", "");
        std::string pritag = pri == "high" ? " !hi" : pri == "med" ? " ~md" : pri == "low" ? " .lo" : "";
        std::string when = kind == "event" ? it.value("start", "") : it.value("due", "");
        long long id = it.value("id", (long long)0);
        std::string left = std::string(selected ? "\xe2\x96\xb8" : " ") + " " + box +
                           " #" + std::to_string(id) + " " + it.value("title", "") + pritag;
        std::string full = left;
        int pad = w - vlen(left) - (int)when.size() - 1;
        if (pad > 0 && !when.empty()) full += std::string(pad, ' ') + when;
        int color = status == "done" ? 10 : (pri == "high" ? 7 : 4);
        if (selected) wattron(hist_win_, A_REVERSE | COLOR_PAIR(color));
        else          wattron(hist_win_, COLOR_PAIR(color));
        mvwaddstr(hist_win_, r, 0, full.c_str());
        if (selected) wattroff(hist_win_, A_REVERSE | COLOR_PAIR(color));
        else          wattroff(hist_win_, COLOR_PAIR(color));
        r++; sidx++;
    };
    auto none = [&]() {
        if (r >= h) return;
        wattron(hist_win_, COLOR_PAIR(10));
        mvwaddstr(hist_win_, r++, 2, "(none)");
        wattroff(hist_win_, COLOR_PAIR(10));
    };

    sep("tasks (" + std::to_string(tasks.size()) + ")");
    if (tasks.empty()) none(); else for (int i : tasks) draw_item(i);
    sep("calendar (" + std::to_string(events.size()) + ")");
    if (events.empty()) none(); else for (int i : events) draw_item(i);
    wnoutrefresh(hist_win_);

    werase(status_win_);
    wattron(status_win_, COLOR_PAIR(9));
    mvwaddstr(status_win_, 0, 1, "AGENDA  j/k move  space toggle done  a add  d delete  r refresh  q/Esc exit");
    wattroff(status_win_, COLOR_PAIR(9));
    wnoutrefresh(status_win_);
}

void ProTUI::_render_help() {
    if (!hist_win_) return;
    int h = getmaxy(hist_win_);
    int w = getmaxx(hist_win_);
    werase(hist_win_);

    using Row = std::pair<std::string,std::string>;
    static const std::vector<Row> rows = {
        {"CURSOR & EDITING",                "NAVIGATION & SEARCH"},
        {"  Ctrl+A    beginning of line",   "  PgUp/PgDn   scroll history"},
        {"  Ctrl+E    end of line",         "  Ctrl+F      fuzzy search"},
        {"  Ctrl+B    back one char",       "  Ctrl+P      command palette"},
        {"  Ctrl+F    forward one char",    "  Ctrl+L      reset scroll"},
        {"  Ctrl+U    kill entire line",    ""},
        {"  Ctrl+K    kill to end of line", "REASONING PANEL"},
        {"  Ctrl+W    delete word back",    "  Ctrl+T      toggle show/hide"},
        {"  Ctrl+D    delete char forward", "  Ctrl+Y      scroll up"},
        {"  Backspace delete char back",    "  Ctrl+N      scroll down"},
        {"",""},
        {"SUBMISSION",                      "HISTORY"},
        {"  Enter      submit prompt",      "  Ctrl+R      recall previous"},
        {"  Ctrl+J     insert newline",     "  Ctrl+X      save to cmd history"},
        {"  Paste      multi-line paste",   "OTHER"},
        {"  Ctrl+U     cancel staged paste","  Ctrl+G      toggle mouse"},
        {"",""},
        {"COMMANDS  (type in input, or Ctrl+P for palette)",""},
        {"  /save /load   session save/resume",  "  /web on|off    web search tool"},
        {"  /reset        clear conversation",   "  /shell on|off  shell tool"},
        {"  /clear        reset scroll",         "  /auto on|off   auto-approve tools"},
        {"  /toggle-reasoning  reasoning",       "  /mcp           list MCP servers"},
        {"  /effort low|medium|high",            "  /undo          undo last file write"},
        {"  /theme dark|matrix|amber|mono",      "  /exit          quit"},
        {"  ! <cmd>  run a shell command",       "  @path          attach/inject a file"},
        {"  /agenda /tasks  task & calendar view", "  F2             toggle agenda view"},
        {"  /edit <path>    open code editor",     "  F3             edit last agent file"},
        {"  /editdock l|r|bottom|full  pane side", "  (vim keys: :q quit, i insert)"},
    };

    int c2 = w / 2;
    int r = 0;

    // Top border
    wattron(hist_win_, COLOR_PAIR(1));
    std::string top = "\xe2\x94\x8c\xe2\x94\x80 Keyboard Shortcuts ";
    while ((int)top.size() < w - 4) top += "\xe2\x94\x80";
    top += "\xe2\x94\x90";
    if (r < h) { mvwaddstr(hist_win_, r++, 0, top.substr(0, w).c_str()); }
    wattroff(hist_win_, COLOR_PAIR(1));

    for (const auto& [left, right] : rows) {
        if (r >= h - 1) break;
        // Side bars
        wattron(hist_win_, COLOR_PAIR(1));
        mvwaddstr(hist_win_, r, 0, "\xe2\x94\x82");
        mvwaddstr(hist_win_, r, w - 3, "\xe2\x94\x82");
        wattroff(hist_win_, COLOR_PAIR(1));
        // Left column
        bool lhead = !left.empty() && left[0] != ' ' && left.find('/') == std::string::npos;
        if (lhead) wattron(hist_win_, A_BOLD | COLOR_PAIR(2));
        else        wattron(hist_win_, COLOR_PAIR(4));
        std::string lstr = " " + left;
        if ((int)lstr.size() > c2 - 1) lstr = lstr.substr(0, c2 - 1);
        mvwaddstr(hist_win_, r, 1, lstr.c_str());
        if (lhead) wattroff(hist_win_, A_BOLD | COLOR_PAIR(2));
        else        wattroff(hist_win_, COLOR_PAIR(4));
        // Right column
        if (!right.empty()) {
            bool rhead = right[0] != ' ';
            if (rhead) wattron(hist_win_, A_BOLD | COLOR_PAIR(2));
            else        wattron(hist_win_, COLOR_PAIR(4));
            std::string rstr = " " + right;
            if ((int)rstr.size() > w - c2 - 4) rstr = rstr.substr(0, w - c2 - 4);
            mvwaddstr(hist_win_, r, c2, rstr.c_str());
            if (rhead) wattroff(hist_win_, A_BOLD | COLOR_PAIR(2));
            else        wattroff(hist_win_, COLOR_PAIR(4));
        }
        r++;
    }

    // Bottom border with dismiss hint
    if (r < h) {
        wattron(hist_win_, COLOR_PAIR(1));
        std::string bot = "\xe2\x94\x94";
        while ((int)bot.size() < w - 4) bot += "\xe2\x94\x80";
        bot += "\xe2\x94\x98";
        mvwaddstr(hist_win_, r, 0, bot.substr(0, w).c_str());
        std::string hint = " F1 or any key to close ";
        int hc = (w - (int)hint.size()) / 2;
        if (hc > 1) mvwaddstr(hist_win_, r, hc, hint.c_str());
        wattroff(hist_win_, COLOR_PAIR(1));
    }
    wnoutrefresh(hist_win_);
}

static bool _is_code_fence(const std::string& text) {
    size_t p = text.find_first_not_of(' ');
    return p != std::string::npos && text.size() >= p + 3 && text.substr(p, 3) == "```";
}

void ProTUI::_render_history() {
    if (editor_active_.load()) return;
    if (!hist_win_) return;
    werase(hist_win_);
    int h = getmaxy(hist_win_);

    if (input_mode_ == InputMode::HELP) { _render_help(); return; }
    if (input_mode_ == InputMode::AGENDA) { _render_agenda(); return; }

    // If in command palette mode, render command palette instead of history
    if (input_mode_ == InputMode::COMMAND_PALETTE) {
        _render_command_palette();
        return;
    }
    
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
    
    // Normal history rendering with markdown
    // Pre-scan: determine code-block state before the visible window
    bool _cb = false;
    if (!indices.empty()) {
        for (int _j = 0; _j < indices[0]; _j++) {
            const auto& _l = wrapped_history_[_j];
            if ((_l.type == "assistant") && _is_code_fence(_l.text))
                _cb = !_cb;
        }
    }
    int _w = getmaxx(hist_win_) - 2;
    for (int i = 0; i < h && i < (int)indices.size(); ++i) {
        const auto& l = wrapped_history_[indices[i]];

        // Tool calls: draw a left gutter that groups a run of consecutive tool lines.
        if (l.type == "tool") {
            int gi = indices[i];
            bool prev_t = gi > 0 && wrapped_history_[gi - 1].type == "tool";
            bool next_t = gi + 1 < (int)wrapped_history_.size() && wrapped_history_[gi + 1].type == "tool";
            const char* g = (!prev_t && next_t) ? "\u256d"
                          : (prev_t && next_t)  ? "\u2502"
                          : (prev_t && !next_t) ? "\u2570"
                                                : "\u2500";
            wattron(hist_win_, COLOR_PAIR(10));
            mvwaddstr(hist_win_, i, 1, g);
            wattroff(hist_win_, COLOR_PAIR(10));
            wattron(hist_win_, COLOR_PAIR(l.color));
            mvwaddstr(hist_win_, i, 3, l.text.substr(0, std::max(0, _w - 2)).c_str());
            wattroff(hist_win_, COLOR_PAIR(l.color));
            continue;
        }

        if (l.type == "diff") {
            int dc = (!l.text.empty() && l.text[0] == '+') ? 2
                   : (!l.text.empty() && l.text[0] == '-') ? 7 : 10;
            wattron(hist_win_, COLOR_PAIR(dc));
            mvwaddstr(hist_win_, i, 1, l.text.substr(0, _w).c_str());
            wattroff(hist_win_, COLOR_PAIR(dc));
            continue;
        }

        bool is_ast = (l.type == "assistant");

        // Determine code-block state for this line
        bool is_fence = is_ast && _is_code_fence(l.text);
        bool old_cb = _cb;
        if (is_fence) _cb = !_cb;
        bool in_cb = old_cb || is_fence; // code fences + everything between them

        // Header detection (lines starting with # that aren't in a code block)
        bool is_header = is_ast && !in_cb && !l.text.empty() &&
                         l.text[l.text.find_first_not_of(' ') != std::string::npos ?
                                l.text.find_first_not_of(' ') : 0] == '#';

        if (in_cb) {
            // Code block: green on dark-gray background
            wattron(hist_win_, COLOR_PAIR(11));
            if (is_fence) wattron(hist_win_, A_BOLD);
            mvwaddstr(hist_win_, i, 1, l.text.substr(0, _w).c_str());
            wattroff(hist_win_, A_BOLD);
            wattroff(hist_win_, COLOR_PAIR(11));
        } else if (is_header) {
            // Header: bold bright white
            wattron(hist_win_, A_BOLD | COLOR_PAIR(l.color));
            mvwaddstr(hist_win_, i, 1, l.text.substr(0, _w).c_str());
            wattroff(hist_win_, A_BOLD | COLOR_PAIR(l.color));
        } else if (is_ast && l.text.find('`') != std::string::npos) {
            // Inline code: render char-by-char toggling color on backtick pairs
            wattron(hist_win_, COLOR_PAIR(l.color));
            bool ic = false;
            int x = 1;
            size_t j = 0;
            std::string txt = l.text.substr(0, _w);
            while (j < txt.size() && x < _w + 1) {
                if (txt[j] == '`') {
                    ic = !ic;
                    if (ic) { wattroff(hist_win_, COLOR_PAIR(l.color)); wattron(hist_win_, COLOR_PAIR(11)); }
                    else    { wattroff(hist_win_, COLOR_PAIR(11));       wattron(hist_win_, COLOR_PAIR(l.color)); }
                    j++; continue;
                }
                mvwaddch(hist_win_, i, x++, (unsigned char)txt[j++]);
            }
            wattroff(hist_win_, COLOR_PAIR(l.color));
            wattroff(hist_win_, COLOR_PAIR(11));
        } else {
            // Plain line
            wattron(hist_win_, COLOR_PAIR(l.color));
            mvwaddstr(hist_win_, i, 1, l.text.substr(0, _w).c_str());
            wattroff(hist_win_, COLOR_PAIR(l.color));
        }
    }
    wnoutrefresh(hist_win_);
}

std::string ProTUI::_get_sparkline(const std::vector<double>& values, int width) {
    static const char* blocks[8] = {"\u2581","\u2582","\u2583","\u2584","\u2585","\u2586","\u2587","\u2588"};
    if (values.empty() || width <= 0) return "";
    int start = std::max(0, (int)values.size() - width);
    double mx = 0.0;
    for (int i = start; i < (int)values.size(); ++i) mx = std::max(mx, values[i]);
    std::string out;
    for (int i = start; i < (int)values.size(); ++i) {
        int level = (mx > 0.0) ? (int)(values[i] / mx * 7.0 + 0.5) : 0;
        if (level < 0) level = 0;
        if (level > 7) level = 7;
        out += blocks[level];
    }
    return out;
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
    {
        std::string eff = " effort: " + reasoning_effort_ + " ";
        if ((int)eff.size() + 4 < x) mvwaddstr(dash_win_, 0, x - (int)eff.size() - 2, eff.c_str());
    }
    wattroff(dash_win_, COLOR_PAIR(1));

    mvwprintw(dash_win_, 1, 2, "RAM: %.1f/%.1fGB", sys_stats_.ram_used_gb, sys_stats_.ram_total_gb);
    wattron(dash_win_, COLOR_PAIR(2));
    mvwaddstr(dash_win_, 1, 22, _get_bar(sys_stats_.ram_percent, 15).c_str());
    wattroff(dash_win_, COLOR_PAIR(2));

    {
        // CPU name + activity — bar pinned to col 58, same column as GPU UTIL bars
        // ": 100.0%" = 8 chars; leave 1-char gap → name fits in 9 chars max (cols 40-56)
        std::string cpu_lbl = sys_stats_.cpu_name.empty() ? "CPU" : sys_stats_.cpu_name;
        if ((int)cpu_lbl.size() > 9) cpu_lbl = cpu_lbl.substr(0, 8) + "~";
        mvwprintw(dash_win_, 1, 40, "%-9s: %.1f%%", cpu_lbl.c_str(), sys_stats_.llama_cpu_percent);
        wattron(dash_win_, COLOR_PAIR(3));
        mvwaddstr(dash_win_, 1, 58, _get_bar(std::min(100.0, sys_stats_.llama_cpu_percent), 15).c_str());
        wattroff(dash_win_, COLOR_PAIR(3));
    }

    for (size_t i = 0; i < sys_stats_.gpus.size(); ++i) {
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

        // Render GPU name to the right of the utilization bar if space permits
        if (x > 78) {
            std::string name_str = " · " + g.name;
            int max_len = x - 76;
            if (max_len > 0) {
                if ((int)name_str.length() > max_len) {
                    name_str = name_str.substr(0, max_len - 3) + "...";
                }
                mvwprintw(dash_win_, row, 75, "%s", name_str.c_str());
            }
        }
        // Context usage right-aligned on the first GPU row
        if (i == 0) {
            std::string ctx_str;
            if (metrics_.ctx_size.has_value() && metrics_.ctx_size.value() > 0) {
                int used  = metrics_.ctx_used.value_or(0);
                int total = metrics_.ctx_size.value();
                int pct   = total > 0 ? (100 * used / total) : 0;
                ctx_str = fmt::format("ctx {:d}% / {:d}", pct, total);
            } else {
                ctx_str = "ctx ─";
            }
            mvwaddstr(dash_win_, row, x - (int)ctx_str.length() - 2, ctx_str.c_str());
        }
    }

    std::string perf = fmt::format("pp {:.1f} · gen {:.1f} t/s", metrics_.pp_speed.value_or(0.0), metrics_.gen_speed.value_or(0.0));
    mvwaddstr(dash_win_, 1, x - (int)perf.length() - 2, perf.c_str());
    wnoutrefresh(dash_win_);
}

void ProTUI::_draw_status() {
    if (!status_win_) return;

    if (approval_pending_.load()) {
        werase(status_win_);
        std::string p = approval_prompt_;
        int sx = getmaxx(status_win_);
        if ((int)p.size() > sx - 2) p = p.substr(0, sx - 2);
        wattron(status_win_, COLOR_PAIR(8) | A_BOLD);
        mvwaddstr(status_win_, 0, 1, p.c_str());
        wattroff(status_win_, COLOR_PAIR(8) | A_BOLD);
        wnoutrefresh(status_win_);
        return;
    }
    
    // Skip drawing status if in search or command palette mode (handled in _render_history)
    if (input_mode_ == InputMode::SEARCH || input_mode_ == InputMode::COMMAND_PALETTE || input_mode_ == InputMode::HELP) return;
    
    int x = getmaxx(status_win_);
    werase(status_win_);
    
    std::string status_line;
    int color = 4; // default white
    
    switch (status_type_) {
        case StatusType::IDLE: {
            if (status_detail_.empty())
                status_line = fmt::format("{}: ready", topic_);
            else
                status_line = fmt::format("{}: ready  ·  {}", topic_, status_detail_);
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
    
    if (queued_depth_ > 0) {
        std::string q = fmt::format("  [{} queued]", queued_depth_);
        status_line += q;
    }
    if ((int)status_line.size() > x - 2) status_line = status_line.substr(0, x - 2);
    wattron(status_win_, COLOR_PAIR(color));
    mvwaddstr(status_win_, 0, 1, status_line.c_str());
    wattroff(status_win_, COLOR_PAIR(color));
    wnoutrefresh(status_win_);
}

void ProTUI::refresh_ui(bool force) {
    if (editor_active_.load()) return;
    std::lock_guard<std::mutex> lock(mtx_);
    _draw_dashboard(); _draw_status(); _draw_borders(); _render_history(); doupdate();
}

std::string ProTUI::get_input() {
    std::string buf; int ch; int pos = 0;
    wtimeout(in_win_, 200); 

    auto redraw_input = [&]() {
        int text_width = getmaxx(in_win_) - 5;
        if (text_width <= 0) text_width = 1;
        
        // Wrap input text with manual newlines handled
        std::vector<std::string> wrapped_lines;
        std::string current_line;
        for (char c : buf) {
            if (c == '\n') {
                wrapped_lines.push_back(current_line);
                current_line.clear();
                continue;
            }
            current_line += c;
            if ((int)current_line.length() >= text_width) {
                wrapped_lines.push_back(current_line);
                current_line.clear();
            }
        }
        wrapped_lines.push_back(current_line);
        
        int total_lines = wrapped_lines.size();
        
        // Determine cursor line & col
        int cursor_line = 0;
        int cursor_col = 0;
        int current_line_len = 0;
        for (int i = 0; i < pos; ++i) {
            if (buf[i] == '\n') {
                cursor_line++;
                cursor_col = 0;
                current_line_len = 0;
            } else {
                cursor_col++;
                current_line_len++;
                if (current_line_len >= text_width) {
                    cursor_line++;
                    cursor_col = 0;
                    current_line_len = 0;
                }
            }
        }
        
        // Capped input box size to 3 lines of text (total height 5)
        int needed_lines = std::min(3, total_lines);
        if (needed_lines != input_lines_) {
            input_lines_ = needed_lines;
            _setup_windows();
            text_width = getmaxx(in_win_) - 5;
            if (text_width <= 0) text_width = 1;
        }
        
        // Scrolling offset for input text
        int start_line = 0;
        if (cursor_line >= 3) {
            start_line = cursor_line - 2;
        }
        
        werase(in_win_);
        _draw_borders();
        for (int r = 0; r < input_lines_; ++r) {
            int line_idx = start_line + r;
            if (line_idx < (int)wrapped_lines.size()) {
                mvwprintw(in_win_, 1 + r, 4, "%s", wrapped_lines[line_idx].c_str());
            }
        }
    };

    while (true) {
        // Tool-approval prompt requested by the worker thread (y/n/a).
        if (approval_pending_.load()) {
            refresh_ui();
            int ach = wgetch(in_win_);
            if (ach != ERR) {
                int res = -1;
                if (ach == 'y' || ach == 'Y') res = 1;
                else if (ach == 'a' || ach == 'A') res = 2;
                else if (ach == 'n' || ach == 'N' || ach == 27) res = 0;
                if (res >= 0) {
                    { std::lock_guard<std::mutex> lk(approval_mtx_); approval_result_ = res; }
                    approval_pending_.store(false);
                    approval_cv_.notify_all();
                }
            }
            continue;
        }

        int text_width = getmaxx(in_win_) - 5;
        if (text_width <= 0) text_width = 1;
        
        // Determine cursor line & col
        int cursor_line = 0;
        int cursor_col = 0;
        int current_line_len = 0;
        for (int i = 0; i < pos; ++i) {
            if (buf[i] == '\n') {
                cursor_line++;
                cursor_col = 0;
                current_line_len = 0;
            } else {
                cursor_col++;
                current_line_len++;
                if (current_line_len >= text_width) {
                    cursor_line++;
                    cursor_col = 0;
                    current_line_len = 0;
                }
            }
        }
        
        int start_line = 0;
        if (cursor_line >= 3) {
            start_line = cursor_line - 2;
        }
        
        refresh_ui(); 
        wmove(in_win_, 1 + (cursor_line - start_line), 4 + cursor_col); 
        doupdate();
        
        ch = wgetch(in_win_);
        if (ch == ERR) {
            // Agent-requested editor open (set from a background tool call).
            if (input_mode_ == InputMode::NORMAL && !editor_active_.load() && !approval_pending_.load()) {
                std::string ep;
                { std::lock_guard<std::mutex> lk(pending_editor_mtx_); ep.swap(pending_editor_path_); }
                if (!ep.empty()) { open_editor(ep); continue; }
            }
            int sc = s_sigint_count.load();
            if (sc >= 1) {
                long long now = std::chrono::steady_clock::now().time_since_epoch().count();
                long long first = s_first_sigint_ns.load();
                long long last  = s_last_sigint_ns.load();
                if (sc >= 2 && (last - first) < 2'000'000'000LL) {
                    // Second press within 2 s → clean exit
                    shutdown();
                    return "";
                }
                if (now - first > 3'000'000'000LL) {
                    // Timed out → reset
                    s_sigint_count.store(0);
                    set_status(StatusType::IDLE, "");
                    refresh_ui(true);
                } else {
                    set_status(StatusType::ERROR, "press Ctrl+C again to quit");
                    refresh_ui(true);
                }
            }
            continue;
        }
        // Any keystroke cancels a pending Ctrl+C warning
        if (s_sigint_count.load() > 0) {
            s_sigint_count.store(0);
            set_status(StatusType::IDLE, "");
        }

        int h_reas = reas_win_ ? getmaxy(reas_win_) : 5;

        if (ch == KEY_F(1)) {
            input_mode_ = (input_mode_ == InputMode::HELP) ? InputMode::NORMAL : InputMode::HELP;
            refresh_ui(true); continue;
        }
        if (input_mode_ == InputMode::HELP) {
            input_mode_ = InputMode::NORMAL; refresh_ui(true); continue;
        }

        if (ch == KEY_F(2)) {
            if (input_mode_ == InputMode::AGENDA) { input_mode_ = InputMode::NORMAL; set_status(StatusType::IDLE, ""); refresh_ui(true); }
            else open_agenda();
            continue;
        }
        if (ch == KEY_F(3)) {
            open_editor(last_file_provider_ ? last_file_provider_() : std::string());
            continue;
        }
        if (input_mode_ == InputMode::AGENDA) {
            int n = (int)agenda_order_ids_.size();
            long long sel_id = (agenda_sel_ >= 0 && agenda_sel_ < n) ? agenda_order_ids_[agenda_sel_] : 0;
            if (ch == 'q' || ch == 27) { input_mode_ = InputMode::NORMAL; set_status(StatusType::IDLE, ""); refresh_ui(true); }
            else if (ch == 'j' || ch == KEY_DOWN) { if (agenda_sel_ < n - 1) agenda_sel_++; refresh_ui(true); }
            else if (ch == 'k' || ch == KEY_UP)   { if (agenda_sel_ > 0) agenda_sel_--; refresh_ui(true); }
            else if (ch == 'r') { _agenda_refetch(); refresh_ui(true); }
            else if ((ch == ' ' || ch == '\n' || ch == 13) && sel_id && agenda_action_) {
                agenda_action_("toggle", sel_id, ""); _agenda_refetch(); refresh_ui(true);
            }
            else if (ch == 'd' && sel_id && agenda_action_) {
                agenda_action_("delete", sel_id, ""); _agenda_refetch(); refresh_ui(true);
            }
            else if (ch == 'a') {
                std::string title = _agenda_add_prompt();
                if (!title.empty() && agenda_action_) agenda_action_("add", 0, title);
                _agenda_refetch(); refresh_ui(true);
            }
            continue;
        }

        // ESC: Alt+Enter newline only
        if (ch == 27) {
            wtimeout(in_win_, 0);
            int next_ch = wgetch(in_win_);
            wtimeout(in_win_, 200);
            if (next_ch == 13 || next_ch == 10) {
                buf.insert(pos++, 1, '\n');
                redraw_input();
                continue;
            }
            if (next_ch != ERR) ungetch(next_ch);
            else if (cancel_callback_) cancel_callback_(); // bare Esc → cancel stream
        }
        
        if (ch == 13) break; // Enter (CR) submits the prompt (under nonl() mode)
        if (ch == 10) { // Shift+Enter or Ctrl+J (LF) inserts a newline
            buf.insert(pos++, 1, '\n');
            redraw_input();
            continue;
        }
        
        // ── Standard readline / editing shortcuts ────────────────────────────
        // Cursor movement
        if (ch == 1)  { pos = 0; redraw_input(); continue; }                   // Ctrl+A: beginning of line
        if (ch == 5)  { pos = (int)buf.size(); redraw_input(); continue; }     // Ctrl+E: end of line
        if (ch == 2)  { if (pos > 0) { pos--; redraw_input(); } continue; }    // Ctrl+B: back one char
        if (ch == 6 && input_mode_ != InputMode::SEARCH) {                     // Ctrl+F: forward one char (when not in search)
            if (pos < (int)buf.size()) { pos++; redraw_input(); } continue;
        }
        // Deletion
        if (ch == 21) { buf.clear(); pending_paste_.clear(); pos = 0; redraw_input(); continue; } // Ctrl+U: kill whole line
        if (ch == 11) { // Ctrl+K: kill to end of line
            buf.erase(pos); pending_paste_.clear(); redraw_input(); continue;
        }
        if (ch == 4) { // Ctrl+D: delete char forward (ignore on empty)
            if (!buf.empty() && pos < (int)buf.size()) { buf.erase(pos, 1); redraw_input(); }
            continue;
        }
        if (ch == 23) { // Ctrl+W: delete word backwards
            if (pos > 0 && !buf.empty()) {
                int s = pos - 1;
                while (s > 0 && buf[s - 1] == ' ') s--;   // skip trailing spaces
                while (s > 0 && buf[s - 1] != ' ') s--;   // skip the word
                buf.erase(s, pos - s);
                pos = s;
                redraw_input();
            }
            continue;
        }
        // View / panel shortcuts
        if (ch == 12) { scroll_offset_ = 0; werase(hist_win_); manual_scroll_ = false; continue; } // Ctrl+L: clear screen view
        if (ch == 20) { // Ctrl+T: toggle reasoning panel
            toggle_reasoning(); reas_scroll_offset_ = 0;
            set_status(StatusType::PROCESSING, show_reasoning_ ? "reasoning shown" : "reasoning hidden");
            refresh_ui(true); continue;
        }
        if (ch == 25) { // Ctrl+Y: scroll reasoning up
            reas_scroll_offset_ += (h_reas > 2 ? h_reas - 2 : 1);
            refresh_ui(true); continue;
        }
        if (ch == 14) { // Ctrl+N: scroll reasoning down
            reas_scroll_offset_ = std::max(0, reas_scroll_offset_ - (h_reas > 2 ? h_reas - 2 : 1));
            refresh_ui(true); continue;
        }
        
        // Command palette mode handling
        if (input_mode_ == InputMode::COMMAND_PALETTE) {
            if (ch == 27) { // ESC: cancel command palette
                input_mode_ = InputMode::NORMAL;
                command_palette_query_.clear();
                command_match_indices_.clear();
                command_selected_idx_ = 0;
                redraw_input();
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
            if (ch == 13 || ch == 10) {
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
                            current_reasoning_.clear();
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
                        } else if (cmd_name == "/effort") {
                            if (reasoning_effort_ == "low") reasoning_effort_ = "medium";
                            else if (reasoning_effort_ == "medium") reasoning_effort_ = "high";
                            else reasoning_effort_ = "low";
                            if (effort_callback_) effort_callback_(reasoning_effort_);
                            set_status(StatusType::PROCESSING, "reasoning effort: " + reasoning_effort_);
                            refresh_ui(true);
                        } else if (cmd_name == "/theme") {
                            cycle_theme();
                            set_status(StatusType::PROCESSING, "theme: " + theme_);
                            refresh_ui(true);
                        } else if (cmd_name == "/save") {
                            if (save_callback_) save_callback_();
                            set_status(StatusType::PROCESSING, "session saved");
                            refresh_ui(true);
                        } else if (cmd_name == "/load") {
                            if (load_callback_) load_callback_();
                            set_status(StatusType::PROCESSING, "session loaded");
                            refresh_ui(true);
                        } else if (cmd_name == "/mcp") {
                            if (mcp_callback_) append_history("", mcp_callback_(), "system");
                            set_status(StatusType::PROCESSING, "mcp status");
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
                redraw_input();
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
        
        last_ch_ = ch;
        
        // Ctrl+F: enter search mode (forward-char is handled above when in search)
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
        
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!pending_paste_.empty()) { buf.clear(); pending_paste_.clear(); pos = 0; }
            else if (pos > 0) { buf.erase(--pos, 1); }
        }
        else if (ch == KEY_LEFT) { if (pos > 0) pos--; }
        else if (ch == KEY_RIGHT) { if (pos < (int)buf.length()) pos++; }
        else if (ch == KEY_HOME) { pos = 0; }
        else if (ch == KEY_END) { pos = (int)buf.length(); }
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
        else if (ch == KEY_F(60)) {
            // Bracketed paste via define_key
            std::string paste;
            int pc;
            while ((pc = wgetch(in_win_)) != ERR && pc != KEY_F(61)) {
                if (pc == '\r' || pc == KEY_ENTER) paste += '\n';
                else if (pc >= 32 || pc == '\n' || pc == '\t') paste += (char)pc;
            }
            if (!paste.empty()) {
                int nl = 0;
                for (char cc : paste) if (cc == '\n') nl++;
                if (nl > 0 || (int)paste.size() > 300) {
                    // Large paste: store actual content, show placeholder in input
                    pending_paste_ = paste;
                    buf = fmt::format("[Pasted \xc2\xb7 {} lines \xc2\xb7 {} chars]",
                                      nl + 1, (int)paste.size());
                    pos = (int)buf.size();
                } else {
                    buf.insert(pos, paste);
                    pos += (int)paste.size();
                }
            }
        }
        else if (ch == 9) { // Tab: autocomplete slash commands and their arguments
            if (!buf.empty() && buf[0] == '/') {
                size_t sp = buf.find(' ');
                if (sp == std::string::npos) {
                    static const std::vector<std::string> cmds = {
                        "/save", "/load", "/reset", "/clear", "/toggle-reasoning",
                        "/effort", "/theme", "/web", "/auto", "/shell", "/undo", "/mcp", "/exit"
                    };
                    std::vector<std::string> matches;
                    for (const auto& cm : cmds) if (cm.rfind(buf, 0) == 0) matches.push_back(cm);
                    if (matches.size() == 1) {
                        buf = matches[0] + " ";
                        pos = (int)buf.size();
                    } else if (matches.size() > 1) {
                        std::string lcp = matches[0];
                        for (const auto& m : matches) {
                            size_t k = 0;
                            while (k < lcp.size() && k < m.size() && lcp[k] == m[k]) k++;
                            lcp = lcp.substr(0, k);
                        }
                        if (lcp.size() > buf.size()) { buf = lcp; pos = (int)buf.size(); }
                        std::string opts;
                        for (const auto& m : matches) opts += m + "  ";
                        set_status(StatusType::PROCESSING, opts);
                    }
                } else {
                    std::string cmd = buf.substr(0, sp);
                    std::string arg = buf.substr(sp + 1);
                    std::vector<std::string> opts;
                    if (cmd == "/theme") opts = {"dark", "matrix", "amber", "mono"};
                    else if (cmd == "/effort") opts = {"low", "medium", "high"};
                    else if (cmd == "/web" || cmd == "/auto" || cmd == "/shell") opts = {"on", "off"};
                    if (!opts.empty()) {
                        std::vector<std::string> matches;
                        for (const auto& o : opts) if (o.rfind(arg, 0) == 0) matches.push_back(o);
                        if (matches.size() == 1) {
                            buf = cmd + " " + matches[0];
                            pos = (int)buf.size();
                        } else if (matches.size() > 1) {
                            std::string line;
                            for (const auto& m : matches) line += m + "  ";
                            set_status(StatusType::PROCESSING, line);
                        }
                    }
                }
            }
        }
        else if (ch >= 32 && ch <= 126) {
            if (!pending_paste_.empty()) { buf.clear(); pending_paste_.clear(); pos = 0; }
            buf.insert(pos++, 1, (char)ch);
        }
        redraw_input();
    }
    std::string result = pending_paste_.empty() ? buf : pending_paste_;
    pending_paste_.clear();
    werase(in_win_);
    input_lines_ = 1;
    _setup_windows();
    return result;
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
    mousemask(mouse_enabled_ ? (BUTTON4_PRESSED | BUTTON5_PRESSED) : 0, NULL);
}

void ProTUI::_handle_mouse() {
    MEVENT ev;
    if (getmouse(&ev) == OK) {
        if (ev.bstate & BUTTON4_PRESSED) { scroll_offset_ += 3; manual_scroll_ = true; }
        if (ev.bstate & BUTTON5_PRESSED) { scroll_offset_ = std::max(0, scroll_offset_ - 3); manual_scroll_ = true; }
        int total = (int)wrapped_history_.size();
        if (scroll_offset_ <= 0 || (total > 0 && scroll_offset_ >= total - 10))
            manual_scroll_ = false;
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
        int total_r = (int)wrapped.size();
        // Clamp scroll so it never goes past the top
        reas_scroll_offset_ = std::min(reas_scroll_offset_, std::max(0, total_r - (h - 2)));
        int start = std::max(0, total_r - (h - 2) - reas_scroll_offset_);
        for (int i = 0; i < h - 2 && (start + i) < total_r; ++i) {
            mvwaddstr(reas_win_, i + 1, 2, wrapped[start + i].substr(0, w-4).c_str());
        }
        // Scroll indicator in top-right corner
        if (reas_scroll_offset_ > 0) {
            std::string ind = " +" + std::to_string(reas_scroll_offset_) + " ";
            mvwaddstr(reas_win_, 0, w - (int)ind.size() - 1, ind.c_str());
        }
        wnoutrefresh(reas_win_);
    }
    draw_box(in_win_, "Input"); wnoutrefresh(in_win_);
}

void ProTUI::update_reasoning(const std::string& text, bool append) {
    if (editor_active_.load()) return;
    std::lock_guard<std::mutex> lock(mtx_);
    std::string ctext = _sanitize_display(text);
    if (append) current_reasoning_ += ctext; else current_reasoning_ = ctext;
}
void ProTUI::clear_reasoning() { std::lock_guard<std::mutex> lock(mtx_); current_reasoning_.clear(); reas_scroll_offset_ = 0; }
void ProTUI::set_metrics(const Metrics& m) {
    std::lock_guard<std::mutex> lock(mtx_);
    // Merge-update: never clear a field that already has a value
    if (m.pp_speed.has_value())  metrics_.pp_speed  = m.pp_speed;
    if (m.gen_speed.has_value()) metrics_.gen_speed = m.gen_speed;
    if (m.ctx_size.has_value())  metrics_.ctx_size  = m.ctx_size;
    // ctx_used is sticky: only advance, never zero out
    if (m.ctx_used.has_value() && m.ctx_used.value() > 0)
        metrics_.ctx_used = m.ctx_used;
}
void ProTUI::set_activity(const std::string& a) { std::lock_guard<std::mutex> lock(mtx_); activity_ = a; }
void ProTUI::activate_last_queued() {
    std::lock_guard<std::mutex> lock(mtx_);
    // Find the most recent queued entry and convert it to a normal user entry
    for (int i = (int)raw_history_.size() - 1; i >= 0; i--) {
        if (raw_history_[i].type == "user_queued") {
            // Change sender from "queued" to "you" in the stored text
            const std::string qpfx = "queued âº ";
            const std::string ypfx = "you âº ";
            if (raw_history_[i].text.compare(0, qpfx.size(), qpfx) == 0)
                raw_history_[i].text.replace(0, qpfx.size(), ypfx);
            raw_history_[i].type  = "user";
            raw_history_[i].color = _get_color_for_type("user");
            // Rebuild wrapped_history_ to pick up the colour change
            int w = getmaxx(stdscr);
            wrapped_history_.clear();
            for (const auto& raw : raw_history_) {
                for (const auto& l : _wrap_text(raw.text, w - 2))
                    wrapped_history_.push_back({l, raw.color, raw.type});
            }
            break;
        }
    }
}
void ProTUI::set_queued(int n) { std::lock_guard<std::mutex> lock(mtx_); queued_depth_ = n > 0 ? n : 0; }
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

bool ProTUI::is_history_empty() const { 
    std::lock_guard<std::mutex> lock(mtx_); 
    return raw_history_.empty(); 
}
} // namespace egodeath
