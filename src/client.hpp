#pragma once
#include "common.hpp"

namespace egodeath {

class LlamaClient {
public:
    struct Config {
        std::string endpoint = "http://127.0.0.1:8080/v1/chat/completions";
        std::string model = "local-model";
        std::optional<std::string> api_key;
        int timeout_seconds = 1800;
        bool verbose = false;
        std::string reasoning_effort = "medium"; // low | medium | high (sent when non-empty)
        bool cache_prompt = true; // reuse the server KV cache across turns (llama.cpp)
    };

    explicit LlamaClient(Config config);
    ~LlamaClient();

    json chat(const json& messages, const std::optional<json>& tools = std::nullopt);
    
    void chat_stream(const json& messages,
                    const std::optional<json>& tools,
                    std::function<void(const UIEvent&)> callback,
                    std::atomic<bool>* cancel = nullptr);

    int get_context_size() const;
    void set_reasoning_effort(const std::string& e) { config_.reasoning_effort = e; }

private:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    
    struct StreamCtx {
        std::function<void(const UIEvent&)> cb;
        std::string buffer;
        std::atomic<bool>* cancel = nullptr;
    };
    static size_t StreamCallback(void* contents, size_t size, size_t nmemb, void* userp);

    Config config_;
    
    // Thread-safe curl handle management: use local handles per request
    CURL* create_curl_handle() const;
};

} // namespace egodeath
