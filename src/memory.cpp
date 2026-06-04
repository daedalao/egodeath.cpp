#include "memory.hpp"
#include <fstream>
#include <regex>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <map>

namespace fs = std::filesystem;

static long long now_ts() { return (long long)std::time(nullptr); }

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

Memory::Memory(fs::path global_root, fs::path project_root, std::string project_name)
    : global_root(global_root), project_name(project_name) {
    if (!project_root.empty()) this->project_root = project_root;

    std::lock_guard<std::mutex> lock(file_mutex_);
    fs::create_directories(global_root);
    if (this->project_root) fs::create_directories(*this->project_root);
}

void Memory::atomic_write_file(const fs::path& path, const std::string& content) {
    fs::path tmp = path;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.good()) { std::cerr << "Failed to open temp file: " << tmp << std::endl; return; }
    out << content;
    out.close();
    if (!out.good()) { std::cerr << "Failed to write temp file: " << tmp << std::endl; fs::remove(tmp); return; }
    try { fs::rename(tmp, path); }
    catch (const std::exception& e) { std::cerr << "Atomic rename failed: " << e.what() << std::endl; fs::remove(tmp); }
}

bool Memory::parse_file(const fs::path& path, const std::string& scope, Entry& out) {
    std::ifstream in(path);
    if (!in.good()) return false;

    out = Entry{};
    out.scope = scope;
    out.path = path.string();
    out.name = path.stem().string();

    std::map<std::string, std::string> fm;
    std::string line;
    bool in_fm = false, fm_done = false;
    std::string body;
    while (std::getline(in, line)) {
        if (!fm_done && line == "---") {
            if (!in_fm) { in_fm = true; continue; }
            in_fm = false; fm_done = true; continue;
        }
        if (in_fm) {
            auto c = line.find(':');
            if (c != std::string::npos) fm[trim(line.substr(0, c))] = trim(line.substr(c + 1));
        } else if (fm_done) {
            body += line + "\n";
        }
    }
    out.body = trim(body);

    // category, with back-compat fallback to the old `type` field.
    out.category = fm.count("category") ? fm["category"] : (fm.count("type") ? fm["type"] : "unknown");
    if (out.category.empty()) out.category = "unknown";
    if (fm.count("description")) out.description = fm["description"];
    out.pinned = fm.count("pinned") && (fm["pinned"] == "true" || fm["pinned"] == "1");

    auto as_ll = [&](const std::string& k) -> long long {
        if (!fm.count(k) || fm[k].empty()) return 0;
        try { return std::stoll(fm[k]); } catch (...) { return 0; }
    };
    out.created = as_ll("created");
    out.last_used = as_ll("last_used");
    out.uses = as_ll("uses");

    // Backfill timestamps for legacy files so sorting/eviction behave.
    if (out.created == 0) {
        std::error_code ec;
        auto t = fs::last_write_time(path, ec);
        out.created = ec ? now_ts()
            : (long long)std::chrono::duration_cast<std::chrono::seconds>(
                  t.time_since_epoch()).count();
    }
    if (out.last_used == 0) out.last_used = out.created;
    return true;
}

void Memory::write_entry(const fs::path& path, const Entry& e) {
    std::string c;
    c += "---\n";
    c += "name: " + e.name + "\n";
    c += "category: " + e.category + "\n";
    c += "description: " + e.description + "\n";
    c += "pinned: " + std::string(e.pinned ? "true" : "false") + "\n";
    c += "created: " + std::to_string(e.created) + "\n";
    c += "last_used: " + std::to_string(e.last_used) + "\n";
    c += "uses: " + std::to_string(e.uses) + "\n";
    c += "---\n\n";
    c += e.body + "\n";
    atomic_write_file(path, c);
}

std::optional<fs::path> Memory::locate(const std::string& name, std::string* scope) {
    for (const auto& rs : roots_with_labels()) {
        fs::path p = rs.root / (name + ".md");
        if (fs::exists(p)) { if (scope) *scope = rs.scope; return p; }
    }
    return std::nullopt;
}

void Memory::save(const std::string& name, const std::string& description, const std::string& fact,
                  const std::string& category, const std::string& scope, bool pinned) {
    std::lock_guard<std::mutex> lock(file_mutex_);

    fs::path target = (scope == "project" && project_root) ? *project_root : global_root;
    fs::path file = target / (name + ".md");

    Entry e;
    // Preserve counters if overwriting an existing memory.
    Entry existing;
    if (fs::exists(file) && parse_file(file, scope, existing)) {
        e.created = existing.created;
        e.uses = existing.uses;
        e.last_used = existing.last_used;
    } else {
        e.created = now_ts();
        e.last_used = now_ts();
    }
    e.name = name;
    e.description = description;
    e.body = trim(fact);
    e.category = category.empty() ? "unknown" : category;
    e.pinned = pinned;
    write_entry(file, e);

    enforce_limit(target);
}

void Memory::enforce_limit(const fs::path& root) {
    std::vector<Entry> entries;
    std::error_code ec;
    if (!fs::exists(root)) return;
    for (const auto& p : fs::directory_iterator(root, ec)) {
        if (p.path().extension() != ".md") continue;
        Entry e;
        if (parse_file(p.path(), "", e)) entries.push_back(e);
    }
    std::vector<Entry*> evictable;
    for (auto& e : entries) if (!e.pinned) evictable.push_back(&e);
    if ((int)evictable.size() <= kScopeCap) return;

    // Least valuable first: fewest uses, then oldest last_used.
    std::sort(evictable.begin(), evictable.end(), [](const Entry* a, const Entry* b) {
        if (a->uses != b->uses) return a->uses < b->uses;
        return a->last_used < b->last_used;
    });
    int to_remove = (int)evictable.size() - kScopeCap;
    for (int i = 0; i < to_remove; ++i) {
        fs::remove(evictable[i]->path, ec);
    }
}

