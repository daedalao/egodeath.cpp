#include "tools.hpp"
#include <fstream>
#include <sstream>
#include <array>
#include <memory>
#include <cstdio>
#include <fnmatch.h>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace egodeath {

Tools::Tools(std::filesystem::path root) : root_(std::move(root)) {
    ignored_patterns_ = {".git", "node_modules", "__pycache__", "build", "dist"};
}

json Tools::schema() {
    static const json s = json::parse(R"([
        {"type": "function", "function": {"name": "read_file", "parameters": {"type": "object", "properties": {"filepath": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "write_file", "parameters": {"type": "object", "properties": {"filepath": {"type": "string"}, "content": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "grep_search", "parameters": {"type": "object", "properties": {"pattern": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "exec_shell", "parameters": {"type": "object", "properties": {"command": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "list_directory", "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}}},
        {"type": "function", "function": {"name": "glob_search", "parameters": {"type": "object", "properties": {"pattern": {"type": "string"}}}}}
    ])");
    return s;
}


static bool is_safe_path(const std::filesystem::path& root, const std::filesystem::path& p) {
    std::error_code ec;
    auto r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    auto q = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;
    auto rs = r.string();
    auto qs = q.string();
    return qs.size() >= rs.size() && qs.compare(0, rs.size(), rs) == 0;
}

std::string Tools::dispatch(const std::string& name, const json& args) {
    if (name == "read_file") {
        auto p = root_ / args.value("filepath", "");
        if (!is_safe_path(root_, p)) return "error: path outside workspace";
        return read_file(p);
    }
    if (name == "write_file") {
        auto p = root_ / args.value("filepath", "");
        if (!is_safe_path(root_, p)) return "error: path outside workspace";
        return write_file(p, args.value("content", ""));
    }
    if (name == "grep_search") return grep_search(args.value("pattern", ""));
    if (name == "exec_shell") return exec_shell(args.value("command", ""));
    if (name == "list_directory") return list_directory(root_ / args.value("path", "."));
    if (name == "glob_search") return glob_search(args.value("pattern", ""));
    return "error: unknown tool";
}

std::string Tools::read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "error: could not open file";
    constexpr size_t MAX_BYTES = 512 * 1024;
    std::string content(MAX_BYTES + 1, '\0');
    in.read(content.data(), MAX_BYTES + 1);
    size_t bytes_read = in.gcount();
    content.resize(bytes_read);
    if (bytes_read > MAX_BYTES) {
        content.resize(MAX_BYTES);
        content += "\n... [truncated at 512KB]";
    }
    return content;
}

std::string Tools::write_file(const std::filesystem::path& p, const std::string& c) {
    std::ofstream out(p); if (!out) return "error: could not write file";
    out << c; return "file written";
}

std::string Tools::grep_search(const std::string& pat) {
    std::regex re;
    try { re = std::regex(pat); } catch (...) { return "error: invalid regex"; }
    namespace fs = std::filesystem;
    std::string res; int matches = 0; std::error_code ec;
    fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        std::error_code dec;
        if (entry.is_directory(dec)) {
            if (ignored_patterns_.count(entry.path().filename().string()))
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(dec)) continue;
        {
            std::ifstream probe(entry.path(), std::ios::binary);
            char b[512];
            std::streamsize n = probe.read(b, sizeof(b)).gcount();
            if (n > 0 && std::memchr(b, '\0', (size_t)n) != nullptr) continue;
        }
        std::error_code rec;
        std::string rel = fs::relative(entry.path(), root_, rec).string();
        if (rec || rel.empty()) rel = entry.path().string();
        std::ifstream in(entry.path()); std::string line; int ln = 1;
        while (std::getline(in, line)) {
            if (std::regex_search(line, re)) {
                res += fmt::format("{}:{} {}\n", rel, ln, line);
                if (++matches >= 500) return res + "... [truncated at 500 matches]\n";
            }
            ln++;
        }
    }
    return res.empty() ? "no matches" : res;
}

std::string Tools::exec_shell(const std::string& cmd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "error: pipe failed";
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return "error: fork failed"; }
    if (pid == 0) {
        // Child: route stdout AND stderr into the pipe; read stdin from /dev/null.
        // Passing cmd as a single argument to `sh -c` keeps multi-line scripts and
        // heredocs intact, and because stderr goes to the pipe (never the inherited
        // terminal) a shell syntax error can no longer corrupt the ncurses display.
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); close(dn); }
        close(pipefd[0]); close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    std::string result; char buf[4096]; ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) result.append(buf, (size_t)n);
    close(pipefd[0]);
    int status = 0; waitpid(pid, &status, 0);
    if (result.size() > 512 * 1024) { result.resize(512 * 1024); result += "\n... [truncated at 512KB]"; }
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

std::string Tools::glob_search(const std::string& pattern) {
    namespace fs = std::filesystem;
    std::string res; int count = 0; std::error_code ec;
    fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        std::error_code dec;
        if (entry.is_directory(dec)) {
            if (ignored_patterns_.count(entry.path().filename().string()))
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(dec)) continue;
        std::error_code rec;
        std::string rel = fs::relative(entry.path(), root_, rec).string();
        if (rec) continue;
        bool m = fnmatch(pattern.c_str(), entry.path().filename().string().c_str(), 0) == 0
              || fnmatch(pattern.c_str(), rel.c_str(), FNM_PATHNAME) == 0;
        if (m) {
            res += rel + "\n";
            if (++count >= 1000) return res + "... [truncated at 1000 files]\n";
        }
    }
    return res.empty() ? "no matches" : res;
}

} // namespace egodeath
