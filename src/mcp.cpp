#include "mcp.hpp"

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <chrono>

extern char** environ;

namespace egodeath {

McpManager::~McpManager() {
    shutdown_all();
}

bool McpManager::any_configured() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !servers_.empty();
}

void McpManager::load_config(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return;
    json cfg;
    try {
        std::stringstream ss; ss << f.rdbuf();
        cfg = json::parse(ss.str());
    } catch (...) { return; }

    if (!cfg.contains("mcpServers") || !cfg["mcpServers"].is_object()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [name, def] : cfg["mcpServers"].items()) {
        if (!def.is_object() || !def.contains("command")) continue;
        auto s = std::make_unique<Server>();
        s->name = name;
        s->command = def.value("command", std::string());
        if (def.contains("args") && def["args"].is_array())
            for (auto& a : def["args"]) if (a.is_string()) s->args.push_back(a.get<std::string>());
        if (def.contains("env") && def["env"].is_object())
            for (auto& [k, v] : def["env"].items())
                s->env[k] = v.is_string() ? v.get<std::string>() : v.dump();
        servers_.push_back(std::move(s));
    }
}

bool McpManager::write_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

bool McpManager::spawn(Server& s) {
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0) return false;
    if (pipe(from_child) != 0) { close(to_child[0]); close(to_child[1]); return false; }

    // Build argv and envp in the parent (no allocation between fork and exec).
    std::vector<std::string> argv_store;
    argv_store.push_back(s.command);
    for (auto& a : s.args) argv_store.push_back(a);
    std::vector<char*> argv;
    for (auto& a : argv_store) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::vector<std::string> env_store;
    for (char** e = environ; e && *e; ++e) env_store.push_back(*e);
    for (auto& [k, v] : s.env) env_store.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& e : env_store) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return false;
    }

    if (pid == 0) {
        // Child: wire stdin/stdout to the pipes, silence stderr to /dev/null so the
        // server's logging can never reach (and corrupt) our ncurses terminal.
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        if (devnull >= 0) close(devnull);
        execvpe(s.command.c_str(), argv.data(), envp.data());
        _exit(127); // exec failed
    }

    // Parent
    close(to_child[0]);
    close(from_child[1]);
    s.pid = pid;
    s.in_fd = to_child[1];
    s.out_fd = from_child[0];
    s.reader_run.store(true);
    s.reader = std::thread([this, sp = &s]() { this->reader_loop(sp); });
    return true;
}

void McpManager::reader_loop(Server* s) {
    std::string buf;
    char chunk[4096];
    while (s->reader_run.load()) {
        ssize_t n = ::read(s->out_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break; // EOF or error -> server gone
        }
        buf.append(chunk, (size_t)n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            json msg;
            try { msg = json::parse(line); } catch (...) { continue; }
            if (msg.contains("id") && msg["id"].is_number_integer()) {
                int id = msg["id"].get<int>();
                std::lock_guard<std::mutex> lk(s->m);
                s->results[id] = msg;
                s->cv.notify_all();
            }
            // notifications (no id) are ignored for the MVP
        }
    }
}

json McpManager::request(Server& s, const std::string& method, const json& params, int timeout_ms) {
    int id = s.next_id.fetch_add(1);
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) req["params"] = params;
    std::string line = req.dump() + "\n";
    if (s.in_fd < 0 || !write_all(s.in_fd, line))
        return {{"error", {{"message", "write failed (server gone?)"}}}};

    std::unique_lock<std::mutex> lk(s.m);
    bool got = s.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [&]() { return s.results.count(id) > 0; });
    if (!got) return {{"error", {{"message", "timeout"}}}};
    json resp = s.results[id];
    s.results.erase(id);
    if (resp.contains("error")) return {{"error", resp["error"]}};
    return resp.contains("result") ? resp["result"] : json::object();
}

void McpManager::notify(Server& s, const std::string& method, const json& params) {
    json n = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) n["params"] = params;
    if (s.in_fd >= 0) write_all(s.in_fd, n.dump() + "\n");
}

bool McpManager::initialize(Server& s) {
    json params = {
        {"protocolVersion", "2025-06-18"},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "egodeath"}, {"version", "0.1"}}}
    };
    json resp = request(s, "initialize", params);
    if (resp.contains("error")) {
        s.last_error = resp["error"].value("message", std::string("initialize failed"));
        return false;
    }
    notify(s, "notifications/initialized", json::object());
    return true;
}

void McpManager::list_tools(Server& s) {
    json resp = request(s, "tools/list", json::object());
    if (resp.contains("error")) {
        s.last_error = resp["error"].value("message", std::string("tools/list failed"));
        return;
    }
    if (resp.contains("tools") && resp["tools"].is_array())
        s.tools = resp["tools"].get<std::vector<json>>();
}

