#include "soul.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <set>

namespace fs = std::filesystem;

static std::string trim_copy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::string Soul::derive_short(const std::string& full) {
    std::vector<std::string> toks;
    std::stringstream ss(full);
    std::string t;
    while (ss >> t) toks.push_back(t);
    if (toks.empty()) return trim_copy(full);

    static const std::set<std::string> honor = {
        "sir","dame","dr","dr.","mr","mr.","mrs","mrs.","ms","ms.","miss","mx",
        "lord","lady","prof","prof.","professor","captain","capt","capt.","cmdr",
        "rev","rev.","st","st.","saint","master","mistress","madam","madame",
        "general","gen","gen.","col","col.","colonel","major","maj","lt","lt.",
        "lieutenant","sergeant","sgt","sgt.","king","queen","prince","princess",
        "duke","duchess","earl","count","countess","baron","baroness","hon",
    };
    static const std::set<std::string> stop_words = {
        "the","of","von","van","de","la","del","di","da",
        "first","second","third","fourth","fifth","sixth","seventh","eighth",
        "ninth","tenth","eleventh","twelfth","thirteenth","fourteenth","fifteenth",
        "sixteenth","seventeenth","eighteenth","nineteenth","twentieth",
        "jr","jr.","sr","sr.","junior","senior","esquire","esq","esq.","phd","md",
    };

    size_t i = 0;
    while (i < toks.size() && honor.count(lower_copy(toks[i]))) i++;
    for (; i < toks.size(); ++i) {
        std::string l = lower_copy(toks[i]);
        if (stop_words.count(l)) break;  // ordinals / connectors end the given name
        bool roman = !l.empty() && l.find_first_not_of("ivxlcdm") == std::string::npos;
        bool num   = !l.empty() && l.find_first_not_of("0123456789") == std::string::npos;
        if (roman || num) continue;
        std::string cand = toks[i];
        return cand.size() > 24 ? cand.substr(0, 24) : cand;
    }
    std::string cand = toks[0];
    return cand.size() > 24 ? cand.substr(0, 24) : cand;
}

bool Soul::is_core(const std::string& n) const {
    std::string ln = lower_copy(n);
    for (const auto& c : core_) if (lower_copy(c) == ln) return true;
    return false;
}

std::string Soul::subst(std::string s) const {
    auto rep = [&](const std::string& ph, const std::string& val) {
        size_t pos = 0;
        while ((pos = s.find(ph, pos)) != std::string::npos) { s.replace(pos, ph.size(), val); pos += val.size(); }
    };
    rep("{fullname}", full_name_.empty() ? name_ : full_name_);
    rep("{name}", name_);
    return s;
}

void Soul::set_raw(const std::string& body) {
    raw_body_ = body;
    preamble_.clear();
    sections_.clear();
    std::istringstream in(body);
    std::string line;
    Section* cur = nullptr;
    std::string pre;
    while (std::getline(in, line)) {
        if (line.rfind("## ", 0) == 0) {
            sections_.push_back(Section{trim_copy(line.substr(3)), ""});
            cur = &sections_.back();
        } else if (cur) {
            cur->body += line + "\n";
        } else {
            pre += line + "\n";
        }
    }
    preamble_ = trim_copy(pre);
    for (auto& s : sections_) s.body = trim_copy(s.body);
}

void Soul::parse(const std::string& content) {
    std::istringstream in(content);
    std::string line;
    std::string body;
    if (std::getline(in, line) && trim_copy(line) == "---") {
        while (std::getline(in, line)) {
            std::string tl = trim_copy(line);
            if (tl == "---") break;
            auto c = line.find(':');
            if (c == std::string::npos) continue;
            std::string key = trim_copy(line.substr(0, c));
            std::string val = trim_copy(line.substr(c + 1));
            if (key == "name") name_ = val;
            else if (key == "fullname") full_name_ = val;
            else if (key == "core") {
                core_.clear();
                std::stringstream ss(val); std::string item;
                while (std::getline(ss, item, ',')) { item = trim_copy(item); if (!item.empty()) core_.push_back(item); }
                if (core_.empty()) core_.push_back("identity");
            }
        }
        std::stringstream rest; rest << in.rdbuf(); body = rest.str();
    } else {
        body = line + "\n";
        std::stringstream rest; rest << in.rdbuf(); body += rest.str();
    }
    if (name_.empty()) name_ = "egodeath";
    set_raw(trim_copy(body));
}

