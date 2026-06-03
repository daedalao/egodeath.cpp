#include "editor.hpp"

#include <ncurses.h>
#include <fstream>
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace egodeath {

enum { C_TEXT = 4, C_KW = 3, C_STR = 2, C_COM = 10, C_NUM = 9, C_PRE = 8, C_DIM = 10 };

static const std::unordered_set<std::string>& keywords(const std::string& lang) {
    static const std::unordered_set<std::string> cpp = {
        "alignas","alignof","auto","bool","break","case","catch","char","class","const","constexpr",
        "continue","default","delete","do","double","else","enum","explicit","extern","false","float",
        "for","friend","goto","if","inline","int","long","mutable","namespace","new","noexcept","nullptr",
        "operator","private","protected","public","return","short","signed","sizeof","static","struct",
        "switch","template","this","throw","true","try","typedef","typename","union","unsigned","using",
        "virtual","void","volatile","while","size_t","string","vector","include","define","pragma"};
    static const std::unordered_set<std::string> py = {
        "def","class","return","if","elif","else","for","while","import","from","as","try","except",
        "finally","with","lambda","yield","pass","break","continue","global","nonlocal","True","False",
        "None","and","or","not","in","is","self","raise","assert","del","async","await","print"};
    static const std::unordered_set<std::string> jsonk = {"true","false","null"};
    static const std::unordered_set<std::string> empty;
    if (lang == "cpp") return cpp;
    if (lang == "py") return py;
    if (lang == "json") return jsonk;
    return empty;
}

void Editor::set_lang_from_ext() {
    auto dot = path_.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path_.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext=="c"||ext=="cc"||ext=="cpp"||ext=="cxx"||ext=="h"||ext=="hh"||ext=="hpp"||ext=="hxx"||ext=="ipp")
        lang_ = "cpp";
    else if (ext == "py") lang_ = "py";
    else if (ext == "json") lang_ = "json";
    else lang_ = "";
}

void Editor::load() {
    lines_.clear();
    std::ifstream in(path_, std::ios::binary);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines_.push_back(line);
        }
    }
    if (lines_.empty()) lines_.push_back("");
    cy_ = cx_ = top_ = left_ = 0;
    undo_.clear();
    set_lang_from_ext();
    original_ = content();
}

std::string Editor::content() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) { out += lines_[i]; if (i + 1 < lines_.size()) out += '\n'; }
    out += '\n';
    return out;
}

bool Editor::save() {
    if (!save_fn_) { status_ = "no save handler"; return false; }
    std::string err = save_fn_(path_, content());
    if (err.empty()) { original_ = content(); status_ = "wrote " + path_; return true; }
    status_ = "save failed: " + err;
    return false;
}

void Editor::push_undo() {
    undo_.push_back({lines_, cy_, cx_});
    if (undo_.size() > 200) undo_.erase(undo_.begin());
}

void Editor::clamp() {
    if (cy_ < 0) cy_ = 0;
    if (cy_ >= (int)lines_.size()) cy_ = (int)lines_.size() - 1;
    if (cx_ < 0) cx_ = 0;
    if (cx_ > (int)lines_[cy_].size()) cx_ = (int)lines_[cy_].size();
}

void Editor::ensure_visible(int textrows, int textw, int) {
    if (cy_ < top_) top_ = cy_;
    if (cy_ >= top_ + textrows) top_ = cy_ - textrows + 1;
    if (top_ < 0) top_ = 0;
    if (cx_ < left_) left_ = cx_;
    if (cx_ >= left_ + textw) left_ = cx_ - textw + 1;
    if (left_ < 0) left_ = 0;
}

