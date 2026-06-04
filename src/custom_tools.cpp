#include "custom_tools.hpp"
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

bool CustomTools::is_reserved(const std::string& name) {
    static const std::vector<std::string> reserved = {
        "read_file", "write_file", "edit_file", "list_directory", "search", "exec_shell",
        "save_memory", "recall_memory", "remove_memory", "categorize_memory",
        "add_task", "add_event", "list_items", "complete_item", "remove_item",
        "ask_user", "open_editor", "get_settings", "set_setting",
        "web_search", "web_fetch", "define_tool", "list_tools", "remove_tool",
    };
    return std::find(reserved.begin(), reserved.end(), name) != reserved.end();
}

void CustomTools::load(fs::path file, std::string project_key) {
    std::lock_guard<std::mutex> lk(mtx_);
    file_ = std::move(file);
    project_key_ = project_key.empty() ? "global" : std::move(project_key);
    defs_.clear();
    std::ifstream in(file_);
    if (!in.good()) return;
    try {
        json j; in >> j;
        if (j.is_array())
            for (auto& e : j) defs_.push_back(def_from_json(e));
    } catch (...) { /* corrupt registry: start empty */ }
}

bool CustomTools::in_scope(const Def& d) const {
    return d.scope == "global" || d.scope == project_key_;
}

CustomTools::Def* CustomTools::find_locked(const std::string& name) {
    for (auto& d : defs_) if (d.name == name && in_scope(d)) return &d;
    return nullptr;
}
const CustomTools::Def* CustomTools::find_locked(const std::string& name) const {
    for (auto& d : defs_) if (d.name == name && in_scope(d)) return &d;
    return nullptr;
}

bool CustomTools::has(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return find_locked(name) != nullptr;
}

bool CustomTools::is_template(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto* d = find_locked(name);
    return d && d->kind == "template";
}

std::optional<CustomTools::Def> CustomTools::get(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto* d = find_locked(name);
    if (!d) return std::nullopt;
    return *d;
}

std::vector<json> CustomTools::schemas() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<json> out;
    for (const auto& d : defs_) {
        if (!in_scope(d)) continue;
        json props = json::object();
        json required = json::array();
        for (const auto& p : d.params) {
            props[p.name] = {{"type", "string"},
                             {"description", p.description.empty() ? "parameter" : p.description}};
            if (p.required) required.push_back(p.name);
        }
        json fn = {
            {"name", d.name},
            {"description", "[custom " + d.kind + "] " + d.description},
            {"parameters", {{"type", "object"}, {"properties", props}, {"required", required}}},
        };
        out.push_back({{"type", "function"}, {"function", fn}});
    }
    return out;
}

std::string CustomTools::define(const Def& in, std::string& err) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (in.name.empty()) { err = "name is required"; return ""; }
    if (is_reserved(in.name)) { err = "'" + in.name + "' is a built-in tool name"; return ""; }
    if (in.kind != "template" && in.kind != "recipe") { err = "kind must be 'template' or 'recipe'"; return ""; }
    if (in.kind == "template" && in.command.empty()) { err = "template tools need a 'command'"; return ""; }
    if (in.kind == "recipe" && (!in.steps.is_array() || in.steps.empty())) { err = "recipe tools need non-empty 'steps'"; return ""; }

    Def d = in;
    if (d.scope.empty()) d.scope = "global";

    bool replaced = false;
    for (auto& e : defs_) {
        if (e.name == d.name && e.scope == d.scope) { e = d; replaced = true; break; }
    }
    if (!replaced) defs_.push_back(d);
    save_locked();
    return std::string(replaced ? "updated" : "defined") + " custom " + d.kind + " tool '" + d.name +
           "' (" + std::to_string(d.params.size()) + " params, scope " + d.scope + ")";
}

bool CustomTools::remove(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t before = defs_.size();
    defs_.erase(std::remove_if(defs_.begin(), defs_.end(),
                   [&](const Def& d) { return d.name == name && in_scope(d); }),
                defs_.end());
    if (defs_.size() == before) return false;
    save_locked();
    return true;
}

void CustomTools::bump_use(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (auto* d = find_locked(name)) { d->uses++; save_locked(); }
}

json CustomTools::list_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    json arr = json::array();
    for (const auto& d : defs_) {
        json j = json::object();
        j["name"] = d.name;
        j["kind"] = d.kind;
        j["description"] = d.description;
        j["scope"] = d.scope;
        j["uses"] = d.uses;
        j["in_scope"] = in_scope(d);
        arr.push_back(j);
    }
    return arr;
}

json CustomTools::def_to_json(const Def& d) {
    json params = json::array();
    for (const auto& p : d.params)
        params.push_back({{"name", p.name}, {"description", p.description}, {"required", p.required}});
    return {
        {"name", d.name}, {"description", d.description}, {"kind", d.kind},
        {"command", d.command}, {"steps", d.steps}, {"params", params},
        {"scope", d.scope}, {"uses", d.uses},
    };
}

CustomTools::Def CustomTools::def_from_json(const json& j) {
    Def d;
    d.name = j.value("name", "");
    d.description = j.value("description", "");
    d.kind = j.value("kind", "template");
    d.command = j.value("command", "");
    if (j.contains("steps") && j["steps"].is_array()) d.steps = j["steps"];
    d.scope = j.value("scope", "global");
    d.uses = j.value("uses", (long long)0);
    if (j.contains("params") && j["params"].is_array())
        for (const auto& p : j["params"]) {
            Param pp;
            pp.name = p.value("name", "");
            pp.description = p.value("description", "");
            pp.required = p.value("required", false);
            if (!pp.name.empty()) d.params.push_back(pp);
        }
    return d;
}

void CustomTools::save_locked() const {
    if (file_.empty()) return;
    json arr = json::array();
    for (const auto& d : defs_) arr.push_back(def_to_json(d));
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    fs::path tmp = file_; tmp += ".tmp";
    { std::ofstream out(tmp, std::ios::trunc); if (!out.good()) return; out << arr.dump(2); }
    fs::rename(tmp, file_, ec);
}
