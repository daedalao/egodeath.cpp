#pragma once
#include "common.hpp"
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>

namespace egodeath {

// Minimal MCP (Model Context Protocol) client: spawns stdio servers, performs the
// initialize handshake, discovers tools, and routes tools/call requests.
class McpManager {
public:
    ~McpManager();

    // Load server definitions: {"mcpServers": {"<name>": {"command":..,"args":[..],"env":{..}}}}
    void load_config(const std::filesystem::path& path);
    // Spawn + initialize + tools/list every configured server (sequential).
    void connect_all();

    // OpenAI-format tool schemas for all live servers; names are mcp__<server>__<tool>.
    json all_tool_schemas() const;
    bool owns(const std::string& tool_name) const;
    // Route a prefixed tool call to its server; returns text result or an "error: ..." string.
    std::string call(const std::string& tool_name, const json& args);

    std::string status() const;       // human-readable, for /mcp
    void shutdown_all();
    bool any_configured() const;

private:
    struct Server {
        std::string name;
        std::string command;
        std::vector<std::string> args;
        std::map<std::string, std::string> env;

        pid_t pid = -1;
        int in_fd = -1;   // parent -> child stdin
        int out_fd = -1;  // child stdout -> parent
        std::thread reader;
        std::atomic<bool> reader_run{false};

        std::atomic<int> next_id{1};
        std::map<int, json> results;
        std::mutex m;
        std::condition_variable cv;

        std::vector<json> tools;  // raw tools/list entries
        bool alive = false;
        std::string last_error;
    };

    std::vector<std::unique_ptr<Server>> servers_;
    // prefixed tool name -> (server, original tool name)
    std::map<std::string, std::pair<Server*, std::string>> tool_index_;
    mutable std::mutex mtx_;

    bool spawn(Server& s);
    void reader_loop(Server* s);
    json request(Server& s, const std::string& method, const json& params, int timeout_ms = 20000);
    void notify(Server& s, const std::string& method, const json& params);
    bool initialize(Server& s);
    void list_tools(Server& s);
    static bool write_all(int fd, const std::string& data);
};

} // namespace egodeath
