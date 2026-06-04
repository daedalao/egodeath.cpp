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
        // No built-in fallback any more: the soul (below) is the system prompt.
        // Any project egodeath.md becomes an optional layer beneath it.
    }
    project_instructions_ = base_prompt;

    // Resolve the XDG config dir up front: the soul and the stores all live here.
    {
        namespace fs = std::filesystem;
        config_dir_ = std::getenv("XDG_CONFIG_HOME")
            ? fs::path(std::getenv("XDG_CONFIG_HOME")) / "egodeath"
            : fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".config" / "egodeath";
    }

    // Load the soul — the agent's renameable identity and core system prompt. On
    // first run it is seeded from the built-in default (with the name templatized
    // as {name} so a rename propagates), then it is fully user-owned via soul.md.
    first_boot_ = soul_.load(config_dir_ / "soul.md", "egodeath", DEFAULT_SYSTEM_PROMPT);
    user_info_path_ = config_dir_ / "user.md";
    {
        std::ifstream uf(user_info_path_);
        if (uf) {
            std::stringstream ss; ss << uf.rdbuf();
            user_info_ = ss.str();
            if (user_info_.size() > kUserInfoCap) user_info_.resize(kUserInfoCap);
        }
    }
    rebuild_system_prompt();

    // Open the SQLite task/calendar store under the XDG config dir.
    {
        namespace fs = std::filesystem;
        store_ = std::make_unique<Store>(config_dir_ / "egodeath.db");
        std::error_code _pec;
        project_key_ = fs::weakly_canonical(fs::current_path(), _pec).string();
        if (_pec) project_key_ = fs::current_path().string();
        custom_tools_.load(config_dir_ / "custom_tools.json", project_key_);
    }

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
    
    shell_jobs_.shutdown();
    clear_sudo_cache();
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

