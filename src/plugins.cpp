#include "plugins.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <cstdlib>

using json = nlohmann::json;
namespace fs = std::filesystem;

int Plugins::parse_fkey(const std::string& s) {
    // Accept "F6".."F12" (or "f6"); returns the function number, 0 if invalid.
    if (s.size() < 2 || (s[0] != 'F' && s[0] != 'f')) return 0;
    int n = 0;
    for (size_t i = 1; i < s.size(); ++i) { if (!isdigit((unsigned char)s[i])) return 0; n = n * 10 + (s[i] - '0'); }
    return (n >= 1 && n <= 12) ? n : 0;
}

void Plugins::load(const fs::path& root) {
    plugins_.clear();
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        fs::path manifest = entry.path() / "plugin.json";
        std::ifstream in(manifest);
        if (!in.good()) continue;
        json j;
        try { in >> j; } catch (...) { continue; }
        if (!j.is_object()) continue;

        Plugin p;
        p.dir = entry.path();
        p.name = j.value("name", entry.path().filename().string());
        p.exec = j.value("exec", "");
        if (p.exec.empty()) continue;  // nothing to run

        if (j.contains("panel") && j["panel"].is_object()) {
            const auto& pn = j["panel"];
            p.has_panel = true;
            p.panel_key = parse_fkey(pn.value("key", ""));
            p.panel_title = pn.value("title", p.name);
            p.panel_refresh = pn.value("refresh", 0);
        }
        if (j.contains("command") && j["command"].is_object()) {
            std::string slash = j["command"].value("slash", "");
            if (!slash.empty()) {
                if (slash[0] != '/') slash = "/" + slash;
                p.has_command = true;
                p.slash = slash;
            }
        }
        if (p.has_panel || p.has_command) plugins_.push_back(std::move(p));
    }
}

const Plugins::Plugin* Plugins::by_slash(const std::string& slash) const {
    for (const auto& p : plugins_) if (p.has_command && p.slash == slash) return &p;
    return nullptr;
}
const Plugins::Plugin* Plugins::by_name(const std::string& name) const {
    for (const auto& p : plugins_) if (p.name == name) return &p;
    return nullptr;
}

std::string Plugins::run(const Plugin& p, const std::string& mode, const std::string& args) const {
    int fd[2];
    if (pipe(fd) != 0) return "plugin: pipe failed";
    char hostcwd[4096];
    if (!getcwd(hostcwd, sizeof(hostcwd))) hostcwd[0] = 0;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return "plugin: fork failed"; }
    if (pid == 0) {
        setsid();
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); close(dn); }
        close(fd[0]); close(fd[1]);
        if (chdir(p.dir.c_str()) != 0) _exit(126);
        setenv("EGODEATH_PLUGIN_MODE", mode.c_str(), 1);
        setenv("EGODEATH_PLUGIN_INPUT", args.c_str(), 1);
        setenv("EGODEATH_PLUGIN_NAME", p.name.c_str(), 1);
        setenv("EGODEATH_CWD", hostcwd, 1);  // the directory egodeath was launched in
        execl("/bin/sh", "sh", "-c", p.exec.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fd[1]);
    std::string out; char buf[4096]; ssize_t n;
    while ((n = read(fd[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fd[0]);
    int status = 0; waitpid(pid, &status, 0);
    if (out.size() > 256 * 1024) { out.resize(256 * 1024); out += "\n\xe2\x80\xa6 (truncated)"; }
    return out;
}

std::string Plugins::run_by_name(const std::string& name, const std::string& mode, const std::string& args) const {
    const Plugin* p = by_name(name);
    return p ? run(*p, mode, args) : std::string("plugin not found: ") + name;
}

std::string Plugins::list_text() const {
    if (plugins_.empty()) return "no plugins installed (~/.config/egodeath/plugins/<name>/plugin.json)";
    std::string out = "Installed plugins:\n";
    for (const auto& p : plugins_) {
        out += "  " + p.name;
        if (p.has_panel) out += "  [panel F" + std::to_string(p.panel_key) + " \"" + p.panel_title + "\"]";
        if (p.has_command) out += "  [" + p.slash + "]";
        out += "\n";
    }
    return out;
}