void McpManager::connect_all() {
    signal(SIGPIPE, SIG_IGN); // a dead server must not kill us via a broken-pipe write
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& up : servers_) {
        Server& s = *up;
        if (!spawn(s)) { s.last_error = "failed to spawn '" + s.command + "'"; continue; }
        if (!initialize(s)) continue;
        list_tools(s);
        s.alive = true;
        for (auto& t : s.tools) {
            std::string tname = t.value("name", std::string());
            if (tname.empty()) continue;
            tool_index_["mcp__" + s.name + "__" + tname] = {&s, tname};
        }
    }
}

json McpManager::all_tool_schemas() const {
    std::lock_guard<std::mutex> lk(mtx_);
    json arr = json::array();
    for (auto& up : servers_) {
        if (!up->alive) continue;
        for (auto& t : up->tools) {
            std::string tname = t.value("name", std::string());
            if (tname.empty()) continue;
            json params = (t.contains("inputSchema") && t["inputSchema"].is_object())
                              ? t["inputSchema"]
                              : json{{"type", "object"}, {"properties", json::object()}};
            arr.push_back({
                {"type", "function"},
                {"function", {
                    {"name", "mcp__" + up->name + "__" + tname},
                    {"description", t.value("description", std::string())},
                    {"parameters", params}
                }}
            });
        }
    }
    return arr;
}

bool McpManager::owns(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return tool_index_.count(tool_name) > 0;
}

std::string McpManager::call(const std::string& tool_name, const json& args) {
    Server* s = nullptr;
    std::string original;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tool_index_.find(tool_name);
        if (it == tool_index_.end()) return "error: unknown MCP tool";
        s = it->second.first;
        original = it->second.second;
    }
    if (!s->alive) return "error: MCP server is not running";

    json params = {{"name", original}, {"arguments", args.is_null() ? json::object() : args}};
    json resp = request(*s, "tools/call", params, 60000);
    if (resp.contains("error"))
        return "error: " + resp["error"].value("message", std::string("tool call failed"));

    std::string out;
    bool is_error = resp.value("isError", false);
    if (resp.contains("content") && resp["content"].is_array()) {
        for (auto& c : resp["content"]) {
            std::string ctype = c.value("type", std::string());
            if (ctype == "text") out += c.value("text", std::string()) + "\n";
            else if (ctype == "image") out += "[image omitted]\n";
            else if (ctype == "resource") out += "[resource: " +
                (c.contains("resource") ? c["resource"].value("uri", std::string()) : std::string()) + "]\n";
        }
    }
    if (out.empty()) out = resp.dump();
    return (is_error ? std::string("error: ") : std::string()) + out;
}

std::string McpManager::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (servers_.empty()) return "no MCP servers configured";
    std::string out;
    for (auto& up : servers_) {
        out += (up->alive ? "● " : "○ ") + up->name +
               (up->alive ? "  (" + std::to_string(up->tools.size()) + " tools)" : "  [down]");
        if (!up->alive && !up->last_error.empty()) out += ": " + up->last_error;
        out += "\n";
        if (up->alive) {
            for (auto& t : up->tools) {
                out += "    mcp__" + up->name + "__" + t.value("name", std::string());
                std::string d = t.value("description", std::string());
                size_t nl = d.find('\n'); if (nl != std::string::npos) d = d.substr(0, nl);
                if (d.size() > 60) d = d.substr(0, 57) + "...";
                if (!d.empty()) out += "  — " + d;
                out += "\n";
            }
        }
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

void McpManager::shutdown_all() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& up : servers_) {
        Server& s = *up;
        s.reader_run.store(false);
        if (s.in_fd >= 0) { close(s.in_fd); s.in_fd = -1; }   // EOF to child stdin -> exit
        if (s.out_fd >= 0) { close(s.out_fd); s.out_fd = -1; } // unblock reader read()
        if (s.reader.joinable()) s.reader.join();
        if (s.pid > 0) {
            int st;
            for (int i = 0; i < 20 && waitpid(s.pid, &st, WNOHANG) == 0; ++i)
                usleep(10000);
            if (waitpid(s.pid, &st, WNOHANG) == 0) {
                kill(s.pid, SIGTERM);
                usleep(50000);
                if (waitpid(s.pid, &st, WNOHANG) == 0) { kill(s.pid, SIGKILL); waitpid(s.pid, &st, 0); }
            }
            s.pid = -1;
        }
        s.alive = false;
    }
}

} // namespace egodeath