bool Soul::load(fs::path file, const std::string& default_name, const std::string& default_body) {
    std::lock_guard<std::mutex> lk(mtx_);
    file_ = std::move(file);
    std::ifstream in(file_);
    if (in.good()) {
        std::stringstream ss; ss << in.rdbuf();
        parse(ss.str());
        return false;
    }
    name_ = default_name.empty() ? "egodeath" : default_name;
    full_name_.clear();
    core_ = {"identity"};
    set_raw(trim_copy(default_body));
    save_locked();
    return true;
}

std::string Soul::name() const { std::lock_guard<std::mutex> lk(mtx_); return name_; }
std::string Soul::full_name() const { std::lock_guard<std::mutex> lk(mtx_); return full_name_.empty() ? name_ : full_name_; }
std::string Soul::body() const { std::lock_guard<std::mutex> lk(mtx_); return raw_body_; }

std::string Soul::prompt() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string out;
    // Surface the formal name when it differs from the everyday handle, so the
    // model knows both even if the body only references {name}.
    if (!full_name_.empty() && full_name_ != name_)
        out += "(Your full, formal name is " + full_name_ + "; you go by " + name_ + ".)\n\n";
    if (!preamble_.empty()) out += preamble_ + "\n\n";
    for (const auto& s : sections_)
        if (is_core(s.name)) out += "## " + s.name + "\n" + s.body + "\n\n";

    std::vector<std::string> idx;
    for (const auto& s : sections_) if (!is_core(s.name)) idx.push_back(s.name);
    if (!idx.empty()) {
        out += "Soul sections \xe2\x80\x94 call recall_soul <name> to read any in full:\n";
        for (size_t i = 0; i < idx.size(); ++i) out += (i ? " \xc2\xb7 " : "") + idx[i];
        out += "\n";
    }
    return subst(trim_copy(out));
}

std::string Soul::section(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string ln = lower_copy(trim_copy(name));
    for (const auto& s : sections_) if (lower_copy(s.name) == ln) return subst(s.body);
    return "";
}

std::vector<std::string> Soul::section_names() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    for (const auto& s : sections_) out.push_back(s.name);
    return out;
}

void Soul::set_name(const std::string& n) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string t = trim_copy(n);
    if (t.empty()) return;
    name_ = t;
    save_locked();
}

void Soul::set_full_name(const std::string& f) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string t = trim_copy(f);
    full_name_ = (t == name_) ? "" : t;  // store only when it differs
    save_locked();
}

void Soul::set_body(const std::string& b) {
    std::lock_guard<std::mutex> lk(mtx_);
    set_raw(trim_copy(b));
    save_locked();
}

void Soul::reload() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::ifstream in(file_);
    if (!in.good()) return;
    std::stringstream ss; ss << in.rdbuf();
    parse(ss.str());
}

fs::path Soul::path() const { std::lock_guard<std::mutex> lk(mtx_); return file_; }

void Soul::save_locked() const {
    if (file_.empty()) return;
    std::string core_csv;
    for (size_t i = 0; i < core_.size(); ++i) core_csv += (i ? ", " : "") + core_[i];
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    fs::path tmp = file_; tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.good()) return;
        out << "---\nname: " << name_ << "\n";
        if (!full_name_.empty()) out << "fullname: " << full_name_ << "\n";
        out << "core: " << core_csv << "\n---\n\n" << raw_body_ << "\n";
    }
    fs::rename(tmp, file_, ec);
}
