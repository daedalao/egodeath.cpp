#pragma once
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <deque>
#include <set>
#include <regex>
#include <nlohmann/json.hpp>
#include <ncurses.h>
#include <curl/curl.h>
#include <fmt/core.h>
#include <fmt/format.h>

namespace egodeath {
using json = nlohmann::json;

struct Metrics {
    std::optional<double> pp_speed;
    std::optional<double> gen_speed;
    std::optional<int> ctx_used;
    std::optional<int> ctx_size;
};

struct UIState {
    std::string topic;
    std::string activity;
    std::string reasoning;
    Metrics metrics;
};

struct UIEvent {
    enum class Type { STREAM_CONTENT, STREAM_REASONING, STREAM_TOOL_CALL, STREAM_END, USER_INPUT, USER_INPUT_QUEUED, ACTIVATE_QUEUED, STREAM_START, METRICS_UPDATE, TOOL_DISPLAY };
    Type type;
    std::string content;
    json tool_call_delta = json::object();
    json timings = json::object();
};

extern const std::string DEFAULT_SYSTEM_PROMPT;

class Style {
public:
    static std::string wrap(std::string_view text, std::string_view color);
    static std::string markdown(std::string text);
};

std::string banner();

} // namespace egodeath
