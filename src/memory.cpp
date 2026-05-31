#include "memory.hpp"
#include <fstream>
#include <regex>
#include <iostream>

namespace fs = std::filesystem;

Memory::Memory(fs::path global_root, fs::path project_root, std::string project_name)
    : global_root(global_root), project_name(project_name) {
    if (!project_root.empty()) this->project_root = project_root;
    fs::create_directories(global_root);
    if (this->project_root) fs::create_directories(*this->project_root);
}

void Memory::save(const std::string& name, const std::string& description, const std::string& fact, const std::string& type, const std::string& scope) {
    fs::path target = (scope == "project" && project_root) ? *project_root : global_root;
    fs::path file = target / (name + ".md");
    
    std::ofstream out(file);
    out << "---\n";
    out << "name: " << name << "\n";
    out << "type: " << type << "\n";
    out << "description: " << description << "\n";
    out << "---\n\n";
    out << fact << "\n";
}

std::vector<json> Memory::list_entries() {
    std::vector<json> entries;
    for (const auto& rs : roots_with_labels()) {
        if (!fs::exists(rs.root)) continue;
        for (const auto& p : fs::directory_iterator(rs.root)) {
            if (p.path().extension() == ".md") {
                json entry = json::object();
                entry["name"] = p.path().stem().string();
                entry["scope"] = rs.scope;
                entry["path"] = p.path().string();
                entries.push_back(entry);
            }
        }
    }
    return entries;
}

std::string Memory::recall(const std::string& query, const std::string& scope) {
    std::string out;
    for (const auto& rs : roots_with_labels()) {
        if (scope != "all" && rs.scope != scope) continue;
        if (!fs::exists(rs.root)) continue;
        for (const auto& p : fs::directory_iterator(rs.root)) {
            if (p.path().extension() == ".md") {
                std::ifstream in(p.path());
                std::string line;
                while (std::getline(in, line)) out += line + "\n";
                out += "\n---\n";
            }
        }
    }
    return out;
}

bool Memory::remove(const std::string& name) {
    bool deleted = false;
    for (const auto& rs : roots_with_labels()) {
        fs::path p = rs.root / (name + ".md");
        if (fs::exists(p)) {
            fs::remove(p);
            deleted = true;
        }
    }
    return deleted;
}

std::string Memory::system_prompt_addendum() {
    std::string mem = recall("", "all");
    if (mem.empty()) return "";
    return "\n\n# Persistent Memories\n" + mem;
}

std::vector<Memory::RootScope> Memory::roots_with_labels(bool project_first) {
    std::vector<RootScope> res;
    if (project_first && project_root) res.push_back({*project_root, "project"});
    res.push_back({global_root, "global"});
    if (!project_first && project_root) res.push_back({*project_root, "project"});
    return res;
}
