#include "agent.hpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>

namespace egodeath {

Agent::Agent(LlamaClient::Config cfg, ProTUI& tui, std::filesystem::path project_memory_root)
    : client_(std::move(cfg)),
      tools_(std::filesystem::current_path()),
      tui_(tui),
      memory_(
          std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".egodeath" / "memory",
          std::move(project_memory_root),
          "egodeath"
      ) {
    history = json::array();
    // System prompt: prefer an external egodeath.md (project root, then .egodeath/),
    // falling back to the built-in DEFAULT_SYSTEM_PROMPT when none is present.
    std::string base_prompt;
    {
        namespace fs = std::filesystem;
        const fs::path candidates[] = {
            fs::current_path() / "egodeath.md",
            fs::current_path() / ".egodeath" / "egodeath.md",
        };
        for (const auto& p : candidates) {
            std::ifstream f(p);
            if (f) {
                std::stringstream ss; ss << f.rdbuf();
                base_prompt = ss.str();
                break;
            }
        }
        if (base_prompt.empty()) base_prompt = DEFAULT_SYSTEM_PROMPT;
    }
    std::string sys_prompt = base_prompt + memory_.system_prompt_addendum();
    history.push_back({{"role", "system"}, {"content", sys_prompt}});

    // Connect any configured MCP servers (EGODEATH_MCP_CONFIG, else .egodeath/mcp.json).
    {
        namespace fs = std::filesystem;
        fs::path cfg;
        if (const char* e = std::getenv("EGODEATH_MCP_CONFIG")) cfg = e;
        else if (fs::exists(fs::current_path() / ".egodeath" / "mcp.json"))
            cfg = fs::current_path() / ".egodeath" / "mcp.json";
        else {
            fs::path cdir = std::getenv("XDG_CONFIG_HOME")
                ? fs::path(std::getenv("XDG_CONFIG_HOME")) / "egodeath"
                : fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".config" / "egodeath";
            if (fs::exists(cdir / "mcp.json")) cfg = cdir / "mcp.json";
        }
        if (!cfg.empty()) { mcp_.load_config(cfg); mcp_.connect_all(); }
    }
    
    // Start UI event processing worker
    ui_worker_ = std::thread([this]() {
        this->process_ui_events();
    });
    
    // Start background task worker
    background_worker_ = std::thread([this]() {
        while (!stop_background_.load()) {
            std::unique_lock<std::mutex> lock(tasks_mtx_);
            tasks_cv_.wait(lock, [this]() {
                return !tasks_.empty() || stop_background_.load();
            });
            
            if (stop_background_.load() && tasks_.empty()) break;
            
            if (!tasks_.empty()) {
                auto task = std::move(tasks_.front());
                tasks_.pop();
                lock.unlock();
                task();
            }
        }
    });
}

Agent::~Agent() {
    shutdown();
}

void Agent::shutdown() {
    running_.store(false);
    stop_background_.store(true);
    tasks_cv_.notify_all();
    ui_queue_cv_.notify_all();
    
    if (background_worker_.joinable()) background_worker_.join();
    if (ui_worker_.joinable()) ui_worker_.join();
}

static std::string repair_json(std::string s) {
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

void Agent::run_step(const std::string& input, const std::string& display_override) {
    if (!running_.load()) return;

    // Store input for later — we add to LLM history when the turn actually starts,
    // not now. This prevents queued messages from polluting the current turn's context.
    {
        std::lock_guard<std::mutex> lock(pending_inputs_mtx_);
        pending_inputs_.push(input);
    }

    // Decide display: explicit override wins; otherwise condense large pastes.
    std::string display;
    if (!display_override.empty()) {
        display = display_override;
    } else {
        display = input;
        int nl = 0; for (char cc : input) if (cc == '\n') nl++;
        if (nl > 0 || (int)input.size() > 300)
            display = fmt::format("[Pasted \xc2\xb7 {} lines \xc2\xb7 {} chars]",
                                  nl + 1, (int)input.size());
    }

    // Is the agent already busy? Check before incrementing queued_.
    bool is_queued = turn_running_.load();

    int depth = ++queued_;
    tui_.clear_reasoning();
    if (!is_queued) tui_.set_activity("thinking");
    tui_.set_queued(is_queued ? depth - 1 : 0);

    // Show in chat with appropriate styling
    {
        std::lock_guard<std::mutex> lock(ui_queue_mtx_);
        UIEvent ev;
        ev.type = is_queued ? UIEvent::Type::USER_INPUT_QUEUED : UIEvent::Type::USER_INPUT;
        ev.content = display;
        ui_queue_.push(ev);
    }
    ui_queue_cv_.notify_one();

    // Enqueue turn
    {
        std::lock_guard<std::mutex> lock(tasks_mtx_);
        tasks_.push([this]() {
            // Pop the pending input and add it to LLM history now
            std::string content;
            {
                std::lock_guard<std::mutex> lock(pending_inputs_mtx_);
                if (!pending_inputs_.empty()) {
                    content = std::move(pending_inputs_.front());
                    pending_inputs_.pop();
                }
            }
            if (!content.empty()) {
                std::lock_guard<std::mutex> lock(mtx_);
                history.push_back({{"role", "user"}, {"content", content}});
            }

            // Signal TUI: activate the queued indicator (if any) for this message
            {
                std::lock_guard<std::mutex> lock(ui_queue_mtx_);
                ui_queue_.push({UIEvent::Type::ACTIVATE_QUEUED, ""});
            }
            ui_queue_cv_.notify_one();

            int waiting = --queued_;
            tui_.set_queued(waiting);
            tui_.set_activity("thinking");
            this->turn_async(0);
        });
    }
    tasks_cv_.notify_one();
}

void Agent::process_ui_events() {
    while (running_.load() || !ui_queue_.empty()) {
        std::unique_lock<std::mutex> lock(ui_queue_mtx_);
        ui_queue_cv_.wait(lock, [this]() {
            return !ui_queue_.empty() || !running_.load();
        });
        
        while (!ui_queue_.empty()) {
            auto ev = ui_queue_.front();
            ui_queue_.pop();
            lock.unlock();
            
            switch (ev.type) {
                case UIEvent::Type::USER_INPUT:
                    // ev.content is already the display version (condensed in run_step)
                    tui_.append_history("you", ev.content, "user");
                    break;
                case UIEvent::Type::USER_INPUT_QUEUED:
                    tui_.append_history("queued", ev.content, "user_queued");
                    break;
                case UIEvent::Type::ACTIVATE_QUEUED:
                    tui_.activate_last_queued();
                    break;
                case UIEvent::Type::STREAM_START:
                    tui_.append_history("egodeath", "", "assistant");
                    break;
                case UIEvent::Type::STREAM_CONTENT:
                    tui_.append_last_history(ev.content);
                    break;
                case UIEvent::Type::STREAM_REASONING:
                    tui_.update_reasoning(ev.content);
                    break;
                case UIEvent::Type::STREAM_TOOL_CALL:
                    // Process tool calls - these are handled in turn_async now
                    break;
                case UIEvent::Type::STREAM_END:
                    if (on_metrics) on_metrics(ev.timings);
                    break;
                case UIEvent::Type::METRICS_UPDATE:
                    if (on_metrics) on_metrics(ev.timings);
                    break;
                case UIEvent::Type::DIFF_DISPLAY:
                    tui_.append_history("", ev.content, "diff");
                    break;
                case UIEvent::Type::TOOL_DISPLAY:
                    tui_.append_history("", ev.content, "tool");
                    break;
            }
            
            lock.lock();
        }
        
        if (!running_.load() && ui_queue_.empty()) break;
    }
}

void Agent::set_reasoning_effort(const std::string& e) {
    client_.set_reasoning_effort(e);
}

static size_t _web_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string Agent::web_search(const std::string& query, int max_results) {
    if (!web_enabled_.load()) return "error: web search is disabled";
    if (searxng_url_.empty()) return "error: SearXNG URL not configured (set EGODEATH_SEARXNG_URL)";
    if (query.empty()) return "error: empty query";
    if (max_results <= 0 || max_results > 15) max_results = 5;

    CURL* curl = curl_easy_init();
    if (!curl) return "error: curl init failed";
    char* esc = curl_easy_escape(curl, query.c_str(), 0);
    std::string url = searxng_url_;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/search?q=" + std::string(esc ? esc : "") + "&format=json";
    if (esc) curl_free(esc);

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "egodeath/1.0");
    CURLcode rc = curl_easy_perform(curl);
    long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) return std::string("error: request failed: ") + curl_easy_strerror(rc);
    if (code == 403) return "error: SearXNG returned 403 - enable JSON output in settings.yml (search.formats: [html, json])";
    if (code != 200) return "error: SearXNG HTTP " + std::to_string(code);

