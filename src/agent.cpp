#include "agent.hpp"
#include <thread>
#include <chrono>
#include <iostream>

namespace egodeath {

Agent::Agent(LlamaClient::Config cfg, ProTUI& tui) 
    : client_(std::move(cfg)), tools_(std::filesystem::current_path()), tui_(tui) {
    history = json::array();
    history.push_back({{"role", "system"}, {"content", DEFAULT_SYSTEM_PROMPT}});
}

std::string repair_json(std::string s) {
    int quotes = 0;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '"' && (i == 0 || s[i-1] != '\\')) quotes++;
    }
    if (quotes % 2 != 0) s += '"';
    int braces = 0;
    for (char c : s) {
        if (c == '{') braces++;
        if (c == '}') braces--;
    }
    while (braces > 0) { s += '}'; braces--; }
    return s;
}

void Agent::run_step(const std::string& input) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        history.push_back({{"role", "user"}, {"content", input}});
    }
    tui_.append_history("you", input, "user");
    tui_.clear_reasoning();
    tui_.set_activity("thinking");
    std::thread([this]() { turn_async(0); }).detach();
}

void Agent::turn_async(int depth) {
    if (depth >= 16) { tui_.set_activity("idle"); return; }
    std::string content;
    json tool_calls = json::array();
    bool is_first = true;
    
    client_.chat_stream(history, tools_.schema(), [&](const UIEvent& ev) {
        if (ev.type == UIEvent::Type::STREAM_CONTENT) {
            if (is_first) { tui_.append_history("egodeath", ev.content, "assistant"); is_first = false; }
            else tui_.append_last_history(ev.content);
            content += ev.content;
        } else if (ev.type == UIEvent::Type::STREAM_REASONING) {
            tui_.update_reasoning(ev.content);
        } else if (ev.type == UIEvent::Type::STREAM_TOOL_CALL) {
            for (auto& tc : ev.tool_call_delta) {
                size_t idx = tc.value("index", 0);
                while (tool_calls.size() <= idx) {
                    json new_call = json::object();
                    new_call["type"] = "function";
                    tool_calls.push_back(new_call);
                }
                auto& target = tool_calls[idx];
                if (tc.contains("id") && tc["id"].is_string()) target["id"] = tc["id"];
                if (tc.contains("function") && tc["function"].is_object()) {
                    if (!target.contains("function")) target["function"] = json::object();
                    auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string()) target["function"]["name"] = fn["name"];
                    if (fn.contains("arguments") && !fn["arguments"].is_null()) {
                        auto& raw_args = fn["arguments"];
                        std::string chunk = raw_args.is_string() ? raw_args.get<std::string>() : raw_args.dump();
                        if (!target["function"].contains("arguments")) target["function"]["arguments"] = "";
                        target["function"]["arguments"] = target["function"]["arguments"].get<std::string>() + chunk;
                    }
                }
            }
        }
    });

    json assistant_msg = {{"role", "assistant"}};
    if (!content.empty()) assistant_msg["content"] = content;
    
    if (!tool_calls.empty()) {
        json valid_calls = json::array();
        for (auto& call : tool_calls) {
            if (!call.contains("function") || !call["function"].is_object()) continue;
            std::string arg_str = "{}";
            if (call["function"].contains("arguments") && call["function"]["arguments"].is_string()) {
                arg_str = call["function"]["arguments"].get<std::string>();
            }
            
            bool ok = false;
            try { json::parse(arg_str); ok = true; } 
            catch (...) {
                std::string repaired = repair_json(arg_str);
                try { json::parse(repaired); call["function"]["arguments"] = repaired; ok = true; } 
                catch (...) {}
            }
            if (ok) valid_calls.push_back(call);
            else {
                std::string fname = "?";
                if (call["function"].contains("name") && call["function"]["name"].is_string()) fname = call["function"]["name"];
                tui_.append_history("system", "Dropped malformed tool: " + fname, "system");
            }
        }
        
        if (!valid_calls.empty()) {
            assistant_msg["tool_calls"] = valid_calls;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                history.push_back(assistant_msg);
            }

            for (auto& call : valid_calls) {
                std::string name = "";
                if (call["function"].contains("name") && call["function"]["name"].is_string()) name = call["function"]["name"];
                std::string id = "";
                if (call.contains("id") && call["id"].is_string()) id = call["id"];
                
                std::string arg_str = "{}";
                if (call["function"].contains("arguments") && call["function"]["arguments"].is_string()) {
                    arg_str = call["function"]["arguments"].get<std::string>();
                }
                json args = json::parse(arg_str);

                tui_.set_activity("calling " + name);
                std::string res = tools_.dispatch(name, args);
                tui_.append_history("system", "Tool " + name + " executed.", "system");
                
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    history.push_back({
                        {"role", "tool"},
                        {"tool_call_id", id},
                        {"name", name},
                        {"content", res}
                    });
                }
            }
            turn_async(depth + 1);
        } else {
            tui_.set_activity("idle");
        }
    } else {
        if (!content.empty()) {
            std::lock_guard<std::mutex> lock(mtx_);
            history.push_back(assistant_msg);
        }
        tui_.set_activity("idle");
    }
}

void Agent::save_session(const std::string& path) {}
void Agent::load_session(const std::string& path) {}

} // namespace egodeath
