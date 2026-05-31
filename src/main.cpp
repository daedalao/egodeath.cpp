#include "agent.hpp"
#include "tui.hpp"
#include <iostream>

using namespace egodeath;

int main() {
    LlamaClient::Config config;
    if (auto e = std::getenv("LLAMA_ENDPOINT")) config.endpoint = e;
    if (auto m = std::getenv("LLAMA_MODEL")) config.model = m;

    ProTUI tui;
    tui.init();
    
    Agent agent(config, tui);

    agent.on_metrics = [&](const json& m) {
        Metrics metrics;
        if (m.contains("prompt_eval_time") && m.value("prompt_eval_time", 0.0) > 0)
            metrics.pp_speed = m.value("prompt_tokens", 0) / (m.value("prompt_eval_time", 1.0) / 1000.0);
        
        // Always preserve previous PP speed if current chunk doesn't have it
        if (metrics.pp_speed == 0.0 && agent.state.metrics.pp_speed.has_value()) {
            metrics.pp_speed = *agent.state.metrics.pp_speed;
        }

        if (m.contains("eval_time") && m.value("eval_time", 0.0) > 0)
            metrics.gen_speed = m.value("eval_tokens", 0) / (m.value("eval_time", 1.0) / 1000.0);
        
        tui.set_metrics(metrics);
        agent.state.metrics.pp_speed = metrics.pp_speed;
    };

    while (tui.is_running()) {
        std::string input = tui.get_input();
        if (input == "/exit" || input == "/quit") break;
        if (input.empty()) continue;

        // history handled by run_step now to be consistent with agent turn
        agent.run_step(input);
    }
    
    tui.shutdown();
    return 0;
}