    json j;
    try { j = json::parse(resp); } catch (...) { return "error: could not parse SearXNG response"; }
    if (!j.contains("results") || !j["results"].is_array() || j["results"].empty()) return "no results";

    std::string out;
    int n = 0;
    for (auto& r : j["results"]) {
        if (n >= max_results) break;
        std::string title = r.value("title", std::string());
        std::string link = r.value("url", std::string());
        std::string snippet = r.value("content", std::string());
        if (snippet.size() > 300) snippet = snippet.substr(0, 297) + "...";
        out += std::to_string(n + 1) + ". " + title + "\n   " + link + "\n";
        if (!snippet.empty()) out += "   " + snippet + "\n";
        n++;
    }
    return out.empty() ? "no results" : out;
}

static size_t ci_find(const std::string& s, const std::string& sub, size_t from) {
    auto it = std::search(s.begin() + from, s.end(), sub.begin(), sub.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it == s.end() ? std::string::npos : (size_t)(it - s.begin());
}

static std::string _html_to_text(std::string h) {
    auto strip_block = [](std::string& s, const std::string& tag) {
        std::string open = "<" + tag, close = "</" + tag + ">";
        size_t p;
        while ((p = ci_find(s, open, 0)) != std::string::npos) {
            size_t e = ci_find(s, close, p);
            if (e == std::string::npos) { s.erase(p); break; }
            s.erase(p, e - p + close.size());
        }
    };
    strip_block(h, "script");
    strip_block(h, "style");

    std::string out;
    bool in_tag = false;
    for (char ch : h) {
        if (ch == '<') { in_tag = true; continue; }
        if (ch == '>') { in_tag = false; out += ' '; continue; }
        if (!in_tag) out += ch;
    }

    auto rep_all = [&](const std::string& a, const std::string& b) {
        size_t p = 0;
        while ((p = out.find(a, p)) != std::string::npos) { out.replace(p, a.size(), b); p += b.size(); }
    };
    rep_all("&amp;", "&"); rep_all("&lt;", "<"); rep_all("&gt;", ">");
    rep_all("&quot;", std::string(1, '"')); rep_all("&#39;", "'"); rep_all("&nbsp;", " ");

    std::string collapsed;
    bool ws = false;
    for (char ch : out) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            if (!ws) { collapsed += ' '; ws = true; }
        } else { collapsed += ch; ws = false; }
    }
    size_t a = collapsed.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t z = collapsed.find_last_not_of(' ');
    return collapsed.substr(a, z - a + 1);
}