std::vector<int> Editor::highlight(const std::string& s, bool& in_block) const {
    int n = (int)s.size();
    std::vector<int> col(n, C_TEXT);
    const auto& kw = keywords(lang_);
    bool cstyle = (lang_ == "cpp");
    bool hash_comment = (lang_ == "py");
    int i = 0;
    if (cstyle) {
        int f = 0; while (f < n && (s[f] == ' ' || s[f] == '\t')) f++;
        if (f < n && s[f] == '#') { for (int k = f; k < n; ++k) col[k] = C_PRE; return col; }
    }
    while (i < n) {
        char c = s[i];
        if (in_block) {
            col[i] = C_COM;
            if (cstyle && c == '*' && i + 1 < n && s[i + 1] == '/') { col[i + 1] = C_COM; i += 2; in_block = false; continue; }
            i++; continue;
        }
        if (cstyle && c == '/' && i + 1 < n && s[i + 1] == '/') { for (int k = i; k < n; ++k) col[k] = C_COM; break; }
        if (cstyle && c == '/' && i + 1 < n && s[i + 1] == '*') { col[i] = col[i + 1] = C_COM; i += 2; in_block = true; continue; }
        if (hash_comment && c == '#') { for (int k = i; k < n; ++k) col[k] = C_COM; break; }
        if (c == '"' || c == '\'') {
            char q = c; col[i] = C_STR; int j = i + 1;
            while (j < n) { col[j] = C_STR; if (s[j] == '\\' && j + 1 < n) { col[j + 1] = C_STR; j += 2; continue; } if (s[j] == q) { j++; break; } j++; }
            i = j; continue;
        }
        if (std::isdigit((unsigned char)c) && (i == 0 || !(std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
            int j = i; while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == 'x')) { col[j] = C_NUM; j++; }
            i = j; continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            int j = i; while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            if (kw.count(s.substr(i, j - i))) for (int k = i; k < j; ++k) col[k] = C_KW;
            i = j; continue;
        }
        i++;
    }
    return col;
}

void Editor::render() {
    if (!win_) return;
    WINDOW* w = (WINDOW*)win_;
    int rows = rows_, cols = cols_;
    werase(w);
    int textrows = rows - 2;
    int numw = (int)std::to_string(std::max<size_t>(1, lines_.size())).size() + 1;
    if (numw < 4) numw = 4;
    int textw = cols - numw - 1;
    if (textw < 1) textw = 1;
    ensure_visible(textrows, textw, numw);

    std::string title = " edit: " + path_ + (dirty() ? "  [+]" : "") + "   (^W or F4: chat)";
    if ((int)title.size() > cols) title = title.substr(0, cols);
    title.resize(cols, ' ');
    wattron(w, A_REVERSE | COLOR_PAIR(C_KW));
    mvwaddstr(w, 0, 0, title.c_str());
    wattroff(w, A_REVERSE | COLOR_PAIR(C_KW));

    bool in_block = false;
    for (int fy = 0; fy < top_ && fy < (int)lines_.size(); ++fy) { auto _ = highlight(lines_[fy], in_block); (void)_; }

    for (int row = 0; row < textrows; ++row) {
        int fy = top_ + row, sy = 1 + row;
        if (fy >= (int)lines_.size()) break;
        const std::string& line = lines_[fy];
        std::string ln = std::to_string(fy + 1);
        std::string lns(numw - 1 - (int)ln.size(), ' '); lns += ln; lns += ' ';
        wattron(w, COLOR_PAIR(C_DIM));
        mvwaddstr(w, sy, 0, lns.c_str());
        wattroff(w, COLOR_PAIR(C_DIM));
        bool blk = in_block;
        std::vector<int> col = highlight(line, blk);
        int start = left_, end = std::min((int)line.size(), left_ + textw);
        int x = numw, i = start;
        while (i < end) {
            int c = col[i], j = i + 1;
            while (j < end && col[j] == c) j++;
            wattron(w, COLOR_PAIR(c));
            mvwaddnstr(w, sy, x + (i - start), line.c_str() + i, j - i);
            wattroff(w, COLOR_PAIR(c));
            i = j;
        }
        in_block = blk;
    }

    std::string modetag = mode_ == Mode::INSERT ? "-- INSERT --" : "-- NORMAL --";
    std::string left = " " + modetag + "  " + std::to_string(cy_ + 1) + ":" + std::to_string(cx_ + 1) +
                       (lang_.empty() ? "" : "  [" + lang_ + "]");
    std::string right = (status_.empty() ? "i insert  :w save  :q quit  :q! discard  ^W/F4 chat " : status_ + " ");
    std::string bar = left;
    int pad = cols - (int)left.size() - (int)right.size();
    if (pad > 0) bar += std::string(pad, ' ') + right; else bar = left.substr(0, cols);
    bar.resize(cols, ' ');
    int barcolor = mode_ == Mode::INSERT ? C_STR : C_TEXT;
    wattron(w, A_REVERSE | COLOR_PAIR(barcolor));
    mvwaddstr(w, rows - 1, 0, bar.c_str());
    wattroff(w, A_REVERSE | COLOR_PAIR(barcolor));

    int scy = 1 + (cy_ - top_), scx = numw + (cx_ - left_);
    wmove(w, scy, scx);
    wnoutrefresh(w);
    doupdate();
}

