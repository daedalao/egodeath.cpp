#include "agent.hpp"
#include "plugins.hpp"
#include "tui.hpp"
#include <iostream>
#include <csignal>
#include <filesystem>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <vector>

using namespace egodeath;

int main() {
    LlamaClient::Config config;

    // User preferences: $XDG_CONFIG_HOME/egodeath/config.json (or ~/.config/egodeath).
    // Precedence: built-in defaults < config.json < environment variables < runtime /commands.
    std::filesystem::path config_dir = std::getenv("XDG_CONFIG_HOME")
        ? std::filesystem::path(std::getenv("XDG_CONFIG_HOME")) / "egodeath"
        : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config" / "egodeath";
    json prefs = json::object();
    {
        std::ifstream pf(config_dir / "config.json");
        if (pf) {
            try { std::stringstream ss; ss << pf.rdbuf(); prefs = json::parse(ss.str()); }
            catch (...) { prefs = json::object(); }
        }
    }
    auto pref_bool = [&](const char* k, bool d) {
        return (prefs.contains(k) && prefs[k].is_boolean()) ? prefs[k].get<bool>() : d; };
    auto pref_str = [&](const char* k, std::string d) {
        return (prefs.contains(k) && prefs[k].is_string()) ? prefs[k].get<std::string>() : d; };

    config.endpoint = pref_str("endpoint", config.endpoint);
    config.model = pref_str("model", config.model);
    config.reasoning_effort = pref_str("reasoning_effort", config.reasoning_effort);
    config.cache_prompt = pref_bool("cache_prompt", true);
    if (auto e = std::getenv("LLAMA_ENDPOINT")) config.endpoint = e;
    if (auto m = std::getenv("LLAMA_MODEL")) config.model = m;
    if (auto re = std::getenv("LLAMA_REASONING_EFFORT")) config.reasoning_effort = re;

    // Trust prompt: decide whether to use project-scoped memory
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path egodeath_dir = cwd / ".egodeath";
    std::filesystem::path project_mem_root;

    if (std::filesystem::exists(egodeath_dir)) {
        project_mem_root = egodeath_dir; // already trusted
    } else {
        std::cout << "Trust '" << cwd.string() << "' for project memory? [y/N] ";
        std::cout.flush();
        std::string line;
        std::getline(std::cin, line);
        if (!line.empty() && (line[0] == 'y' || line[0] == 'Y')) {
            std::error_code ec;
            std::filesystem::create_directories(egodeath_dir, ec);
            if (!ec) project_mem_root = egodeath_dir;
        }
    }

    // Install Ctrl+C handler — requires two presses within 2 s to exit
    signal(SIGINT, [](int) { ProTUI::sigint_received(); });

    ProTUI tui;
    tui.init();

    Agent agent(config, tui, project_mem_root);

    // Fetch context window size from server once at startup
    int ctx_size = agent.get_context_size();
    agent.set_ctx_size(ctx_size);
    {
        double ca = (prefs.contains("compact_at") && prefs["compact_at"].is_number())
                        ? prefs["compact_at"].get<double>() : 0.85;
        if (ca < 0.3) ca = 0.3; if (ca > 0.95) ca = 0.95;
        agent.set_compact_at(ca);
    }
    // Feature toggles: config.json is the base, env vars override.
    agent.set_searxng_url(pref_str("searxng_url", "http://127.0.0.1:8888"));
    agent.set_web_enabled(pref_bool("web_search", false));
    agent.set_shell_enabled(pref_bool("shell", false));
    agent.set_auto_approve(pref_bool("auto_approve", false));
    if (auto su = std::getenv("EGODEATH_SEARXNG_URL")) agent.set_searxng_url(su);
    if (auto ws = std::getenv("EGODEATH_WEB_SEARCH")) { std::string v = ws; agent.set_web_enabled(!v.empty() && v != "0"); }
    if (auto sh = std::getenv("EGODEATH_SHELL")) { std::string v = sh; agent.set_shell_enabled(!v.empty() && v != "0"); }
    if (auto aa = std::getenv("EGODEATH_AUTO_APPROVE")) { std::string v = aa; agent.set_auto_approve(!v.empty() && v != "0"); }

    // Wire Esc → cancel current generation
    tui.set_cancel_callback([&agent]() { agent.cancel(); });
    tui.set_effort_callback([&agent](const std::string& e) { agent.set_reasoning_effort(e); });
    tui.set_initial_effort(config.reasoning_effort);
    tui.set_theme(pref_str("theme", "dark"));

    auto session_path = [](const std::string& name) -> std::string {
        namespace fs = std::filesystem;
        fs::path base = fs::exists(".egodeath")
            ? fs::path(".egodeath") / "sessions"
            : fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".egodeath" / "sessions";
        std::error_code ec; fs::create_directories(base, ec);
        std::string n = name.empty() ? "last" : name;
        if (n.find('/') != std::string::npos) return n;
        if (n.size() < 5 || n.substr(n.size() - 5) != ".json") n += ".json";
        return (base / n).string();
    };
    tui.set_save_callback([&]() { agent.save_session(session_path("last")); });
    tui.set_load_callback([&]() {
        std::string p = session_path("last");
        if (std::filesystem::exists(p) && !agent.is_busy()) agent.load_session(p);
    });
    tui.set_mcp_callback([&]() { return agent.mcp_status(); });
    tui.set_agenda_provider([&]() { return agent.agenda_snapshot(); });
    tui.set_agenda_action([&](const std::string& op, long long id, const std::string& arg) {
        return agent.agenda_action(op, id, arg);
    });
    tui.set_memory_provider([&]() { return agent.memory_snapshot(); });
    tui.set_memory_action([&](const std::string& op, const std::string& name, const std::string& arg) {
        return agent.memory_action(op, name, arg);
    });
    tui.set_soul_name_provider([&]() { return agent.soul_name(); });
    tui.set_busy_fn([&]() { return agent.is_busy(); });

    // Interface plugins: dockless panels (F6+) and slash commands from
    // ~/.config/egodeath/plugins/<name>/plugin.json
    Plugins plugins;
    std::filesystem::path proot = std::getenv("XDG_CONFIG_HOME")
        ? std::filesystem::path(std::getenv("XDG_CONFIG_HOME")) / "egodeath" / "plugins"
        : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".config" / "egodeath" / "plugins";
    auto reload_plugins = [&]() {
        plugins.load(proot);
        std::vector<ProTUI::PluginPanelMeta> metas;
        for (const auto& p : plugins.all())
            if (p.has_panel) metas.push_back({p.name, p.panel_title.empty() ? p.name : p.panel_title, p.panel_key, p.panel_refresh});
        tui.register_plugin_panels(metas);
    };
    reload_plugins();
    tui.set_plugin_runner([&plugins](const std::string& n, const std::string& m, const std::string& a) {
        return plugins.run_by_name(n, m, a);
    });
    tui.set_editor_save_fn([&](const std::string& p, const std::string& c) { return agent.editor_save(p, c); });
    tui.set_last_file_provider([&]() { return agent.last_written_file(); });
    tui.set_editor_dock(pref_str("editor_dock", "right"));

    agent.on_metrics = [&](const json& m) {
        Metrics metrics;
        metrics.ctx_size = ctx_size > 0 ? std::optional<int>(ctx_size) : std::nullopt;

        // Preserve previous pp speed if this chunk doesn't carry one
        if (m.contains("prompt_per_second") && m.value("prompt_per_second", 0.0) > 0)
            metrics.pp_speed = m.value("prompt_per_second", 0.0);
        else if (m.contains("prompt_ms") && m.value("prompt_ms", 0.0) > 0)
            metrics.pp_speed = m.value("prompt_n", 0) / (m.value("prompt_ms", 1.0) / 1000.0);
        else if (m.contains("prompt_eval_time") && m.value("prompt_eval_time", 0.0) > 0)
            metrics.pp_speed = m.value("prompt_tokens", 0) / (m.value("prompt_eval_time", 1.0) / 1000.0);

        if (!metrics.pp_speed.has_value() && agent.state.metrics.pp_speed.has_value())
            metrics.pp_speed = *agent.state.metrics.pp_speed;

        if (m.contains("predicted_per_second") && m.value("predicted_per_second", 0.0) > 0)
            metrics.gen_speed = m.value("predicted_per_second", 0.0);
        else if (m.contains("predicted_ms") && m.value("predicted_ms", 0.0) > 0)
            metrics.gen_speed = m.value("predicted_n", 0) / (m.value("predicted_ms", 1.0) / 1000.0);
        else if (m.contains("eval_time") && m.value("eval_time", 0.0) > 0)
            metrics.gen_speed = m.value("eval_tokens", 0) / (m.value("eval_time", 1.0) / 1000.0);

        // Exact value from STREAM_END timings (highest priority).
        // usage.total_tokens reflects the FULL context (cached + new); prompt_n is
        // only the newly-processed prefix, so prefer usage to keep the gauge honest.
        if (m.contains("usage_total_tokens")) {
            metrics.ctx_used = m.value("usage_total_tokens", 0);
            int processed = m.value("prompt_n", 0);
            int gen_tok = m.value("usage_completion_tokens", m.value("predicted_n", 0));
            int cached = m.value("cache_n", m.value("usage_cached_tokens", 0));
            std::string cache_hint = cached > 0 ? fmt::format("  \u26a1{} cached", cached) : std::string();
            tui.set_status(StatusType::IDLE,
                fmt::format("{}pp + {}gen tokens{}", processed, gen_tok, cache_hint));
        }
        else if (m.contains("prompt_n")) {
            int pp_tok = m.value("prompt_n", 0);
            int gen_tok = m.value("predicted_n", 0);
            metrics.ctx_used = pp_tok + gen_tok;
            tui.set_status(StatusType::IDLE,
                fmt::format("{}pp + {}gen tokens", pp_tok, gen_tok));
        }
        // Live estimate from METRICS_UPDATE (est_prompt + tokens generated so far)
        else if (m.contains("ctx_used_estimate"))
            metrics.ctx_used = m.value("ctx_used_estimate", 0);
        // Preserve last known value between turns
        else if (agent.state.metrics.ctx_used.has_value())
            metrics.ctx_used = agent.state.metrics.ctx_used;

        tui.set_metrics(metrics);
        agent.state.metrics = metrics;
    };

    agent.boot_greeting();

    std::string cur_effort = config.reasoning_effort;
    while (tui.is_running()) {
        std::string input = tui.get_input();
        if (input == "/exit" || input == "/quit") break;

        // Interface-plugin slash commands and /plugins listing.
        if (input == "/plugins reload" || input == "/reload") {
            reload_plugins();
            tui.append_history("", "reloaded plugins:\n" + plugins.list_text(), "system");
            continue;
        }
        if (input == "/plugins") { tui.append_history("", plugins.list_text(), "system"); continue; }
        if (!input.empty() && input[0] == '/') {
            std::string tok = input.substr(0, input.find(' '));
            if (const auto* pp = plugins.by_slash(tok)) {
                std::string pargs = input.size() > tok.size() ? input.substr(tok.size() + 1) : std::string();
                tui.append_history("", tok + (pargs.empty() ? "" : " " + pargs), "system");
                std::string pout = plugins.run(*pp, "command", pargs);
                tui.append_history("", pout.empty() ? "(no output)" : pout, "system");
                continue;
            }
        }
        if (input.empty()) continue;

        // Inline slash command: /mcp (list MCP servers and tools)
        if (input == "/mcp") {
            tui.append_history("", agent.mcp_status(), "system");
            continue;
        }

        // Inline slash command: /agenda or /tasks (open the task/calendar view)
        if (input == "/agenda" || input == "/tasks" || input == "/cal" || input == "/calendar") {
            tui.open_agenda();
            continue;
        }

        // Inline slash command: /memory or /mem (open the persistent memory view)
        if (input == "/memory" || input == "/mem" || input == "/memories") {
            tui.open_memory();
            continue;
        }

        // Inline slash command: /learn (show the observed repeated-command log)
        if (input == "/learn") {
            tui.append_history("", agent.observed_commands_text(), "system");
            continue;
        }

        // Inline slash command: /sudo-forget (clear any cached sudo password now)
        if (input == "/sudo-forget" || input == "/sudo forget") {
            agent.clear_sudo_cache();
            tui.append_history("", "cached sudo password cleared", "system");
            continue;
        }

        // Inline slash command: /soul (edit the agent's core identity in the editor)
        if (input == "/soul") {
            tui.open_editor(agent.soul_path());
            continue;
        }

        // Inline slash command: /rename <name> (rename the agent / its soul)
        if (input.rfind("/rename", 0) == 0) {
            std::string arg = input.size() > 7 ? input.substr(7) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            if (arg.empty()) tui.append_history("", "usage: /rename <name>  (current: " + agent.soul_name() + ")", "system");
            else { agent.set_soul_name(arg); std::string _ev = agent.soul_name(); std::string _fl = agent.soul_full_name();
                   tui.append_history("", _fl == _ev ? ("renamed to: " + _ev) : ("renamed to: " + _fl + "  (going by " + _ev + ")"), "system"); }
            continue;
        }

        // Inline slash command: /editdock left|right|bottom|full (where the editor pane appears)
        if (input.rfind("/editdock", 0) == 0) {
            std::string arg = input.size() > 9 ? input.substr(9) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            if (arg.empty()) tui.append_history("", "editor dock: " + tui.editor_dock() + "  (usage: /editdock left|right|bottom|full)", "system");
            else { tui.set_editor_dock(arg); tui.append_history("", "editor dock set to: " + tui.editor_dock(), "system"); }
            continue;
        }

        // Inline slash command: /edit [path] (open the modal code editor; no arg = last agent file)
        if (input.rfind("/edit", 0) == 0) {
            std::string arg = input.size() > 5 ? input.substr(5) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            if (arg.empty()) arg = agent.last_written_file();
            if (arg.empty()) { tui.append_history("", "usage: /edit <path>", "system"); continue; }
            tui.open_editor(arg);
            continue;
        }

        // Inline slash command: /undo (restore the last file written by a tool)
        if (input == "/undo") {
            tui.append_history("", agent.undo_last_edit(), "system");
            continue;
        }

        // Inline slash command: /save [name|path]
        if (input.rfind("/save", 0) == 0) {
            std::string arg = input.size() > 5 ? input.substr(5) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            std::string path = session_path(arg);
            agent.save_session(path);
            tui.append_history("", "session saved: " + path, "system");
            continue;
        }

        // Inline slash command: /load [name|path]
        if (input.rfind("/load", 0) == 0) {
            std::string arg = input.size() > 5 ? input.substr(5) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            std::string path = session_path(arg);
            if (!std::filesystem::exists(path)) {
                tui.append_history("", "no such session: " + path, "system");
            } else if (agent.is_busy()) {
                tui.append_history("", "cannot load while the agent is working", "system");
            } else {
                agent.load_session(path);
                tui.append_history("", "session loaded: " + path, "system");
            }
            continue;
        }

        // Inline slash command: /effort [low|medium|high] (no arg = cycle)
        if (input.rfind("/effort", 0) == 0) {
            std::string arg = input.size() > 7 ? input.substr(7) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            if (arg == "low" || arg == "medium" || arg == "high") {
                cur_effort = arg;
            } else if (arg.empty()) {
                cur_effort = (cur_effort == "low") ? "medium"
                           : (cur_effort == "medium") ? "high" : "low";
            } else {
                tui.append_history("", "usage: /effort [low|medium|high]", "system");
                continue;
            }
            agent.set_reasoning_effort(cur_effort);
            tui.set_initial_effort(cur_effort);
            tui.append_history("", "reasoning effort: " + cur_effort, "system");
            continue;
        }

        // Inline slash command: /theme [name] (no arg = cycle; custom themes from config)
        if (input.rfind("/theme", 0) == 0) {
            std::string arg = input.size() > 6 ? input.substr(6) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            if (arg.empty()) {
                tui.cycle_theme();
                tui.append_history("", "theme: " + tui.current_theme(), "system");
            } else if (tui.has_theme(arg)) {
                tui.set_theme(arg);
                tui.append_history("", "theme: " + arg, "system");
            } else {
                tui.append_history("", "unknown theme: " + arg + " (custom themes go in ~/.config/egodeath/themes.json)", "system");
            }
            continue;
        }

        // Inline slash command: /web on|off (SearXNG web search tool)
        if (input.rfind("/web", 0) == 0) {
            std::string arg = input.size() > 4 ? input.substr(4) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            if (arg == "on" || arg == "1" || arg == "yes") {
                agent.set_web_enabled(true);
                tui.append_history("", "web search: on", "system");
            } else if (arg == "off" || arg == "0" || arg == "no") {
                agent.set_web_enabled(false);
                tui.append_history("", "web search: off", "system");
            } else {
                tui.append_history("", std::string("web search: ") + (agent.web_enabled() ? "on" : "off")
                                       + "  (usage: /web on|off)", "system");
            }
            continue;
        }

        // Inline slash command: /auto on|off (per-tool approval auto-accept)
        if (input.rfind("/auto", 0) == 0) {
            std::string arg = input.size() > 5 ? input.substr(5) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            bool on = (arg == "on" || arg == "1" || arg == "yes");
            bool off = (arg == "off" || arg == "0" || arg == "no");
            if (!on && !off) { tui.append_history("", "usage: /auto [on|off]", "system"); continue; }
            agent.set_auto_approve(on);
            tui.append_history("", std::string("auto-approve: ") + (on ? "on" : "off"), "system");
            continue;
        }

        // Inline slash command: /shell on|off (the model's exec_shell tool; off by default)
        if (input.rfind("/shell", 0) == 0) {
            std::string arg = input.size() > 6 ? input.substr(6) : std::string();
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
            while (!arg.empty() && arg.back() == ' ') arg.pop_back();
            bool on = (arg == "on" || arg == "1" || arg == "yes");
            bool off = (arg == "off" || arg == "0" || arg == "no");
            if (!on && !off) {
                tui.append_history("", std::string("shell access: ") + (agent.shell_enabled() ? "on" : "off")
                                       + "  (usage: /shell on|off)", "system");
                continue;
            }
            agent.set_shell_enabled(on);
            tui.append_history("", std::string("shell access: ") + (on ? "on" : "off"), "system");
            continue;
        }

        // ! shell passthrough: run locally, show output, don't involve the model
        if (input[0] == '!') {
            std::string cmd = input.substr(1);
            size_t s = cmd.find_first_not_of(' ');
            cmd = (s == std::string::npos) ? "" : cmd.substr(s);
            if (cmd.empty()) { tui.append_history("", "usage: !<shell command>", "system"); continue; }
            tui.append_history("", "! " + cmd, "system");
            agent.observe_command(cmd, "user");
            std::string out = Tools::exec_shell(cmd);
            if (out.empty()) out = "(no output)";
            if (out.size() > 6000) out = out.substr(0, 6000) + "\n\u2026 (truncated)";
            tui.append_history("", out, "system");
            continue;
        }

        // @file injection: inline file contents into the message sent to the model,
        // while the chat view keeps the clean @mention the user typed.
        {
            std::string augmented = input;
            std::stringstream toks(input);
            std::string tok;
            bool injected = false;
            while (toks >> tok) {
                if (tok.size() > 1 && tok[0] == '@') {
                    std::error_code ec;
                    std::filesystem::path fp = tok.substr(1);
                    if (std::filesystem::is_regular_file(fp, ec)) {
                        augmented += "\n\n--- " + tok + " ---\n" + Tools::read_file(fp);
                        injected = true;
                    }
                }
            }
            if (injected) { agent.run_step(augmented, input); continue; }
        }

        agent.run_step(input);
    }

    tui.shutdown();
    return 0;
}
