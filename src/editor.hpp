#pragma once

#include <string>
#include <vector>
#include <functional>

namespace egodeath {

// A non-modal, focusable text editor with vim-style keybindings. It owns its own
// ncurses window for its docked region. The host (ProTUI) drives it one key at a
// time via handle_key(), renders with render(), and reads keys with read_key(),
// so focus can move between the editor and the chat without closing the file.
class Editor {
public:
    void set_save_fn(std::function<std::string(const std::string&, const std::string&)> fn) {
        save_fn_ = std::move(fn);
    }
    void open(const std::string& path, int x, int y, int w, int h, void* input_win);
    bool is_open() const { return open_; }
    void render();
    int read_key();            // blocks up to ~200ms; returns ERR on timeout
    bool handle_key(int ch);   // returns true when the editor should close
    void close();
    const std::string& path() const { return path_; }

private:
    enum class Mode { NORMAL, INSERT };

    void* win_ = nullptr;        // WINDOW* for drawing the pane
    void* input_win_ = nullptr;  // WINDOW* to read keys from (host's, has working keypad)
    int rows_ = 0, cols_ = 0;
    bool open_ = false, want_close_ = false;

    std::vector<std::string> lines_{""};
    std::string path_, lang_, status_, original_;
    Mode mode_ = Mode::NORMAL;
    int cy_ = 0, cx_ = 0;
    int top_ = 0, left_ = 0;
    std::string pending_;
    std::string last_search_;

    struct Snapshot { std::vector<std::string> lines; int cy, cx; };
    std::vector<Snapshot> undo_;

    std::function<std::string(const std::string&, const std::string&)> save_fn_;

    void load();
    std::string content() const;
    bool dirty() const { return content() != original_; }
    bool save();
    void set_lang_from_ext();
    void push_undo();
    void clamp();
    void ensure_visible(int textrows, int textw, int numw);
    std::vector<int> highlight(const std::string& line, bool& in_block) const;
    std::string prompt(const std::string& label);
    void run_command(const std::string& cmd);
    void search();
    void search_dir(bool forward);
    void word_forward();
    void word_back();
    void handle_insert(int ch);
    bool handle_normal(int ch);  // returns true if a quit was requested
};

} // namespace egodeath