std::string Editor::prompt(const std::string& label) {
    WINDOW* w = (WINDOW*)win_;
    WINDOW* kw = (WINDOW*)(input_win_ ? input_win_ : win_);
    std::string buf;
    for (;;) {
        std::string bar = " " + label + buf;
        if ((int)bar.size() > cols_) bar = bar.substr(bar.size() - cols_);
        bar.resize(cols_, ' ');
        wattron(w, A_REVERSE | COLOR_PAIR(C_PRE));
        mvwaddstr(w, rows_ - 1, 0, bar.c_str());
        wattroff(w, A_REVERSE | COLOR_PAIR(C_PRE));
        wmove(w, rows_ - 1, std::min(cols_ - 1, (int)(1 + label.size() + buf.size())));
        wnoutrefresh(w); doupdate();
        int ch = wgetch(kw);
        if (ch == ERR) continue;
        if (ch == 27) return std::string(1, '\x1b');
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) return buf;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (buf.empty()) return std::string(1, '\x1b');
            buf.pop_back(); continue;
        }
        if (ch >= 32 && ch < 127) buf += (char)ch;
    }
}

void Editor::search() {
    std::string q = prompt("/");
    if (!q.empty() && q[0] == '\x1b') { status_.clear(); return; }
    if (q.empty()) q = last_search_;
    if (q.empty()) { status_ = "no pattern"; return; }
    last_search_ = q;
    search_dir(true);
}

void Editor::search_dir(bool forward) {
    if (last_search_.empty()) { status_ = "no pattern (use /)"; return; }
    const std::string& q = last_search_;
    int N = (int)lines_.size();
    if (forward) {
        for (int s = 0; s <= N; ++s) {
            int ly = (cy_ + s) % N;
            int from = (s == 0) ? cx_ + 1 : 0;
            auto pos = lines_[ly].find(q, std::max(0, from));
            if (pos != std::string::npos) { cy_ = ly; cx_ = (int)pos; status_ = "/" + q; return; }
        }
    } else {
        for (int s = 0; s <= N; ++s) {
            int ly = ((cy_ - s) % N + N) % N;
            const std::string& line = lines_[ly];
            int upto = (s == 0) ? cx_ - 1 : (int)line.size();
            if (upto >= 0) {
                auto pos = line.rfind(q, std::max(0, upto));
                if (pos != std::string::npos && (int)pos <= upto) { cy_ = ly; cx_ = (int)pos; status_ = "?" + q; return; }
            }
        }
    }
    status_ = "not found: " + q;
}