std::string Agent::web_fetch(const std::string& url) {
    if (!web_enabled_.load()) return "error: web fetch is disabled";
    if (url.empty()) return "error: empty url";
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return "error: url must start with http:// or https://";

    CURL* curl = curl_easy_init();
    if (!curl) return "error: curl init failed";
    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; egodeath/1.0)");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    CURLcode rc = curl_easy_perform(curl);
    long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    char* ct = nullptr; curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    std::string ctype = ct ? ct : "";
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) return std::string("error: request failed: ") + curl_easy_strerror(rc);
    if (code >= 400) return "error: HTTP " + std::to_string(code);

    std::string text;
    if (ctype.find("text/html") != std::string::npos || ctype.empty())
        text = _html_to_text(resp);
    else if (ctype.find("text/") != std::string::npos || ctype.find("json") != std::string::npos ||
             ctype.find("xml") != std::string::npos)
        text = resp;
    else
        return "error: unsupported content-type: " + ctype;

    const size_t cap = 20000;
    if (text.size() > cap)
        text = text.substr(0, cap) + "\n... [truncated, " + std::to_string(text.size()) + " chars total]";
    return text.empty() ? "(no readable text)" : text;
}

void Agent::checkpoint_file(const std::filesystem::path& p) {
    if (p.empty()) return;
    FileCheckpoint cp; cp.path = p;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        cp.prior = ss.str();
        cp.existed = true;
    }
    std::lock_guard<std::mutex> lock(checkpoints_mtx_);
    checkpoints_.push_back(std::move(cp));
    if (checkpoints_.size() > 50) checkpoints_.erase(checkpoints_.begin());
}

