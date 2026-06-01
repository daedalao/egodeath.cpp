#include "memory.hpp"
#include <fstream>
#include <regex>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

Memory::Memory(fs::path global_root, fs::path project_root, std::string project_name)
    : global_root(global_root), project_name(project_name) {
    if (!project_root.empty()) this->project_root = project_root;
    
    // Lock during initialization to avoid race with concurrent construction
    std::lock_guard<std::mutex> lock(file_mutex_);
    fs::create_directories(global_root);
    if (this->project_root) fs::create_directories(*this->project_root);
}

void Memory::save(const std::string& name, const std::string& description, const std::string& fact, const std::string& type, const std::string& scope) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    fs::path target = (scope == "project" && project_root) ? *project_root : global_root;
    fs::path file = target / (name + ".md");
    
    std::string content;
    content += "---\n";
    content += "name: " + name + "\n";
    content += "type: " + type + "\n";
    content += "description: " + description + "\n";
    content += "---\n\n";
    content += fact + "\n";
    
    atomic_write_file(file, content);
}

void Memory::atomic_write_file(const std::filesystem::path& path, const std::string& content) {
    fs::path tmp = path;
    tmp += ".tmp";
    
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.good()) {
        std::cerr << "Failed to open temp file: " << tmp << std::endl;
        return;
    }
    
    out << content;
    out.close();
    
    if (!out.good()) {
        std::cerr << "Failed to write to temp file: " << tmp << std::endl;
        fs::remove(tmp); // Clean up on failure
        return;
    }
    
    // Atomic rename
    try {
        fs::rename(tmp, path);
    } catch (const std::exception& e) {
        std::cerr << "Atomic rename failed: " << e.what() << std::endl;
        fs::remove(tmp); // Clean up on failure
    }
}

std::string Memory::read_file_content(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.good()) return "";
    
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<json> Memory::list_entries() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    std::vector<json> entries;
    for (const auto& rs : roots_with_labels()) {
        if (!fs::exists(rs.root)) continue;
        try {
            for (const auto& p : fs::directory_iterator(rs.root)) {
                if (p.path().extension() == ".md") {
                    json entry = json::object();
                    entry["name"] = p.path().stem().string();
                    entry["scope"] = rs.scope;
                    entry["path"] = p.path().string();
                    entries.push_back(entry);
                }
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Directory iteration error: " << e.what() << std::endl;
        }
    }
    return entries;
}

bool Memory::matches_query(const json& metadata, const std::string& query) {
    if (query.empty()) return true;
    
    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
    
    // Check name and description fields
    if (metadata.contains("name")) {
        std::string name = metadata["name"].get<std::string>();
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name.find(query_lower) != std::string::npos) return true;
    }
    
    if (metadata.contains("description")) {
        std::string desc = metadata["description"].get<std::string>();
        std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
        if (desc.find(query_lower) != std::string::npos) return true;
    }
    
    return false;
}

std::string Memory::recall(const std::string& query, const std::string& scope, size_t max_chars) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    std::string out;
    size_t current_chars = 0;
    
    for (const auto& rs : roots_with_labels()) {
        if (scope != "all" && rs.scope != scope) continue;
        if (!fs::exists(rs.root)) continue;
        
        try {
            for (const auto& p : fs::directory_iterator(rs.root)) {
                if (p.path().extension() == ".md") {
                    // Extract filename for metadata (basic parsing)
                    std::ifstream in(p.path());
                    std::string line;
                    bool in_frontmatter = false;
                    size_t fm_end_pos = 0;
                    std::string content;
                    
                    // Try to parse frontmatter
                    while (std::getline(in, line)) {
                        if (line == "---") {
                            if (!in_frontmatter) {
                                in_frontmatter = true;
                            } else {
                                in_frontmatter = false;
                                fm_end_pos = content.length();
                                break;
                            }
                        } else if (in_frontmatter) {
                            content += line + "\n";
                        }
                    }
                    
                    // Read actual content after frontmatter
                    content.clear();
                    in.clear();
                    in.seekg(0);
                    bool skip_frontmatter = true;
                    while (std::getline(in, line)) {
                        if (line == "---") {
                            skip_frontmatter = !skip_frontmatter;
                            continue;
                        }
                        if (!skip_frontmatter) {
                            content += line + "\n";
                        }
                    }
                    
                    // Basic relevance filtering
                    if (!query.empty()) {
                        std::string name = p.path().stem().string();
                        std::string desc = "";
                        std::string tmp;
                        in.clear();
                        in.seekg(0);
                        bool found_desc = false;
                        while (std::getline(in, tmp) && !found_desc) {
                            if (tmp.find("description:") == 0) {
                                desc = tmp.substr(13);
                                found_desc = true;
                            }
                        }
                        
                        std::string q_lower = query;
                        std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), ::tolower);
                        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                        
                        if (name.find(q_lower) == std::string::npos && 
                            desc.find(q_lower) == std::string::npos) {
                            continue;
                        }
                    }
                    
                    // Check for memory size limit
                    if (current_chars + content.length() > max_chars) {
                        std::cerr << "Memory recall truncated at " << max_chars << " characters" << std::endl;
                        break;
                    }
                    
                    out += "---\n";
                    out += "Source: " + p.path().filename().string() + "\n";
                    out += "Scope: " + rs.scope + "\n";
                    out += "Name: " + p.path().stem().string() + "\n";
                    out += "---\n\n";
                    out += content;
                    out += "\n---\n";
                    
                    current_chars += content.length();
                }
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Directory iteration error: " << e.what() << std::endl;
        }
    }
    
    return out;
}

bool Memory::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    bool deleted = false;
    for (const auto& rs : roots_with_labels()) {
        fs::path p = rs.root / (name + ".md");
        if (fs::exists(p)) {
            try {
                fs::remove(p);
                deleted = true;
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Failed to remove file " << p << ": " << e.what() << std::endl;
            }
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
