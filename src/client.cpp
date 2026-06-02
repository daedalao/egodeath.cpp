#include "client.hpp"
#include <mutex>
#include <memory>

namespace egodeath {

// Global curl initialization mutex for thread-safe init/cleanup
static std::mutex curl_init_mutex;
static int curl_init_count = 0;

static void cleanup_curl_handle(CURL* handle) {
    if (handle) {
        curl_easy_cleanup(handle);
    }
}

LlamaClient::LlamaClient(Config config) : config_(std::move(config)) {
    std::lock_guard<std::mutex> lock(curl_init_mutex);
    if (curl_init_count == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    curl_init_count++;
}

LlamaClient::~LlamaClient() {
    std::lock_guard<std::mutex> lock(curl_init_mutex);
    curl_init_count--;
    if (curl_init_count == 0) {
        curl_global_cleanup();
    }
}

CURL* LlamaClient::create_curl_handle() const {
    CURL* handle = curl_easy_init();
    if (!handle) return nullptr;
    
    // Set common options
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    
    return handle;
}

size_t LlamaClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

size_t LlamaClient::StreamCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<StreamCtx*>(userp);
    if (ctx->cancel && ctx->cancel->load()) return 0; // signal abort to curl
    ctx->buffer.append((char*)contents, total);

    size_t pos;
    while ((pos = ctx->buffer.find("\n")) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        if (line.compare(0, 6, "data: ") == 0) {
            std::string data = line.substr(6);
            if (data == "[DONE]") break;
            try {
                auto j = json::parse(data);
                
                if (j.contains("timings")) {
                    UIEvent ev;
                    ev.type = UIEvent::Type::STREAM_END;
                    ev.timings = j["timings"];
                    // Merge OpenAI usage (total prompt incl. cached) so the context
                    // gauge stays correct when the KV cache skips prefix tokens.
                    if (j.contains("usage") && j["usage"].is_object()) {
                        const auto& u = j["usage"];
                        if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
                            ev.timings["usage_prompt_tokens"] = u["prompt_tokens"];
                        if (u.contains("completion_tokens") && u["completion_tokens"].is_number())
                            ev.timings["usage_completion_tokens"] = u["completion_tokens"];
                        if (u.contains("total_tokens") && u["total_tokens"].is_number())
                            ev.timings["usage_total_tokens"] = u["total_tokens"];
                        if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object() &&
                            u["prompt_tokens_details"].contains("cached_tokens"))
                            ev.timings["usage_cached_tokens"] = u["prompt_tokens_details"]["cached_tokens"];
                    }
                    ctx->cb(ev);
                }
                
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& delta = j["choices"][0]["delta"];
                    if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                        UIEvent ev;
                        ev.type = UIEvent::Type::STREAM_REASONING;
                        ev.content = delta["reasoning_content"].get<std::string>();
                        ctx->cb(ev);
                    }
                    if (delta.contains("content") && delta["content"].is_string()) {
                        UIEvent ev;
                        ev.type = UIEvent::Type::STREAM_CONTENT;
                        ev.content = delta["content"].get<std::string>();
                        ctx->cb(ev);
                    }
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        UIEvent ev;
                        ev.type = UIEvent::Type::STREAM_TOOL_CALL;
                        ev.tool_call_delta = delta["tool_calls"];
                        ctx->cb(ev);
                    }
                }
            } catch (...) {}
        }
    }
    return total;
}

json LlamaClient::chat(const json& messages, const std::optional<json>& tools) {
    std::unique_ptr<CURL, void(*)(CURL*)> curl(create_curl_handle(), cleanup_curl_handle);
    if (!curl) return {};
    
    std::string response;
    json body = {{"model", config_.model}, {"messages", messages}, {"stream", false}, {"cache_prompt", config_.cache_prompt}};
    if (!config_.reasoning_effort.empty()) body["reasoning_effort"] = config_.reasoning_effort;
    if (tools) body["tools"] = *tools;

    std::string json_body = body.dump(-1, ' ', false, json::error_handler_t::replace);
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (config_.api_key) headers = curl_slist_append(headers, ("Authorization: Bearer " + *config_.api_key).c_str());

    curl_easy_setopt(curl.get(), CURLOPT_URL, config_.endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    if (config_.verbose) curl_easy_setopt(curl.get(), CURLOPT_VERBOSE, 1L);

    if (curl_easy_perform(curl.get()) != CURLE_OK) {
        curl_slist_free_all(headers);
        return {};
    }
    curl_slist_free_all(headers);
    try { return json::parse(response); } catch (...) { return {}; }
}

void LlamaClient::chat_stream(const json& messages, const std::optional<json>& tools, std::function<void(const UIEvent&)> callback, std::atomic<bool>* cancel) {
    std::unique_ptr<CURL, void(*)(CURL*)> curl(create_curl_handle(), cleanup_curl_handle);
    if (!curl) return;
    
    json body = {{"model", config_.model}, {"messages", messages}, {"stream", true}, {"cache_prompt", config_.cache_prompt}, {"stream_options", {{"include_usage", true}}}};
    if (!config_.reasoning_effort.empty()) body["reasoning_effort"] = config_.reasoning_effort;
    if (tools) body["tools"] = *tools;

    std::string json_body = body.dump(-1, ' ', false, json::error_handler_t::replace);
    StreamCtx ctx{callback, "", cancel};
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (config_.api_key) headers = curl_slist_append(headers, ("Authorization: Bearer " + *config_.api_key).c_str());

    curl_easy_setopt(curl.get(), CURLOPT_URL, config_.endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, StreamCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    if (config_.verbose) curl_easy_setopt(curl.get(), CURLOPT_VERBOSE, 1L);

    curl_easy_perform(curl.get());
    curl_slist_free_all(headers);
}


int LlamaClient::get_context_size() const {
    // Strip path from endpoint to get base URL, then hit /props
    std::string base = config_.endpoint;
    auto pos = base.find("://");
    if (pos != std::string::npos) {
        pos = base.find('/', pos + 3);
        if (pos != std::string::npos) base = base.substr(0, pos);
    }
    std::string url = base + "/props";

    std::unique_ptr<CURL, void(*)(CURL*)> curl(create_curl_handle(), cleanup_curl_handle);
    if (!curl) return 0;
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 5L);

    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);

    if (curl_easy_perform(curl.get()) != CURLE_OK) return 0;
    try {
        auto j = json::parse(response);
        // Top-level (older llama.cpp)
        if (j.contains("n_ctx") && j["n_ctx"].is_number())
            return j["n_ctx"].get<int>();
        // Nested under default_generation_settings (newer llama.cpp)
        if (j.contains("default_generation_settings")) {
            auto& dgs = j["default_generation_settings"];
            if (dgs.contains("n_ctx") && dgs["n_ctx"].is_number())
                return dgs["n_ctx"].get<int>();
            if (dgs.contains("params") && dgs["params"].contains("n_ctx"))
                return dgs["params"]["n_ctx"].get<int>();
        }
    } catch (...) {}
    return 0;
}

} // namespace egodeath