std::string Agent::undo_last_edit() {
    FileCheckpoint cp;
    {
        std::lock_guard<std::mutex> lock(checkpoints_mtx_);
        if (checkpoints_.empty()) return "nothing to undo";
        cp = std::move(checkpoints_.back());
        checkpoints_.pop_back();
    }
    std::error_code ec;
    if (cp.existed) {
        std::ofstream out(cp.path, std::ios::binary);
        if (!out) return "undo failed: cannot write " + cp.path.string();
        out << cp.prior;
        return "undid edit to " + cp.path.filename().string() + " (" + std::to_string(cp.prior.size()) + " bytes restored)";
    }
    std::filesystem::remove(cp.path, ec);
    return "undid creation of " + cp.path.filename().string() + " (file removed)";
}

void Agent::maybe_compress_history() {
    if (ctx_size_ <= 0) return;

    int est = 0, n = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        n = (int)history.size();
        for (const auto& msg : history) est += (int)(msg.dump().size() / 4);
    }
    if (est < (int)(compact_at_ * ctx_size_) || n < 12) return;

    // Pick a cut that keeps the system message plus a recent tail beginning on a
    // user turn, so no tool message is orphaned from its assistant tool_calls.
    int cut;
    std::string transcript;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cut = (int)history.size() - 8;
        if (cut < 2) return;
        while (cut < (int)history.size() && history[cut].value("role", std::string()) != "user") cut++;
        if (cut >= (int)history.size()) return;
        for (int i = 1; i < cut; ++i) {
            std::string role = history[i].value("role", std::string());
            std::string body = (history[i].contains("content") && history[i]["content"].is_string())
                                   ? history[i]["content"].get<std::string>() : std::string();
            if (body.size() > 1500) body = body.substr(0, 1500) + " ...";
            if (!body.empty()) transcript += role + ": " + body + "\n";
        }
    }
    if (transcript.empty()) return;

    tui_.set_status(StatusType::PROCESSING, "compressing context...");

    json sum_msgs = json::array();
    sum_msgs.push_back({{"role", "system"}, {"content",
        "You compress conversations. Summarize the following exchange concisely, "
        "preserving decisions made, important facts, file paths, and any unfinished "
        "tasks. Output only the summary."}});
    sum_msgs.push_back({{"role", "user"}, {"content", transcript}});

    json resp = client_.chat(sum_msgs, std::nullopt);
    std::string summary;
    try {
        if (resp.contains("choices") && resp["choices"].is_array() && !resp["choices"].empty())
            summary = resp["choices"][0]["message"].value("content", std::string());
    } catch (...) {}
    if (summary.empty()) return;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (cut >= (int)history.size()) return; // history changed under us; abort
        json nh = json::array();
        nh.push_back(history[0]);
        nh.push_back({{"role", "user"}, {"content", "[Summary of earlier conversation]\n" + summary}});
        nh.push_back({{"role", "assistant"}, {"content", "Understood -- continuing from the summary above."}});
        for (int i = cut; i < (int)history.size(); ++i) nh.push_back(history[i]);
        history = nh;
    }
    {
        std::lock_guard<std::mutex> lock(ui_queue_mtx_);
        UIEvent ev; ev.type = UIEvent::Type::TOOL_DISPLAY;
        ev.content = "\u26a0 context compressed -- summarized " + std::to_string(cut - 1) + " earlier messages";
        ui_queue_.push(ev);
    }
    ui_queue_cv_.notify_one();
}

