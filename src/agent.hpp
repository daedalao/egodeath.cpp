#pragma once
#include "common.hpp"
#include "client.hpp"
#include "tools.hpp"
#include "tui.hpp"

namespace egodeath {

class Agent {
public:
    Agent(LlamaClient::Config cfg, ProTUI& tui);
    
    void run_step(const std::string& input);
    void turn_async(int depth = 0);
    
    void save_session(const std::string& path);
    void load_session(const std::string& path);
    
    UIState state;
    json history;
    std::function<void(const json&)> on_metrics;

private:
    LlamaClient client_;
    Tools tools_;
    ProTUI& tui_;
    std::mutex mtx_;
};

} // namespace egodeath
