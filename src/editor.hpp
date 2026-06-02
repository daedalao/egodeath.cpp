#pragma once

#include <string>
#include <vector>
#include <functional>

namespace egodeath {

// A self-contained, full-window modal text editor with vim-style keybindings.
// While run() is executing it owns the terminal exclusively (the caller
// suppresses background repaints), so it can drive its own blocking input loop.
// Saving is delegated to save_fn_ so edits flow through the agent's
// checkpoint/undo machinery.
class Editor {
public:
    // save_fn(path, content) -> "" on success, otherwise an error message.
    void set_save_fn(std::function<std::string(const std::string&, const std::string&)> fn) {
        save_fn_ = std::move(fn);
    }
    // Opens `path` and runs the modal editor in the rectangle (x,y,w,h) until the
    // user closes it (:q / :q!).
    void run(const std::string& path, int x, int y, int w, int h);

private:
    enum class Mode { NORMAL, INSERT };

    std::vector<std::string> lines_{""};
    std::string path_, lang_, status_;
    Mode mode_ = Mode::NORMAL;
    int cy_ = 0, cx_ = 0;     // cursor (line, byte column)
    int top_ = 0, left_ = 0;  // scroll origin
    bool dirty_ = false;
    std::string pending_;     // pending normal-mode prefix (e.g. "d", "g")
    std::string last_search_;

    struct Snapshot { std::vector<std::string> lines; int cy, cx; };
    std::vector<Snapshot> undo_;

    std::function<std::string(const std::string&, const std::string&)> save_fn_;

    void load();
    std::string content() const;
    bool save();
    void set_lang_from_ext();

    void push_undo();
    void clamp();
    void ensure_visible(int textrows, int textw, int numw);
    void render(void* win, int rows, int cols);
    std::vector<int> highlight(const std::string& line, bool& in_block) const;

    // input handlers; return false to request closing the editor
    bool handle_normal(void* win, int rows, int cols, int ch, bool& open);
    void handle_insert(int ch);

    std::string prompt(void* win, int rows, int cols, const std::string& label);
    bool run_command(const std::string& cmd, bool& open);  // ':' commands
    void search(void* win, int rows, int cols);            // '/' prompt
    void search_dir(bool forward);                         // n / N
    void word_forward();
    void word_back();
};

} // namespace egodeath