void Agent::turn_async(int depth) {
    if (depth >= 16 || !running_.load()) { 
        tui_.set_activity("idle"); 
        return; 
    }
    if (depth == 0) turn_running_.store(true);
    if (depth == 0) maybe_compress_history();
    

    
    std::string content;
    json tool_calls = json::array();
    bool is_first = true;
    
    json all_tools = tools_.schema();
    if (!shell_enabled_.load()) {
        json _filt = json::array();
        for (auto& _t : all_tools)
            if (!(_t.contains("function") && _t["function"].value("name", std::string()) == "exec_shell"))
                _filt.push_back(_t);
        all_tools = _filt;
    }
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"save_memory","description":"Persist a fact to long-term memory across sessions.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Short unique slug"},"description":{"type":"string","description":"One-line summary of what this memory contains"},"fact":{"type":"string","description":"The content to store"},"type":{"type":"string","description":"Category: user, project, feedback, or reference"},"scope":{"type":"string","description":"global or project, defaults to global"}},"required":["name","description","fact"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"recall_memory","description":"Retrieve memories. Use an empty query string to get ALL memories. Pass keywords to filter by name or description.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Keyword filter. Empty string returns all memories."},"scope":{"type":"string","description":"all, global, or project. defaults to all"}}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"remove_memory","description":"Delete a memory entry by its name slug.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"The name slug of the memory to delete"}},"required":["name"]}}})SCHEMA"));
    if (web_enabled_.load()) {
        all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"web_search","description":"Search the web via a local SearXNG instance. Returns ranked results with titles, URLs, and snippets.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Search query"},"max_results":{"type":"integer","description":"How many results to return (default 5, max 15)"}},"required":["query"]}}})SCHEMA"));
        all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"web_fetch","description":"Fetch a URL and return its readable text content (HTML stripped). Use after web_search to read a result page.","parameters":{"type":"object","properties":{"url":{"type":"string","description":"Absolute http(s) URL to fetch"}},"required":["url"]}}})SCHEMA"));
    }
    for (auto& _mt : mcp_.all_tool_schemas()) all_tools.push_back(_mt);
    auto stream_start = std::chrono::steady_clock::now();
    int gen_token_count = 0;
    auto last_metrics_push = stream_start;

    // Rough prompt-token estimate for live ctx display (1 token ≈ 4 bytes of serialised JSON)
    int est_prompt_tokens = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& msg : history)
            est_prompt_tokens += (int)(msg.dump().size() / 4);
    }

    client_.chat_stream(history, all_tools, [this, &content, &tool_calls, &is_first,
                                              &stream_start, &gen_token_count, &last_metrics_push,
                                              &est_prompt_tokens](const UIEvent& ev) {
        // Enqueue UI events for processing on the main thread
        {
            std::lock_guard<std::mutex> lock(ui_queue_mtx_);
            
            if (ev.type == UIEvent::Type::STREAM_CONTENT) {
                if (is_first) {
                    ui_queue_.push({UIEvent::Type::STREAM_START, ""});
                    is_first = false;
                    stream_start = std::chrono::steady_clock::now();
                    last_metrics_push = stream_start;
                }
                if (!ev.content.empty()) {
                    ui_queue_.push({UIEvent::Type::STREAM_CONTENT, ev.content});
                    gen_token_count++;
                    auto now = std::chrono::steady_clock::now();
                    auto ms_since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_metrics_push).count();
                    if (ms_since >= 500) {
                        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - stream_start).count();
                        if (total_ms > 0) {
                            UIEvent mev;
                            mev.type = UIEvent::Type::METRICS_UPDATE;
                            mev.timings = {
                                {"predicted_per_second", gen_token_count * 1000.0 / total_ms},
                                {"ctx_used_estimate", est_prompt_tokens + gen_token_count}
                            };
                            ui_queue_.push(mev);
                        }
                        last_metrics_push = now;
                    }
                }
                content += ev.content;
            } else if (ev.type == UIEvent::Type::STREAM_REASONING) {
                ui_queue_.push({UIEvent::Type::STREAM_REASONING, ev.content});
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
            } else if (ev.type == UIEvent::Type::STREAM_END) {
                ui_queue_.push({ev});
            }
        }
        ui_queue_cv_.notify_one();
    }, &cancel_requested_);

    // Abort if cancelled mid-stream
    if (cancel_requested_.load()) {
        cancel_requested_.store(false);
        {
            std::lock_guard<std::mutex> lock(ui_queue_mtx_);
            UIEvent cev; cev.type = UIEvent::Type::TOOL_DISPLAY;
            cev.content = "\xe2\x8a\x98 generation cancelled";
            ui_queue_.push(cev);
        }
        ui_queue_cv_.notify_one();
        if (depth == 0) turn_running_.store(false);
        tui_.set_activity("idle");
        return;
    }

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
                
                std::lock_guard<std::mutex> lock(ui_queue_mtx_);
                UIEvent _de; _de.type = UIEvent::Type::TOOL_DISPLAY; _de.content = "\u26a0 " + fname + "  (malformed — dropped)"; ui_queue_.push(_de);
                ui_queue_cv_.notify_one();
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
                json args;
                try {
                    args = json::parse(arg_str);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(ui_queue_mtx_);
                    UIEvent _pe; _pe.type = UIEvent::Type::TOOL_DISPLAY; _pe.content = "\u26a0 " + name + "  (argument parse error — dropped)"; ui_queue_.push(_pe);
                    ui_queue_cv_.notify_one();
                    continue;
                }

                // Per-tool approval gate for mutating tools.
                bool _denied = false;
                if (((name == "exec_shell" && shell_enabled_.load()) || name == "write_file" || name == "edit_file" || mcp_.owns(name)) && !auto_approve_.load()) {
                    std::string _aarg;
                    for (const auto& [_k, _v] : args.items()) {
                        if (_k == "command" || _k == "filepath" || _k == "path") {
                            _aarg = _v.is_string() ? _v.get<std::string>() : _v.dump();
                            break;
                        }
                    }
                    if (_aarg.size() > 80) _aarg = _aarg.substr(0, 77) + "...";
                    if (name == "write_file" || name == "edit_file") {
                        std::string _diff = tools_.preview(name, args);
                        if (!_diff.empty()) {
                            std::lock_guard<std::mutex> _dl(ui_queue_mtx_);
                            UIEvent _dv; _dv.type = UIEvent::Type::DIFF_DISPLAY; _dv.content = _diff;
                            ui_queue_.push(_dv); ui_queue_cv_.notify_one();
                        }
                    }
                    int _r = tui_.request_tool_approval("\u26a0 approve " + name + "?  " + _aarg + "   [y]es  [n]o  [a]lways");
                    if (_r == 2) auto_approve_.store(true);
                    if (_r == 0) _denied = true;
                }

                std::string res;
                if (_denied) {
                    res = "[tool call denied by user]";
                } else if (name == "save_memory") {
                    memory_.save(args.value("name",""), args.value("description",""), args.value("fact",""), args.value("type","unknown"), args.value("scope","global"));
                    res = "memory saved";
                } else if (name == "recall_memory") {
                    std::string rq = args.value("query","");
                    // Empty query, or wildcard phrases, return everything
                    if (rq == "*" || rq == "all" || rq == "all memories" ||
                        rq == "everything" || rq == "any")
                        rq = "";
                    res = memory_.recall(rq, args.value("scope","all"));
                    if (res.empty()) res = "no memories found";
                } else if (name == "remove_memory") {
                    res = memory_.remove(args.value("name","")) ? "memory removed" : "memory not found";
                } else if (name == "web_search") {
                    res = web_search(args.value("query", ""), args.value("max_results", 5));
                } else if (name == "web_fetch") {
                    res = web_fetch(args.value("url", ""));
                } else if (name == "write_file") {
                    checkpoint_file(args.value("filepath", ""));
                    res = tools_.dispatch(name, args);
                } else if (name == "edit_file") {
                    checkpoint_file(args.value("filepath", ""));
                    res = tools_.dispatch(name, args);
                } else if (name == "exec_shell" && !shell_enabled_.load()) {
                    res = "error: shell access is disabled (the user can enable it with /shell on)";
                } else if (mcp_.owns(name)) {
                    res = mcp_.call(name, args);
                } else {
                    res = tools_.dispatch(name, args);
                }
                
                // Format tool call as a compact display block
                std::string _key_arg;
                for (const auto& [_k, _v] : args.items()) {
                    if (_k == "filepath" || _k == "command" || _k == "pattern" ||
                        _k == "path" || _k == "query" || _k == "name" || _k == "url") {
                        _key_arg = _v.is_string() ? _v.get<std::string>() : _v.dump();
                        break;
                    }
                }
                if (_key_arg.size() > 55) _key_arg = _key_arg.substr(0, 52) + "...";
                std::string _header = "\u25b8 " + name + (_key_arg.empty() ? "" : "  " + _key_arg);
                // Result preview: first non-empty line + size hint if multi-line
                size_t _nl = res.find('\n');
                std::string _first = (_nl != std::string::npos) ? res.substr(0, _nl) : res;
                if (_first.size() > 120) _first = _first.substr(0, 117) + "...";
                size_t _lines = 1; for (char _c : res) if (_c == '\n') _lines++;
                std::string _body = "  " + (_first.empty() ? "(empty)" : _first);
                if (_lines > 1 || res.size() > 120)
                    _body += "\n  \u2026 " + std::to_string(res.size()) + " chars  " + std::to_string(_lines) + " lines";
                std::string _tool_display = _header + "\n" + _body;

                std::lock_guard<std::mutex> lock(ui_queue_mtx_);
                UIEvent _td; _td.type = UIEvent::Type::TOOL_DISPLAY; _td.content = _tool_display; ui_queue_.push(_td);

                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    history.push_back({
                        {"role", "tool"},
                        {"tool_call_id", id},
                        {"name", name},
                        {"content", res}
                    });
                }
                ui_queue_cv_.notify_one();
            }
            
            turn_async(depth + 1);
        } else {
            std::lock_guard<std::mutex> lock(ui_queue_mtx_);
            UIEvent _ie; _ie.type = UIEvent::Type::TOOL_DISPLAY; _ie.content = "\u26a0 all tool calls invalid"; ui_queue_.push(_ie);
            ui_queue_cv_.notify_one();
        }
    } else {
        if (!content.empty()) {
            std::lock_guard<std::mutex> lock(mtx_);
            history.push_back(assistant_msg);
        }
    }
    
    if (depth == 0) turn_running_.store(false);
    tui_.set_activity("idle");
}

