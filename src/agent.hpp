#pragma once
#include "common.hpp"
#include "client.hpp"
#include "tools.hpp"
#include "tui.hpp"
#include "memory.hpp"
#include "mcp.hpp"
#include "store.hpp"

#include <queue>
#include <condition_variable>
#include <functional>
#include <memory>

namespace egodeath {

class Agent {
public:
    Agent(LlamaClient::Config cfg, ProTUI& tui,
          std::filesystem::path project_memory_root = {});
    int get_context_size() const;
    void set_reasoning_effort(const std::string& e);
    void set_auto_approve(bool b) { auto_approve_.store(b); }
    void set_shell_enabled(bool b) { shell_enabled_.store(b); }
    bool shell_enabled() const { return shell_enabled_.load(); }
    void set_ctx_size(int n) { ctx_size_ = n; }
    void set_compact_at(double f) { compact_at_ = f; }
    void set_web_enabled(bool b) { web_enabled_.store(b); }
    bool web_enabled() const { return web_enabled_.load(); }
    void set_searxng_url(const std::string& u) { searxng_url_ = u; }
    std::string undo_last_edit();
    std::string mcp_status() const { return mcp_.status(); }
    json agenda_snapshot();
    std::string agenda_action(const std::string& op, long long id, const std::string& arg);
    std::string last_written_file();
    std::string editor_save(const std::string& path, const std::string& content);
    void cancel();
    ~Agent();
    
    void run_step(const std::string& input, const std::string& display_override = "");
    
    void turn_async(int depth = 0);
    void shutdown();
    
    void save_session(const std::string& path);
    void load_session(const std::string& path);
    bool is_busy() const { return turn_running_.load(); }
    
    UIState state;
    json history;
    std::function<void(const json&)> on_metrics;

private:
    LlamaClient client_;
    Tools tools_;
    ProTUI& tui_;
    std::mutex mtx_;
    
    std::mutex async_mtx_;
    std::atomic<bool> running_{true};
    std::atomic<int> queued_{0};       // tasks waiting in the queue
    std::atomic<bool> turn_running_{false}; // true while turn_async(0) is executing
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> auto_approve_{false};
    std::atomic<bool> shell_enabled_{false};
    int ctx_size_ = 0;
    double compact_at_ = 0.85;
    std::atomic<bool> web_enabled_{false};
    std::string searxng_url_;
    struct FileCheckpoint { std::filesystem::path path; bool existed = false; std::string prior; };
    std::vector<FileCheckpoint> checkpoints_;
    std::mutex checkpoints_mtx_;
    std::queue<std::string> pending_inputs_; // content deferred until turn starts
    std::mutex pending_inputs_mtx_;
    
    std::queue<UIEvent> ui_queue_;
    std::mutex ui_queue_mtx_;
    std::condition_variable ui_queue_cv_;
    
    std::thread ui_worker_;
    
    std::thread background_worker_;
    
    std::queue<std::function<void()>> tasks_;
    std::mutex tasks_mtx_;
    std::condition_variable tasks_cv_;
    
    std::atomic<bool> stop_background_{false};
    
    void process_ui_events();
    void maybe_compress_history();
    std::string web_search(const std::string& query, int max_results);
    std::string web_fetch(const std::string& url);
    void checkpoint_file(const std::filesystem::path& p);
    void rebuild_tui_from_history();

    Memory memory_;
    McpManager mcp_;
    std::unique_ptr<Store> store_;
    std::filesystem::path config_dir_;
    json read_config();
    std::string write_config(const json& cfg);
    std::string last_file_;
    std::mutex last_file_mtx_;
};

} // namespace egodeath