void Editor::word_forward() {
    const std::string& l = lines_[cy_];
    int n = (int)l.size(), i = cx_;
    auto isw = [](char c){ return std::isalnum((unsigned char)c) || c == '_'; };
    if (i < n && isw(l[i])) while (i < n && isw(l[i])) i++;
    while (i < n && !isw(l[i])) i++;
    if (i >= n && cy_ < (int)lines_.size() - 1) { cy_++; cx_ = 0; } else cx_ = i;
}

void Editor::word_back() {
    auto isw = [](char c){ return std::isalnum((unsigned char)c) || c == '_'; };
    if (cx_ == 0) { if (cy_ > 0) { cy_--; cx_ = (int)lines_[cy_].size(); } return; }
    const std::string& l = lines_[cy_];
    int i = cx_ - 1;
    while (i > 0 && !isw(l[i])) i--;
    while (i > 0 && isw(l[i - 1])) i--;
    cx_ = i;
}

void Editor::run_command(const std::string& cmd) {
    std::string c = cmd;
    while (!c.empty() && c.front() == ' ') c.erase(c.begin());
    while (!c.empty() && c.back() == ' ') c.pop_back();
    if (c == "w") { save(); return; }
    if (c.rfind("w ", 0) == 0) { path_ = c.substr(2); set_lang_from_ext(); save(); return; }
    if (c == "q") { if (dirty()) status_ = "unsaved changes — :q! or ZQ to discard, :wq/ZZ to save"; else want_close_ = true; return; }
    if (c == "q!") { want_close_ = true; return; }
    if (c == "wq" || c == "x") { if (save()) want_close_ = true; return; }
    status_ = "unknown command: :" + c;
}

void Editor::handle_insert(int ch) {
    std::string& line = lines_[cy_];
    if (ch == 27) { mode_ = Mode::NORMAL; if (cx_ > 0) cx_--; status_.clear(); return; }
    if (ch == KEY_UP)    { if (cy_ > 0) cy_--; clamp(); return; }
    if (ch == KEY_DOWN)  { if (cy_ < (int)lines_.size() - 1) cy_++; clamp(); return; }
    if (ch == KEY_LEFT)  { if (cx_ > 0) cx_--; return; }
    if (ch == KEY_RIGHT) { if (cx_ < (int)line.size()) cx_++; return; }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (cx_ > 0) { line.erase(cx_ - 1, 1); cx_--; }
        else if (cy_ > 0) { int pl = (int)lines_[cy_ - 1].size(); lines_[cy_ - 1] += line; lines_.erase(lines_.begin() + cy_); cy_--; cx_ = pl; }
        return;
    }
    if (ch == KEY_DC) {
        if (cx_ < (int)line.size()) line.erase(cx_, 1);
        else if (cy_ < (int)lines_.size() - 1) { line += lines_[cy_ + 1]; lines_.erase(lines_.begin() + cy_ + 1); }
        return;
    }
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        std::string rest = line.substr(cx_); line.erase(cx_);
        lines_.insert(lines_.begin() + cy_ + 1, rest); cy_++; cx_ = 0; return;
    }
    if (ch == '\t') { line.insert(cx_, 4, ' '); cx_ += 4; return; }
    if (ch >= 32 && ch < 127) { line.insert(cx_, 1, (char)ch); cx_++; }
}