void Agent::save_session(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ofstream out(path);
    if (out) out << history.dump(2);
}

void Agent::load_session(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        json loaded = json::parse(content);
        if (loaded.is_array()) {
            { std::lock_guard<std::mutex> lock(mtx_); history = loaded; }
            rebuild_tui_from_history();
        }
    } catch (...) {}
}

void Agent::rebuild_tui_from_history() {
    tui_.clear_history();
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& msg : history) {
        std::string role = msg.value("role", std::string());
        if (role == "system") continue;
        if (role == "user") {
            if (msg.contains("content") && msg["content"].is_string()) {
                std::string c = msg["content"].get<std::string>();
                if (!c.empty()) tui_.append_history("", c, "user");
            }
        } else if (role == "assistant") {
            if (msg.contains("content") && msg["content"].is_string() && !msg["content"].get<std::string>().empty())
                tui_.append_history("", msg["content"].get<std::string>(), "assistant");
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                for (const auto& call : msg["tool_calls"]) {
                    if (!call.contains("function") || !call["function"].is_object()) continue;
                    std::string name = call["function"].value("name", std::string("?"));
                    std::string keyarg;
                    try {
                        json a = json::parse(call["function"].value("arguments", std::string("{}")));
                        for (const auto& [k, v] : a.items()) {
                            if (k == "command" || k == "filepath" || k == "path" ||
                                k == "pattern" || k == "query" || k == "name" || k == "url") {
                                keyarg = v.is_string() ? v.get<std::string>() : v.dump();
                                break;
                            }
                        }
                    } catch (...) {}
                    if (keyarg.size() > 55) keyarg = keyarg.substr(0, 52) + "...";
                    tui_.append_history("", "\u25b8 " + name + (keyarg.empty() ? "" : "  " + keyarg), "tool");
                }
            }
        } else if (role == "tool") {
            std::string c = (msg.contains("content") && msg["content"].is_string()) ? msg["content"].get<std::string>() : std::string();
            size_t nl = c.find('\n');
            std::string first = (nl != std::string::npos) ? c.substr(0, nl) : c;
            if (first.size() > 120) first = first.substr(0, 117) + "...";
            tui_.append_history("", "  " + (first.empty() ? "(empty)" : first), "tool");
        }
    }
}

int Agent::get_context_size() const {
    return client_.get_context_size();
}

void Agent::cancel() { cancel_requested_.store(true); }

} // namespace egodeath
