#pragma once

#include <string>
#include <vector>
#include <functional>

namespace egodeath {

// A self-contained, full-window modal text editor. While run() is executing it
// owns the terminal exclusively (the caller suppresses background repaints), so
// it can drive its own blocking input loop. Saving is delegated to save_fn_ so
// edits flow through the agent's checkpoint/undo machinery.
class Editor {
public:
    // save_fn(path, content) -> "" on success, otherwise an error message.
    void set_save_fn(std::function<std::string(const std::string&, const std::string&)> fn) {
        save_fn_ = std::move(fn);
    }
    // Opens `path` and runs the modal editor until the user closes it.
    void run(const std::string& path);

private:
    std::vector<std::string> lines_{""};
    std::string path_, lang_, status_;
    int cy_ = 0, cx_ = 0;     // cursor (line, byte column)
    int top_ = 0, left_ = 0;  // scroll origin
    bool dirty_ = false, quit_armed_ = false;
    std::string last_search_;
    std::function<std::string(const std::string&, const std::string&)> save_fn_;

    void load();
    std::string content() const;
    bool save();
    void set_lang_from_ext();

    void clamp();
    void ensure_visible(int textrows, int textw, int numw);
    void render(void* win, int rows, int cols);
    std::vector<int> highlight(const std::string& line, bool& in_block) const;

    std::string prompt(void* win, int rows, int cols, const std::string& label);
    void find_next(void* win, int rows, int cols);
};

} // namespace egodeath