void Agent::run_step(const std::string& input, const std::string& display_override, bool silent) {
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

    // Show in chat with appropriate styling (suppressed for silent kickoffs).
    if (!silent) {
        std::lock_guard<std::mutex> lock(ui_queue_mtx_);
        UIEvent ev;
        ev.type = is_queued ? UIEvent::Type::USER_INPUT_QUEUED : UIEvent::Type::USER_INPUT;
        ev.content = display;
        ui_queue_.push(ev);
        ui_queue_cv_.notify_one();
    }

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
            // Refresh the system prompt (soul + memory index) so renames and new
            // memories take effect live, without a restart.
            rebuild_system_prompt();
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
                    tui_.append_history(soul_.name(), "", "assistant");
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

// Validate a tool call against the schema we actually advertised to the model.
// Returns an empty string if valid, otherwise a precise error to hand back as the
// tool result so the model self-corrects on the next turn (instead of a silent drop).
static long long json_id(const json& a) {
    if (!a.contains("id")) return 0;
    const auto& v = a["id"];
    if (v.is_number_integer()) return v.get<long long>();
    if (v.is_string()) { try { return std::stoll(v.get<std::string>()); } catch (...) {} }
    return 0;
}

static std::string validate_tool_call(const json& all_tools, const std::string& name, const json& args) {
    const json* fn = nullptr;
    for (const auto& t : all_tools)
        if (t.contains("function") && t["function"].value("name", std::string()) == name) {
            fn = &t["function"]; break;
        }
    if (!fn)
        return "error: unknown tool '" + name + "'. Only call tools from the provided list.";
    if (fn->contains("parameters") && (*fn)["parameters"].contains("required") &&
        (*fn)["parameters"]["required"].is_array()) {
        std::vector<std::string> missing;
        for (const auto& r : (*fn)["parameters"]["required"]) {
            if (!r.is_string()) continue;
            std::string key = r.get<std::string>();
            bool absent = !args.contains(key) || args[key].is_null() ||
                          (args[key].is_string() && args[key].get<std::string>().empty());
            if (absent) missing.push_back(key);
        }
        if (!missing.empty()) {
            std::string m;
            for (size_t i = 0; i < missing.size(); ++i) { if (i) m += ", "; m += "'" + missing[i] + "'"; }
            return "error: missing required argument(s): " + m +
                   ". Re-issue the call with every required field set.";
        }
    }
    return "";
}

json Agent::read_config() {
    std::ifstream f(config_dir_ / "config.json");
    if (!f) return json::object();
    try { std::stringstream ss; ss << f.rdbuf(); json j = json::parse(ss.str()); return j.is_object() ? j : json::object(); }
    catch (...) { return json::object(); }
}

std::string Agent::write_config(const json& cfg) {
    std::error_code ec; std::filesystem::create_directories(config_dir_, ec);
    std::ofstream o(config_dir_ / "config.json");
    if (!o) return "cannot write config.json";
    o << cfg.dump(2) << "\n";
    return "";
}

std::string Agent::last_written_file() {
    std::lock_guard<std::mutex> lk(last_file_mtx_);
    return last_file_;
}

std::string Agent::editor_save(const std::string& path, const std::string& content) {
    checkpoint_file(path);
    std::ofstream out(path, std::ios::binary);
    if (!out) return "cannot open " + path + " for writing";
    out << content;
    out.close();
    { std::lock_guard<std::mutex> lk(last_file_mtx_); last_file_ = path; }
    // If the soul was edited in the in-app editor, reload it live.
    {
        std::error_code ec;
        if (std::filesystem::weakly_canonical(path, ec) ==
            std::filesystem::weakly_canonical(soul_.path(), ec)) {
            soul_.reload();
            rebuild_system_prompt();
        }
    }
    return "";
}

void Agent::rebuild_system_prompt() {
    // The soul is the entire system prompt; the user profile and the memory index
    // ride along as the agent's running notes and recall of what it already knows.
    std::string prompt = soul_.prompt();
    {
        std::lock_guard<std::mutex> ulk(user_info_mtx_);
        if (!user_info_.empty())
            prompt += "\n\n# About the user (your own running notes)\n" + user_info_;
    }
    prompt += memory_.system_prompt_addendum();
    if (store_) {
        auto cmds = store_->top_commands(3, false, {std::string(), project_key_}, 5);
        if (!cmds.empty()) {
            prompt += "\n\n# Observed repeated commands\n"
                      "The user (or you) has run these several times. When it fits, offer via ask_user "
                      "to crystallize one into a reusable tool (define_tool) or a memory; once you act on "
                      "it or the user declines, call dismiss_pattern so it stops being suggested.\n";
            for (auto& c : cmds)
                prompt += "- " + c.raw + "  Ã" + std::to_string(c.count) + " (" + c.source + ")\n";
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    // Build the system message explicitly: insert(pos, {...}) would bind to the
    // initializer-list overload and splice ["role","system"] in as an array.
    json sysmsg = json::object();
    sysmsg["role"] = "system";
    sysmsg["content"] = prompt;
    if (!history.empty() && history[0].is_object() &&
        history[0].value("role", std::string()) == "system")
        history[0] = sysmsg;
    else
        history.insert(history.begin(), sysmsg);
}

std::string Agent::soul_name() { return soul_.name(); }
std::string Agent::soul_path() { return soul_.path().string(); }
std::string Agent::set_soul_name(const std::string& n) {
    // Treat the given name as the formal name and go by a short handle derived
    // from it (e.g. "Sir James Edelbrock the Eighteenth" -> "James").
    soul_.set_full_name(n);
    soul_.set_name(Soul::derive_short(n));
    rebuild_system_prompt();
    return soul_.name();
}

std::string Agent::soul_full_name() { return soul_.full_name(); }

std::string Agent::sudo_password() {
    {
        std::lock_guard<std::mutex> lk(sudo_mtx_);
        if (!sudo_pw_.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - sudo_pw_time_).count();
            if (age < sudo_ttl_secs_) return sudo_pw_;
            std::fill(sudo_pw_.begin(), sudo_pw_.end(), '\0');
            sudo_pw_.clear();
        }
    }
    const char* u = std::getenv("USER");
    std::string pw = tui_.request_password(
        std::string("\xf0\x9f\x94\x92 sudo password for ") + (u ? u : "root") + "  (Enter to submit, Esc to cancel):");
    if (pw.empty()) return "";
    std::lock_guard<std::mutex> lk(sudo_mtx_);
    sudo_pw_ = pw;
    sudo_pw_time_ = std::chrono::steady_clock::now();
    return sudo_pw_;
}

void Agent::observe_command(const std::string& cmd, const std::string& source) {
    if (store_ && !cmd.empty()) store_->record_command(cmd, source, project_key_);
}

std::string Agent::observed_commands_text() {
    if (!store_) return "(no command store)";
    auto cmds = store_->top_commands(1, true, {std::string(), project_key_}, 50);
    if (cmds.empty()) return "no commands observed yet";
    std::string out = "Observed commands (count Ã, source, dismissed):\n";
    for (auto& c : cmds)
        out += fmt::format("  Ã{:<3} {:<6} {} {}\n", c.count, c.source,
                           c.dismissed ? "[dismissed]" : "          ", c.raw);
    return out;
}

void Agent::clear_sudo_cache() {
    std::lock_guard<std::mutex> lk(sudo_mtx_);
    std::fill(sudo_pw_.begin(), sudo_pw_.end(), '\0');
    sudo_pw_.clear();
}

std::string Agent::get_user_info() {
    std::lock_guard<std::mutex> lk(user_info_mtx_);
    return user_info_.empty() ? "(no user profile yet)" : user_info_;
}

std::string Agent::set_user_info(const std::string& content) {
    if (content.size() > kUserInfoCap)
        return "rejected: user info is " + std::to_string(content.size()) +
               " chars but the hard limit is " + std::to_string(kUserInfoCap) +
               ". Decide what matters most about the user and compress it to fit.";
    {
        std::lock_guard<std::mutex> lk(user_info_mtx_);
        user_info_ = content;
        std::error_code ec;
        std::filesystem::create_directories(user_info_path_.parent_path(), ec);
        std::filesystem::path tmp = user_info_path_; tmp += ".tmp";
        { std::ofstream out(tmp, std::ios::trunc); if (out) out << content; }
        std::filesystem::rename(tmp, user_info_path_, ec);
    }
    rebuild_system_prompt();
    return "saved (" + std::to_string(content.size()) + "/" + std::to_string(kUserInfoCap) + " chars)";
}

void Agent::boot_greeting() {
    if (!first_boot_) return;
    first_boot_ = false;
    // A hidden first turn so a freshly-born agent speaks before the user types.
    // The awakening guidance lives in its own (non-core) soul section, so it is
    // injected here exactly once instead of riding in the prompt every turn.
    std::string msg = "(You have just been instantiated for the very first time on this machine. "
                      "You have no memories yet \u2014 this is your first breath.";
    std::string awk = soul_.section("awakening");
    if (!awk.empty()) msg += " Follow this, your awakening:\n\n" + awk;
    msg += "\n\nBegin.)";
    run_step(msg, "", /*silent=*/true);
}

json Agent::agenda_snapshot() {
    json arr = json::array();
    if (!store_) return arr;
    for (auto& it : store_->list("all", "all", "", "", 200, {std::string(), project_key_})) arr.push_back(it.to_json());
    return arr;
}

std::string Agent::agenda_action(const std::string& op, long long id, const std::string& arg) {
    if (!store_) return "store unavailable";
    std::string e;
    if (op == "toggle") {
        auto it = store_->get(id);
        if (!it) return "no item #" + std::to_string(id);
        return store_->set_status(id, it->status == "done" ? "open" : "done", e) ? "" : e;
    }
    if (op == "delete") return store_->remove(id, e) ? "" : e;
    if (op == "add") {
        Item t; t.kind = "task"; t.title = arg; t.project = project_key_;
        return store_->add(t, e) > 0 ? "" : e;
    }
    return "unknown op";
}

json Agent::memory_snapshot() {
    json arr = json::array();
    for (auto& j : memory_.list_entries()) arr.push_back(j);
    return arr;
}
std::string Agent::memory_action(const std::string& op, const std::string& name, const std::string& arg) {
    if (op == "delete")   return memory_.remove(name) ? "" : "not found";
    if (op == "pin")      return memory_.set_pinned(name, true) ? "" : "not found";
    if (op == "unpin")    return memory_.set_pinned(name, false) ? "" : "not found";
    if (op == "category") return memory_.set_category(name, arg) ? "" : "not found";
    return "unknown op";
}

std::string Agent::run_custom_tool(const std::string& name, const json& args, int rdepth) {
    auto od = custom_tools_.get(name);
    if (!od) return "error: no such custom tool";
    const auto& d = *od;
    // Replace every {param} placeholder with the supplied (or empty) argument.
    auto subst = [&](std::string s) {
        for (const auto& p : d.params) {
            std::string ph = "{" + p.name + "}";
            std::string val;
            if (args.contains(p.name))
                val = args[p.name].is_string() ? args[p.name].get<std::string>() : args[p.name].dump();
            size_t pos = 0;
            while ((pos = s.find(ph, pos)) != std::string::npos) { s.replace(pos, ph.size(), val); pos += val.size(); }
        }
        return s;
    };
    if (d.kind == "template") {
        return tools_.dispatch("exec_shell", json{{"command", subst(d.command)}});
    }
    // recipe: run each step through the built-in dispatcher (or a nested custom tool).
    if (rdepth > 4) return "error: recipe nesting too deep";
    std::string out;
    int i = 0;
    for (const auto& step : d.steps) {
        ++i;
        std::string stool = step.value("tool", "");
        json sargs = (step.contains("args") && step["args"].is_object()) ? step["args"] : json::object();
        for (auto& [k, v] : sargs.items())
            if (v.is_string()) v = subst(v.get<std::string>());
        std::string sres;
        if (stool.empty()) sres = "error: step missing 'tool'";
        else if (CustomTools::is_reserved(stool) || !custom_tools_.has(stool)) sres = tools_.dispatch(stool, sargs);
        else sres = run_custom_tool(stool, sargs, rdepth + 1);
        out += "step " + std::to_string(i) + " [" + stool + "]: " + sres + "\n";
    }
    return out.empty() ? "(recipe produced no output)" : out;
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
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"save_memory","description":"Persist a fact to long-term memory across sessions. Categorize it so the memory index stays organized; pin only durable, high-value facts. Low-value memories are auto-evicted once a scope fills up.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Short unique slug"},"description":{"type":"string","description":"One-line summary of what this memory contains"},"fact":{"type":"string","description":"The content to store"},"category":{"type":"string","description":"A short category label, e.g. user, project, feedback, reference, preference, person, snippet"},"pinned":{"type":"boolean","description":"true to protect this memory from auto-eviction (use sparingly for durable facts)"},"scope":{"type":"string","description":"global or project, defaults to global"}},"required":["name","description","fact"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"categorize_memory","description":"Re-categorize or pin/unpin an existing memory by name. Use this to keep the memory index tidy.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"The memory slug"},"category":{"type":"string","description":"New category label (omit to leave unchanged)"},"pinned":{"type":"boolean","description":"Pin or unpin (omit to leave unchanged)"}},"required":["name"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"recall_memory","description":"Retrieve memories. Use an empty query string to get ALL memories. Pass keywords to filter by name or description.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Keyword filter. Empty string returns all memories."},"scope":{"type":"string","description":"all, global, or project. defaults to all"}}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"remove_memory","description":"Delete a memory entry by its name slug.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"The name slug of the memory to delete"}},"required":["name"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"add_task","description":"Add a task to the task/calendar store.","parameters":{"type":"object","properties":{"title":{"type":"string"},"notes":{"type":"string"},"priority":{"type":"string","description":"low, med, or high"},"due":{"type":"string","description":"Due date YYYY-MM-DD (optional)"},"scope":{"type":"string","description":"project (this directory, default) or global"}},"required":["title"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"add_event","description":"Add a calendar event with a start time (and optional end) to the store.","parameters":{"type":"object","properties":{"title":{"type":"string"},"start":{"type":"string","description":"Start as YYYY-MM-DD or YYYY-MM-DDTHH:MM"},"end":{"type":"string","description":"End (optional)"},"notes":{"type":"string"},"scope":{"type":"string","description":"project (this directory, default) or global"}},"required":["title","start"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"list_items","description":"List tasks and/or events from the store. Defaults to open items in this directory plus global.","parameters":{"type":"object","properties":{"kind":{"type":"string","description":"task, event, or all (default all)"},"status":{"type":"string","description":"open, done, cancelled, or all (default open)"},"from":{"type":"string","description":"Earliest date YYYY-MM-DD (optional)"},"to":{"type":"string","description":"Latest date YYYY-MM-DD (optional)"},"scope":{"type":"string","description":"project (this directory), global, or all (default all)"}}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"complete_item","description":"Mark a task or event done by its numeric id.","parameters":{"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"remove_item","description":"Delete a task or event by its numeric id.","parameters":{"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"ask_user","description":"Ask the user a multiple-choice question and wait for their pick. Use it to get guidance or confirm a direction, especially while building something. Give 2-5 short, distinct options.","parameters":{"type":"object","properties":{"question":{"type":"string"},"options":{"type":"array","items":{"type":"string"},"description":"2 to 5 short options to choose from"}},"required":["question","options"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"open_editor","description":"Open a file in the in-app code editor so the user can view or edit it. The pane appears in the user\u0027s TUI.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path to open"}},"required":["path"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"get_settings","description":"Return the user\u0027s persisted settings from config.json.","parameters":{"type":"object","properties":{}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"set_setting","description":"Change one setting and persist it to config.json. Live-applies theme, editor_dock, web_search, shell, auto_approve, reasoning_effort, compact_at; endpoint/model/ctx take effect on restart.","parameters":{"type":"object","properties":{"key":{"type":"string","description":"Setting name, e.g. theme, editor_dock, web_search, shell, auto_approve, reasoning_effort, compact_at"},"value":{"description":"New value (string, boolean, or number depending on the setting)"}},"required":["key","value"]}}})SCHEMA"));
    if (web_enabled_.load()) {
        all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"web_search","description":"Search the web via a local SearXNG instance. Returns ranked results with titles, URLs, and snippets.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Search query"},"max_results":{"type":"integer","description":"How many results to return (default 5, max 15)"}},"required":["query"]}}})SCHEMA"));
        all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"web_fetch","description":"Fetch a URL and return its readable text content (HTML stripped). Use after web_search to read a result page.","parameters":{"type":"object","properties":{"url":{"type":"string","description":"Absolute http(s) URL to fetch"}},"required":["url"]}}})SCHEMA"));
    }
    for (auto& _mt : mcp_.all_tool_schemas()) all_tools.push_back(_mt);
    for (auto& _ct : custom_tools_.schemas()) all_tools.push_back(_ct);
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"define_tool","description":"Create or update a reusable custom tool so you stop hand-writing the same shell every time. kind=template is a shell command with {param} placeholders; kind=recipe is an ordered list of steps, each {\"tool\":name,\"args\":{...}} calling an existing tool (read_file, edit_file, exec_shell, …) or another custom tool, with {param} placeholders allowed in string args. The tool becomes callable by name on the next turn and persists across sessions.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Unique snake_case name; must not be a built-in tool name"},"description":{"type":"string"},"kind":{"type":"string","description":"template or recipe"},"command":{"type":"string","description":"For kind=template: the shell command, using {param} placeholders"},"steps":{"type":"array","description":"For kind=recipe: ordered steps, each an object {tool, args}","items":{"type":"object"}},"params":{"type":"array","description":"Parameters this tool accepts","items":{"type":"object","properties":{"name":{"type":"string"},"description":{"type":"string"},"required":{"type":"boolean"}}}},"scope":{"type":"string","description":"global (default) or project"}},"required":["name","description","kind"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"list_tools","description":"List the custom tools you have defined, with kind, scope and use counts.","parameters":{"type":"object","properties":{}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"list_observed_commands","description":"List shell commands the user and you have run repeatedly (the learn-from-repetition log), with counts.","parameters":{"type":"object","properties":{}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"dismiss_pattern","description":"Stop suggesting a repeated command (mark it handled or unwanted) so it no longer appears under Observed repeated commands.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"The command to stop tracking"}},"required":["command"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"remove_tool","description":"Delete a custom tool by name.","parameters":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"get_soul","description":"Read your own soul: your current name and the body of your core identity / system prompt.","parameters":{"type":"object","properties":{}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"recall_soul","description":"Read one section of your own soul in full. Your prompt lists your soul sections by name; pass a name (e.g. editing, memory, tools, interaction, awakening, or any you have added) to load that section detail on demand.","parameters":{"type":"object","properties":{"section":{"type":"string","description":"The section name to read"}},"required":["section"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"get_user_info","description":"Read your compact, always-loaded profile of the user.","parameters":{"type":"object","properties":{}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"set_user_info","description":"Replace your always-loaded notes about the user with this content \u2014 everything about them worth keeping in front of you at all times. HARD LIMIT 1023 characters: content over the limit is rejected, so you must decide what is worth keeping and compress (rewrite the whole profile each time, do not just append). No approval is needed; maintain it freely as you learn about them.","parameters":{"type":"object","properties":{"content":{"type":"string","description":"The full user profile, <= 1023 characters"}},"required":["content"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"run_shell_background","description":"Start a shell command in the background and return immediately with a job id. Use this for long-running or non-terminating commands (servers, watchers, builds, training) instead of exec_shell, which blocks until the command finishes. Requires shell access.","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"run_sudo","description":"Run a shell command with root privileges. The user is asked to approve and to securely enter their password, which is fed to sudo over a pipe and is never shown to you, logged, put on the command line, or written to disk. Use this instead of putting sudo in exec_shell. Requires shell access.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"The command to run as root, without a leading sudo"}},"required":["command"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"job_output","description":"Read the latest combined stdout/stderr (tail) and running/exited status of a background job started with run_shell_background.","parameters":{"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"list_jobs","description":"List your background shell jobs with their status.","parameters":{"type":"object","properties":{}}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"stop_job","description":"Terminate a background shell job (and its process group) by id.","parameters":{"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]}}})SCHEMA"));
    all_tools.push_back(json::parse(R"SCHEMA({"type":"function","function":{"name":"set_soul","description":"Update your own soul \u2014 your core, always-loaded identity. Provide name to rename yourself and/or body to rewrite your guiding system prompt. Use {name} inside body to refer to your own name. This changes who you are, so do it deliberately and usually only when the user asks.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"New everyday/short name to go by (optional)"},"full_name":{"type":"string","description":"New formal/full name (optional); if you set this and omit name, a short everyday name is derived from it"},"body":{"type":"string","description":"New system-prompt body (optional); may use {name} (everyday) and {fullname} (formal)"}}}}})SCHEMA"));
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
                // A tool call's arguments must be a JSON object; some models emit an
                // array or scalar. Coerce to an empty object so dispatch stays safe.
                if (!args.is_object()) args = json::object();

                std::string _verr = validate_tool_call(all_tools, name, args);

                // Per-tool approval gate for mutating tools (skipped for invalid calls).
                bool _denied = false;
                if (_verr.empty() && ((name == "exec_shell" && shell_enabled_.load()) || name == "write_file" || name == "edit_file" || name == "set_setting" || name == "set_soul" || mcp_.owns(name) || (custom_tools_.has(name) && shell_enabled_.load()) || (name == "run_shell_background" && shell_enabled_.load()) || (name == "run_sudo" && shell_enabled_.load())) && !auto_approve_.load()) {
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
                try {
                if (!_verr.empty()) {
                    res = _verr;
                } else if (_denied) {
                    res = "[tool call denied by user]";
                } else if (name == "save_memory") {
                    std::string cat = args.value("category", args.value("type", "unknown"));
                    memory_.save(args.value("name",""), args.value("description",""), args.value("fact",""), cat, args.value("scope","global"), args.value("pinned", false));
                    res = "memory saved";
                } else if (name == "categorize_memory") {
                    std::string nm = args.value("name", "");
                    bool ok = false;
                    if (args.contains("category") && args["category"].is_string())
                        ok = memory_.set_category(nm, args["category"].get<std::string>());
                    if (args.contains("pinned") && args["pinned"].is_boolean())
                        ok = memory_.set_pinned(nm, args["pinned"].get<bool>()) || ok;
                    res = ok ? "memory updated" : "memory not found (or nothing to change)";
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
                } else if (name == "add_task") {
                    Item it; it.kind = "task"; it.title = args.value("title", "");
                    it.notes = args.value("notes", ""); it.priority = args.value("priority", "");
                    it.due = args.value("due", "");
                    it.project = args.value("scope", "project") == "global" ? "" : project_key_;
                    std::string e; long long id = store_ ? store_->add(it, e) : -1;
                    res = id > 0 ? fmt::format("task #{} added: {}", id, it.title) : "error: " + e;
                } else if (name == "add_event") {
                    Item it; it.kind = "event"; it.title = args.value("title", "");
                    it.notes = args.value("notes", ""); it.start_ts = args.value("start", "");
                    it.end_ts = args.value("end", "");
                    it.project = args.value("scope", "project") == "global" ? "" : project_key_;
                    std::string e; long long id = store_ ? store_->add(it, e) : -1;
                    res = id > 0 ? fmt::format("event #{} added: {}", id, it.title) : "error: " + e;
                } else if (name == "list_items") {
                    if (!store_) { res = "error: store unavailable"; }
                    else {
                        std::string sc = args.value("scope", "all");
                        std::vector<std::string> projs = sc == "global" ? std::vector<std::string>{std::string()}
                                                       : sc == "project" ? std::vector<std::string>{project_key_}
                                                       : std::vector<std::string>{std::string(), project_key_};
                        auto items = store_->list(args.value("kind", "all"), args.value("status", "open"),
                                                  args.value("from", ""), args.value("to", ""), 100, projs);
                        if (items.empty()) res = "(no items)";
                        else for (auto& it : items) res += it.one_line() + "\n";
                    }
                } else if (name == "complete_item") {
                    std::string e; bool ok = store_ && store_->set_status(json_id(args), "done", e);
                    res = ok ? "marked done" : "error: " + e;
                } else if (name == "remove_item") {
                    std::string e; bool ok = store_ && store_->remove(json_id(args), e);
                    res = ok ? "removed" : "error: " + e;
                } else if (name == "ask_user") {
                    std::string q = args.value("question", "");
                    std::vector<std::string> opts;
                    if (args.contains("options") && args["options"].is_array())
                        for (auto& o : args["options"]) {
                            if (o.is_string()) opts.push_back(o.get<std::string>());
                            else if (o.is_object() && o.contains("label") && o["label"].is_string()) opts.push_back(o["label"].get<std::string>());
                        }
                    if (q.empty() || opts.size() < 2) res = "error: ask_user needs 'question' and at least 2 string 'options'";
                    else {
                        int idx = tui_.ask_choice(q, opts);
                        res = idx < 0 ? "[user dismissed the question without choosing]" : "user selected: " + opts[idx];
                    }
                } else if (name == "open_editor") {
                    std::string p = args.value("path", "");
                    if (p.empty()) res = "error: path required";
                    else { tui_.request_open_editor(p); res = "opening editor for " + p + " (it appears in the user's TUI)"; }
                } else if (name == "get_settings") {
                    json cfg = read_config();
                    res = cfg.empty() ? "(no settings saved yet)" : cfg.dump(2);
                } else if (name == "set_setting") {
                    std::string key = args.value("key", "");
                    json val = args.contains("value") ? args["value"] : json();
                    auto is_key = [&](std::initializer_list<const char*> l) { for (auto* s : l) if (key == s) return true; return false; };
                    std::string err;
                    // Coerce/validate the value per setting so a stray type can't corrupt config.json.
                    if (key.empty()) err = "key required";
                    else if (is_key({"theme", "editor_dock", "reasoning_effort", "model", "endpoint", "searxng_url"})) {
                        if (!val.is_string()) err = "value for '" + key + "' must be a string";
                    } else if (is_key({"web_search", "shell", "auto_approve", "cache_prompt"})) {
                        bool b = false, ok = true;
                        if (val.is_boolean()) b = val.get<bool>();
                        else if (val.is_number()) b = val.get<double>() != 0;
                        else if (val.is_string()) {
                            std::string s = val.get<std::string>();
                            for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
                            if (s == "true" || s == "on" || s == "1" || s == "yes") b = true;
                            else if (s == "false" || s == "off" || s == "0" || s == "no") b = false;
                            else ok = false;
                        } else ok = false;
                        if (!ok) err = "value for '" + key + "' must be true or false";
                        else val = b;
                    } else if (key == "compact_at") {
                        if (val.is_string()) { try { val = std::stod(val.get<std::string>()); } catch (...) { err = "compact_at must be a number"; } }
                        else if (!val.is_number()) err = "compact_at must be a number";
                    }
                    if (!err.empty()) { res = "error: " + err; }
                    else {
                        json cfg = read_config(); cfg[key] = val;
                        std::string werr = write_config(cfg);
                        if (!werr.empty()) res = "error: " + werr;
                        else {
                            std::string applied = "saved (takes effect on restart)";
                            if (key == "theme") { tui_.set_theme(val.get<std::string>()); applied = "applied live"; }
                            else if (key == "editor_dock") { tui_.set_editor_dock(val.get<std::string>()); applied = "applied live"; }
                            else if (key == "web_search") { set_web_enabled(val.get<bool>()); applied = "applied live"; }
                            else if (key == "shell") { set_shell_enabled(val.get<bool>()); applied = "applied live"; }
                            else if (key == "auto_approve") { set_auto_approve(val.get<bool>()); applied = "applied live"; }
                            else if (key == "reasoning_effort") { set_reasoning_effort(val.get<std::string>()); applied = "applied live"; }
                            else if (key == "compact_at") { double d = val.get<double>(); if (d < 0.3) d = 0.3; if (d > 0.95) d = 0.95; set_compact_at(d); applied = "applied live"; }
                            res = "setting '" + key + "' = " + val.dump() + " (" + applied + ")";
                        }
                    }
                } else if (name == "write_file") {
                    checkpoint_file(args.value("filepath", ""));
                    res = tools_.dispatch(name, args);
                    { std::lock_guard<std::mutex> lk(last_file_mtx_); last_file_ = args.value("filepath", ""); }
                } else if (name == "edit_file") {
                    checkpoint_file(args.value("filepath", ""));
                    res = tools_.dispatch(name, args);
                    { std::lock_guard<std::mutex> lk(last_file_mtx_); last_file_ = args.value("filepath", ""); }
                } else if (name == "get_soul") {
                    res = "name: " + soul_.name() + "\n\n" + soul_.body();
                } else if (name == "set_soul") {
                    bool changed = false;
                    bool explicit_name = args.contains("name") && args["name"].is_string() && !args["name"].get<std::string>().empty();
                    if (args.contains("full_name") && args["full_name"].is_string() && !args["full_name"].get<std::string>().empty()) {
                        std::string fn = args["full_name"].get<std::string>();
                        soul_.set_full_name(fn);
                        if (!explicit_name) soul_.set_name(Soul::derive_short(fn));
                        changed = true;
                    }
                    if (explicit_name) { soul_.set_name(args["name"].get<std::string>()); changed = true; }
                    if (args.contains("body") && args["body"].is_string()) { soul_.set_body(args["body"].get<std::string>()); changed = true; }
                    if (changed) { rebuild_system_prompt(); res = "soul updated; I am now '" + soul_.name() + "'"; }
                    else res = "nothing to change (provide name, full_name, and/or body)";
                } else if (name == "recall_soul") {
                    std::string s = soul_.section(args.value("section", ""));
                    res = s.empty() ? "no such soul section (your prompt lists the available section names)" : s;
                } else if (name == "get_user_info") {
                    res = get_user_info();
                } else if (name == "set_user_info") {
                    res = set_user_info(args.value("content", ""));
                } else if (name == "define_tool") {
                    CustomTools::Def d;
                    d.name = args.value("name", "");
                    d.description = args.value("description", "");
                    d.kind = args.value("kind", "template");
                    d.command = args.value("command", "");
                    if (args.contains("steps") && args["steps"].is_array()) d.steps = args["steps"];
                    if (args.contains("params") && args["params"].is_array())
                        for (auto& p : args["params"]) {
                            CustomTools::Param pp;
                            pp.name = p.value("name", "");
                            pp.description = p.value("description", "");
                            pp.required = p.value("required", false);
                            if (!pp.name.empty()) d.params.push_back(pp);
                        }
                    d.scope = (args.value("scope", "global") == "project") ? project_key_ : "global";
                    std::string err; std::string r = custom_tools_.define(d, err);
                    if (err.empty() && store_ && !d.command.empty()) store_->dismiss_command(d.command);
                    res = err.empty() ? r : "error: " + err;
                } else if (name == "list_observed_commands") {
                    res = observed_commands_text();
                } else if (name == "dismiss_pattern") {
                    res = (store_ && store_->dismiss_command(args.value("command", ""))) ? "no longer tracking that command" : "no matching command";
                } else if (name == "list_tools") {
                    res = custom_tools_.list_json().dump(2);
                } else if (name == "remove_tool") {
                    res = custom_tools_.remove(args.value("name", "")) ? "tool removed" : "no such custom tool";
                } else if (custom_tools_.has(name)) {
                    if (!shell_enabled_.load()) res = "error: custom tools run shell and require /shell on";
                    else { custom_tools_.bump_use(name); res = run_custom_tool(name, args, 0); }
                } else if (name == "run_sudo") {
                    if (!shell_enabled_.load()) res = "error: shell access is disabled (the user can enable it with /shell on)";
                    else {
                        std::string cmd = args.value("command", "");
                        if (cmd.empty()) res = "error: 'command' is required";
                        else {
                            std::string pw = sudo_password();  // cached, or securely prompts the user
                            if (pw.empty()) res = "[sudo cancelled \u2014 the user did not provide a password]";
                            else {
                                int code = 0;
                                std::string out = Tools::exec_sudo(cmd, pw, code);
                                std::fill(pw.begin(), pw.end(), '\0');
                                if (out.find("Sorry, try again") != std::string::npos ||
                                    out.find("incorrect password") != std::string::npos ||
                                    out.find("authentication failure") != std::string::npos ||
                                    out.find("a password is required") != std::string::npos) {
                                    clear_sudo_cache();
                                    res = "sudo authentication failed (cached password cleared). Tell the user, and retry to re-prompt.\n" + out;
                                } else {
                                    res = out;
                                }
                            }
                        }
                    }
                } else if (name == "run_shell_background") {
                    if (!shell_enabled_.load()) res = "error: shell access is disabled (the user can enable it with /shell on)";
                    else {
                        std::string cmd = args.value("command", "");
                        if (cmd.empty()) res = "error: 'command' is required";
                        else {
                            int jid = shell_jobs_.start(cmd);
                            res = jid < 0 ? "error: failed to start background job"
                                          : "started background job " + std::to_string(jid) +
                                            "  (poll it with job_output id=" + std::to_string(jid) + ")";
                        }
                    }
                } else if (name == "job_output") {
                    std::string o = shell_jobs_.output((int)args.value("id", 0));
                    res = o.empty() ? "no such job id" : o;
                } else if (name == "stop_job") {
                    res = shell_jobs_.stop((int)args.value("id", 0)) ? "stop signal sent" : "no such job id";
                } else if (name == "list_jobs") {
                    auto js = shell_jobs_.list();
                    if (js.empty()) res = "no background jobs";
                    else {
                        std::string s;
                        for (auto& j : js)
                            s += "[" + std::to_string(j.id) + "] " +
                                 (j.running ? "running" : "exited(" + std::to_string(j.exit_code) + ")") +
                                 "  $ " + j.command + "\n";
                        res = s;
                    }
                } else if (name == "exec_shell" && !shell_enabled_.load()) {
                    res = "error: shell access is disabled (the user can enable it with /shell on)";
                } else if (mcp_.owns(name)) {
                    res = mcp_.call(name, args);
                } else {
                    res = tools_.dispatch(name, args);
                }
                } catch (const std::exception& _ex) {
                    res = std::string("error: tool '") + name + "' failed: " + _ex.what();
                }

                // Observe shell commands the agent ran, for learn-from-repetition.
                if (store_ && _verr.empty() && !_denied &&
                    (name == "exec_shell" || name == "run_shell_background" || name == "run_sudo")) {
                    std::string _c = args.value("command", "");
                    if (!_c.empty()) store_->record_command(_c, "agent", project_key_);
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