bool Editor::handle_normal(int ch) {
    std::string& line = lines_[cy_];
    bool was_g = (pending_ == "g"), was_d = (pending_ == "d"), was_Z = (pending_ == "Z");
    pending_.clear();

    switch (ch) {
        case 'h': case KEY_LEFT:  if (cx_ > 0) cx_--; break;
        case 'l': case KEY_RIGHT: if (cx_ < (int)line.size()) cx_++; break;
        case 'j': case KEY_DOWN:  if (cy_ < (int)lines_.size() - 1) cy_++; clamp(); break;
        case 'k': case KEY_UP:    if (cy_ > 0) cy_--; clamp(); break;
        case '0': case KEY_HOME:  cx_ = 0; break;
        case '$': case KEY_END:   cx_ = std::max(0, (int)line.size() - 1); break;
        case '^': { int i = 0; while (i < (int)line.size() && (line[i]==' '||line[i]=='\t')) i++; cx_ = i; break; }
        case 'G': cy_ = (int)lines_.size() - 1; clamp(); break;
        case 'g': if (was_g) { cy_ = 0; clamp(); } else pending_ = "g"; break;
        case 'w': word_forward(); clamp(); break;
        case 'b': word_back(); clamp(); break;
        case KEY_PPAGE: cy_ -= (rows_ - 3); clamp(); break;
        case KEY_NPAGE: cy_ += (rows_ - 3); clamp(); break;

        case 'i': push_undo(); mode_ = Mode::INSERT; break;
        case 'a': push_undo(); mode_ = Mode::INSERT; if (cx_ < (int)line.size()) cx_++; break;
        case 'I': push_undo(); mode_ = Mode::INSERT; { int i=0; while(i<(int)line.size()&&(line[i]==' '||line[i]=='\t'))i++; cx_=i; } break;
        case 'A': push_undo(); mode_ = Mode::INSERT; cx_ = (int)line.size(); break;
        case 'o': push_undo(); lines_.insert(lines_.begin() + cy_ + 1, ""); cy_++; cx_ = 0; mode_ = Mode::INSERT; break;
        case 'O': push_undo(); lines_.insert(lines_.begin() + cy_, ""); cx_ = 0; mode_ = Mode::INSERT; break;

        case 'x': if (cx_ < (int)line.size()) { push_undo(); line.erase(cx_, 1); clamp(); } break;
        case 'D': if (cx_ < (int)line.size()) { push_undo(); line.erase(cx_); clamp(); } break;
        case 'd':
            if (was_d) {
                push_undo();
                if (lines_.size() == 1) lines_[0].clear();
                else { lines_.erase(lines_.begin() + cy_); if (cy_ >= (int)lines_.size()) cy_ = (int)lines_.size() - 1; }
                cx_ = 0;
            } else pending_ = "d";
            break;

        case 'Z': if (was_Z) { if (save()) want_close_ = true; } else pending_ = "Z"; break;
        case 'Q': if (was_Z) want_close_ = true; break;
        case 'u': if (!undo_.empty()) { auto s = undo_.back(); undo_.pop_back(); lines_ = s.lines; cy_ = s.cy; cx_ = s.cx; clamp(); status_ = "undo"; } else status_ = "nothing to undo"; break;

        case '/': search(); break;
        case 'n': search_dir(true); break;
        case 'N': search_dir(false); break;

        case ':': { std::string cmd = prompt(":"); if (!cmd.empty() && cmd[0] != '\x1b') run_command(cmd); break; }
        case 27: status_.clear(); break;
        default: break;
    }
    return want_close_;
}

int Editor::read_key() {
    if (!input_win_) return ERR;
    return wgetch((WINDOW*)input_win_);
}

bool Editor::handle_key(int ch) {
    if (ch == ERR) return false;
    if (mode_ == Mode::INSERT) { handle_insert(ch); return false; }
    return handle_normal(ch);
}

void Editor::open(const std::string& path, int x, int y, int w, int h, void* input_win) {
    if (open_) close();
    path_ = path;
    load();
    mode_ = Mode::NORMAL;
    status_.clear();
    want_close_ = false;
    rows_ = h; cols_ = w;
    set_escdelay(25);
    win_ = newwin(h, w, y, x);
    keypad((WINDOW*)win_, TRUE);  // keep the terminal in keypad mode across this window's refreshes
    input_win_ = input_win;       // but read keys from the host window (its keypad is known-good)
    open_ = true;
    curs_set(1);
}

void Editor::close() {
    if (win_) {
        WINDOW* w = (WINDOW*)win_;
        werase(w); wnoutrefresh(w); doupdate();
        delwin(w);
        win_ = nullptr;
    }
    open_ = false;
    curs_set(0);
}

} // namespace egodeath