void Memory::bump_use(const fs::path& path) {
    Entry e;
    if (!parse_file(path, "", e)) return;
    e.uses += 1;
    e.last_used = now_ts();
    // parse_file cleared scope; preserve the name/fields, scope is irrelevant to disk.
    write_entry(path, e);
}

std::vector<Memory::Entry> Memory::collect_all() {
    std::vector<Entry> out;
    for (const auto& rs : roots_with_labels()) {
        if (!fs::exists(rs.root)) continue;
        std::error_code ec;
        for (const auto& p : fs::directory_iterator(rs.root, ec)) {
            if (p.path().extension() != ".md") continue;
            Entry e;
            if (parse_file(p.path(), rs.scope, e)) out.push_back(e);
        }
    }
    return out;
}

std::vector<json> Memory::list_entries() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    std::vector<json> entries;
    for (auto& e : collect_all()) {
        json j = json::object();
        j["name"] = e.name;
        j["scope"] = e.scope;
        j["category"] = e.category;
        j["description"] = e.description;
        j["pinned"] = e.pinned;
        j["uses"] = e.uses;
        j["last_used"] = e.last_used;
        j["path"] = e.path;
        entries.push_back(j);
    }
    return entries;
}

bool Memory::set_category(const std::string& name, const std::string& category) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    std::string scope;
    auto p = locate(name, &scope);
    if (!p) return false;
    Entry e;
    if (!parse_file(*p, scope, e)) return false;
    e.category = category.empty() ? "unknown" : category;
    write_entry(*p, e);
    return true;
}

bool Memory::set_pinned(const std::string& name, bool pinned) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    std::string scope;
    auto p = locate(name, &scope);
    if (!p) return false;
    Entry e;
    if (!parse_file(*p, scope, e)) return false;
    e.pinned = pinned;
    write_entry(*p, e);
    return true;
}

std::string Memory::recall(const std::string& query, const std::string& scope, size_t max_chars) {
    std::lock_guard<std::mutex> lock(file_mutex_);

    std::string q = lower(query);
    std::string out;
    size_t current = 0;

    for (auto& e : collect_all()) {
        if (scope != "all" && e.scope != scope) continue;
        if (!q.empty()) {
            if (lower(e.name).find(q) == std::string::npos &&
                lower(e.description).find(q) == std::string::npos &&
                lower(e.category).find(q) == std::string::npos &&
                lower(e.body).find(q) == std::string::npos)
                continue;
        }
        if (current + e.body.size() > max_chars) break;

        out += "---\n";
        out += "Name: " + e.name + "\n";
        out += "Scope: " + e.scope + "\n";
        out += "Category: " + e.category + "\n";
        out += "---\n\n";
        out += e.body + "\n---\n";
        current += e.body.size();

        // Only count an explicit (keyworded) recall as a use, so the every-turn
        // digest pass below never inflates the counters.
        if (!q.empty()) bump_use(e.path);
    }
    return out;
}

bool Memory::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    bool deleted = false;
    for (const auto& rs : roots_with_labels()) {
        fs::path p = rs.root / (name + ".md");
        if (fs::exists(p)) {
            try { fs::remove(p); deleted = true; }
            catch (const fs::filesystem_error& e) { std::cerr << "remove failed " << p << ": " << e.what() << std::endl; }
        }
    }
    return deleted;
}

std::string Memory::system_prompt_addendum() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    auto entries = collect_all();
    if (entries.empty()) return "";

    // Most relevant first: pinned, then recently used, then most used.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.pinned != b.pinned) return a.pinned;
        if (a.last_used != b.last_used) return a.last_used > b.last_used;
        return a.uses > b.uses;
    });

    // Group into categories, preserving the relevance order within each.
    std::vector<std::string> cat_order;
    std::map<std::string, std::vector<const Entry*>> by_cat;
    for (const auto& e : entries) {
        if (!by_cat.count(e.category)) cat_order.push_back(e.category);
        by_cat[e.category].push_back(&e);
    }

    std::string out = "\n\n# Persistent Memory (index)\n";
    out += "Below is a categorized index of what you remember. Each line is name — description. "
           "Use recall_memory(name) to read the full content of any entry on demand.\n";
    size_t used = out.size();
    bool truncated = false;
    for (const auto& cat : cat_order) {
        std::string header = "\n## " + cat + "\n";
        if (used + header.size() > kDigestChars) { truncated = true; break; }
        out += header; used += header.size();
        for (const auto* e : by_cat[cat]) {
            std::string pin = e->pinned ? "📌 " : "";
            std::string tag = e->scope == "project" ? " ·proj" : "";
            std::string ln = "- " + pin + e->name + tag +
                             (e->description.empty() ? "" : " — " + e->description) + "\n";
            if (used + ln.size() > kDigestChars) { truncated = true; break; }
            out += ln; used += ln.size();
        }
        if (truncated) break;
    }
    if (truncated) out += "… (index truncated; recall_memory with keywords to search the rest)\n";
    return out;
}

std::vector<Memory::RootScope> Memory::roots_with_labels(bool project_first) {
    std::vector<RootScope> res;
    if (project_first && project_root) res.push_back({*project_root, "project"});
    res.push_back({global_root, "global"});
    if (!project_first && project_root) res.push_back({*project_root, "project"});
    return res;
}
