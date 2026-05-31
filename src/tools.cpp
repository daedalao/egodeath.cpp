#include "tools.hpp"
#include <fstream>
#include <sstream>
#include <array>
#include <memory>
#include <cstdio>
#include <fnmatch.h>

namespace egodeath {

Tools::Tools(std::filesystem::path root) : root_(std::move(root)) {
    ignored_patterns_ = {".git", "node_modules", "__pycache__", "build", "dist"};
}

json Tools::schema() {
    return json::parse(R"([
        {"type": "function", "function": {"name": "read_file", "parameters": {"type": "object", "properties": {"filepath": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "write_file", "parameters": {"type": "object", "properties": {"filepath": {"type": "string"}, "content": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "grep_search", "parameters": {"type": "object", "properties": {"pattern": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "exec_shell", "parameters": {"type": "object", "properties": {"command": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "list_directory", "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "glob_search", "parameters": {"type": "object", "properties": {"pattern": {"type": "string"}}}}}
    ])");
}

std::string Tools::dispatch(const std::string& name, const json& args) {
    if (name == "read_file") return read_file(root_ / args.value("filepath", ""));
    if (name == "write_file") return write_file(root_ / args.value("filepath", ""), args.value("content", ""));
    if (name == "grep_search") return grep_search(args.value("pattern", ""), root_);
    if (name == "exec_shell") return exec_shell(args.value("command", ""));
    if (name == "list_directory") return list_directory(root_ / args.value("path", "."));
    if (name == "glob_search") return glob_search(args.value("pattern", ""), root_);
    return "error: unknown tool";
}

std::string Tools::read_file(const std::filesystem::path& p) {
    std::ifstream in(p); if (!in) return "error: could not open file";
    std::stringstream ss; ss << in.rdbuf(); return ss.str();
}

std::string Tools::write_file(const std::filesystem::path& p, const std::string& c) {
    std::ofstream out(p); if (!out) return "error: could not write file";
    out << c; return "file written";
}

std::string Tools::grep_search(const std::string& pat, const std::filesystem::path& root) {
    std::string res; std::regex re;
    try { re = std::regex(pat); } catch (...) { return "error: invalid regex"; }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::ifstream in(entry.path()); std::string line; int ln = 1;
            while (std::getline(in, line)) {
                if (std::regex_search(line, re)) res += fmt::format("{}:{} {}\n", entry.path().filename().string(), ln, line);
                ln++;
            }
        }
    }
    return res.empty() ? "no matches" : res;
}

std::string Tools::exec_shell(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "error: popen failed";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result.empty() ? "(empty output)" : result;
}

std::string Tools::list_directory(const std::filesystem::path& p) {
    if (!std::filesystem::exists(p)) return "error: path does not exist";
    if (!std::filesystem::is_directory(p)) return "error: not a directory";
    std::string res;
    for (const auto& entry : std::filesystem::directory_iterator(p)) {
        res += fmt::format("{} {}\n", entry.is_directory() ? "[DIR]" : "[FILE]", entry.path().filename().string());
    }
    return res.empty() ? "(empty directory)" : res;
}

std::string Tools::glob_search(const std::string& pattern, const std::filesystem::path& root) {
    std::string res;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (fnmatch(pattern.c_str(), entry.path().filename().string().c_str(), 0) == 0) {
            res += entry.path().relative_path().string() + "\n";
        }
    }
    return res.empty() ? "no matches" : res;
}

} // namespace egodeath
