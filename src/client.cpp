#include "client.hpp"

namespace egodeath {

LlamaClient::LlamaClient(Config config) : config_(std::move(config)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ = curl_easy_init();
}

LlamaClient::~LlamaClient() {
    if (curl_) curl_easy_cleanup(curl_);
    curl_global_cleanup();
}

size_t LlamaClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

size_t LlamaClient::StreamCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<StreamCtx*>(userp);
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
                    ctx->cb({UIEvent::Type::STREAM_END, "", j["timings"]});
                }
                
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& delta = j["choices"][0]["delta"];
                    if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                        ctx->cb({UIEvent::Type::STREAM_REASONING, delta["reasoning_content"].get<std::string>()});
                    }
                    if (delta.contains("content") && delta["content"].is_string()) {
                        ctx->cb({UIEvent::Type::STREAM_CONTENT, delta["content"].get<std::string>()});
                    }
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        ctx->cb({UIEvent::Type::STREAM_TOOL_CALL, "", delta["tool_calls"]});
                    }
                }
            } catch (...) {}
        }
    }
    return total;
}

json LlamaClient::chat(const json& messages, const std::optional<json>& tools) {
    if (!curl_) return {};
    std::string response;
    json body = {{"model", config_.model}, {"messages", messages}, {"stream", false}};
    if (tools) body["tools"] = *tools;

    std::string json_body = body.dump();
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (config_.api_key) headers = curl_slist_append(headers, ("Authorization: Bearer " + *config_.api_key).c_str());

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, config_.endpoint.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    if (config_.verbose) curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1L);

    if (curl_easy_perform(curl_) != CURLE_OK) return {};
    curl_slist_free_all(headers);
    try { return json::parse(response); } catch (...) { return {}; }
}

void LlamaClient::chat_stream(const json& messages, const std::optional<json>& tools, std::function<void(const UIEvent&)> callback) {
    if (!curl_) return;
    json body = {{"model", config_.model}, {"messages", messages}, {"stream", true}};
    if (tools) body["tools"] = *tools;

    std::string json_body = body.dump();
    StreamCtx ctx{callback, ""};
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (config_.api_key) headers = curl_slist_append(headers, ("Authorization: Bearer " + *config_.api_key).c_str());

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, config_.endpoint.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, StreamCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    if (config_.verbose) curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1L);

    curl_easy_perform(curl_);
    curl_slist_free_all(headers);
}

} // namespace egodeath
