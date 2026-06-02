# egodeath.cpp

A terminal agent interface for locally-hosted LLMs. Connects to any llama.cpp-compatible
server (`/v1/chat/completions`) and provides a multi-panel ncurses TUI with real-time
streaming, agentic tool use, persistent memory, and hardware monitoring.

---

## Features

### Interface
- Multi-panel layout: system dashboard, scrollable chat history, reasoning panel, status bar, input box
- Syntax-highlighted message types — user (red), assistant (white), tool calls (green), system (cyan)
- Collapsible reasoning panel with independent scroll
- Status bar with live state: idle, streaming, tool execution, error
- Real-time prompt-processing and generation speed (t/s) updating during streaming
- Context window usage displayed in the dashboard (`ctx N% / limit`, pulled from `/props` at startup)
- Smart auto-scroll: follows new output unless you have manually scrolled up
- Terminal resize handled without losing history

### Tool use
- `read_file` — read a file (capped at 512 KB, rejects paths outside working directory)
- `write_file` — write a file (same path restriction)
- `exec_shell` — run a shell command and return output (**disabled by default**; enable with `/shell on`)
- `grep_search` — regex search across files (skips binaries)
- `list_directory` — list a directory
- `glob_search` — find files matching a glob pattern
- Tool calls rendered as inline blocks showing tool name, primary argument, and result preview

### Memory
- File-backed persistent memory across sessions
- Global store at `~/.egodeath/memory/`; project-scoped store at `.egodeath/` in the working directory
- Memories are injected into the system prompt at startup
- Model can call `save_memory`, `recall_memory`, `remove_memory` as tools

### System monitoring
- RAM usage polled from `/proc/meminfo`
- llama.cpp process CPU usage from `/proc/[pid]/stat`
- NVIDIA GPUs via NVML (loaded dynamically, no static link required)
- AMD GPUs via `/sys/class/drm` sysfs

---

## Requirements

- CMake ≥ 3.20
- C++20
- libcurl
- ncursesw (wide-character ncurses)
- [fmtlib](https://github.com/fmtlib/fmt)
- [nlohmann/json](https://github.com/nlohmann/json) (optional system package; bundled header-only fallback used otherwise)
- A running [llama.cpp](https://github.com/ggerganov/llama.cpp) server or any OpenAI-compatible `/v1/chat/completions` endpoint

### Arch / archpower (ppc64le)

```bash
sudo pacman -S cmake curl ncurses fmt nlohmann-json
```

### Debian / Ubuntu

```bash
sudo apt install cmake libcurl4-openssl-dev libncursesw5-dev libfmt-dev nlohmann-json3-dev
```

---

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Binary is placed at `build/egodeath`.

---

## Running

```bash
# Defaults: http://127.0.0.1:8080/v1/chat/completions, model "local-model"
./build/egodeath

# Custom endpoint and model
LLAMA_ENDPOINT=http://192.168.1.10:8080/v1/chat/completions \
LLAMA_MODEL=qwen2.5-coder-32b \
./build/egodeath
```

| Variable | Default | Description |
|---|---|---|
| `LLAMA_ENDPOINT` | `http://127.0.0.1:8080/v1/chat/completions` | Server URL |
| `LLAMA_MODEL` | `local-model` | Model name sent in requests |
| `LLAMA_REASONING_EFFORT` | `medium` | Reasoning effort (`low`/`medium`/`high`) sent with each request |
| `EGODEATH_AUTO_APPROVE` | _(unset)_ | Set to `1` to auto-approve all tool calls (skip confirmation prompts) |
| `EGODEATH_SHELL` | _(unset)_ | Set to `1` to enable the model's `exec_shell` tool at startup |
| `EGODEATH_SEARXNG_URL` | `http://127.0.0.1:8888` | Base URL of the SearXNG instance for web search |
| `EGODEATH_WEB_SEARCH` | _(unset)_ | Set to `1` to enable the web search tool at startup |
| `EGODEATH_MCP_CONFIG` | `.egodeath/mcp.json` | Path to the MCP server config |

### Configuration file

Persistent preferences live in `~/.config/egodeath/config.json` (or
`$XDG_CONFIG_HOME/egodeath/config.json`). All keys are optional:

```json
{
  "endpoint": "http://127.0.0.1:8080/v1/chat/completions",
  "model": "local-model",
  "reasoning_effort": "medium",
  "theme": "dark",
  "web_search": false,
  "shell": false,
  "auto_approve": false,
  "searxng_url": "http://127.0.0.1:8888",
  "compact_at": 0.85
}
```

**Precedence (later wins):** built-in defaults → `config.json` → environment variables →
runtime `/commands`. So you can set `"web_search": true` or `"shell": true` here to have
them on by default, while still toggling them per-session with `/web` / `/shell`. `compact_at` is the fraction of the context window (0.3–0.95) at which the conversation is auto-summarized.

The same directory is also the global home for MCP servers: `~/.config/egodeath/mcp.json`
is used when no project-level `.egodeath/mcp.json` is present.

### System prompt (`egodeath.md`)

The system prompt is read from an external file at startup, in this order:

1. `./egodeath.md` (project root)
2. `./.egodeath/egodeath.md`
3. the built-in default (used if neither file exists)

Edit `egodeath.md` to customize agent behavior per project. Project memory from the
`.egodeath/memory` store is appended to whichever prompt is loaded.

### Reasoning effort

Because llama-server pins a single model, you tune *thinking depth* instead of swapping
models. Set it once with `LLAMA_REASONING_EFFORT`, or cycle it live with `/effort` in the
command palette. The current level is shown on the dashboard's top border. The value is
sent as `reasoning_effort` on each request; models that don't support it ignore the field.

### Inline input shortcuts

Typed directly in the input box (no palette needed):

| Syntax | Action |
|---|---|
| `!<command>` | Run a shell command locally and print its output — the model is not involved |
| `@<path>` | Inject a file's contents into your message; the chat view keeps the clean `@path` mention while the model receives the full text |
| `/effort [low\|medium\|high]` | Set reasoning effort (no argument cycles) |
| `/theme [dark\|matrix\|amber\|mono]` | Switch color theme (no argument cycles) |
| `/auto [on\|off]` | Toggle tool-call auto-approval |
| `/shell [on\|off]` | Enable/disable the model's `exec_shell` tool (off by default) |
| `/web [on\|off]` | Toggle the SearXNG web search tool (no argument shows status) |
| `/undo` | Restore the file changed by the most recent `write_file` tool call |
| `/mcp` | List connected MCP servers and their tools |
| `/save [name]` | Save the session (default slot `last`, or a named slot) |
| `/load [name]` | Load a saved session and repaint the transcript |
| `Tab` | Autocomplete a slash command and its arguments |

You can mix multiple `@path` mentions in one message. Files are read relative to the
working directory; non-existent paths are left as plain text.

### Tool-call approval

Mutating tools (`exec_shell`, `write_file`) prompt for confirmation before running:

```
⚠ approve exec_shell?  rm -rf build   [y]es  [n]o  [a]lways
```

- **y** — run this one call
- **n** / **Esc** — deny it (the model is told the call was denied and can adapt)
- **a** — approve this and all further calls for the rest of the session

Read-only tools (read_file, grep, list, glob, recall_memory) never prompt. Start in
auto-approve mode with `EGODEATH_AUTO_APPROVE=1`, or toggle live with `/auto on|off`.

### Context compression

When the conversation approaches ~85% of the model's context window, egodeath
automatically summarizes the older turns into a single recap and continues from there,
so long sessions don't overflow a fixed local model. The cut is aligned to a user-turn
boundary so tool-call/result pairs are never split, and a `context compressed` note is
shown when it happens.

### Themes

Four built-in color themes: `dark` (default), `matrix`, `amber`, and `mono`. Switch with
`/theme` (palette cycles) or `/theme <name>` (inline). The change applies instantly.

**Custom themes** can be defined in `~/.config/egodeath/themes.json`. Each theme overrides
any of these color roles (others inherit from `dark`); each value is `[foreground,
background]` in xterm-256 codes (`-1` = terminal default):

```json
{
  "nord": {
    "border": [110, -1],
    "text":   [188, -1],
    "user":   [236, 110],
    "code":   [108, 236]
  }
}
```

Roles: `border`, `tool`, `accent`, `text`, `user`, `alt`, `error`, `highlight`,
`selection`, `dim`, `code`. Custom themes join the `/theme` cycle alongside the built-ins,
and can be set as the default via `"theme"` in `config.json`.

### Web search (SearXNG)

egodeath can search and read the web through a local
[SearXNG](https://github.com/searxng/searxng) instance. These tools are **off by default**
— enable them per-session with `/web on` (or start enabled via `EGODEATH_WEB_SEARCH=1`).
While enabled, two tools are offered to the model; while disabled, neither is exposed:

- **`web_search`** — query SearXNG, returns ranked title/URL/snippet results.
- **`web_fetch`** — fetch an `http(s)` URL and return its readable text (HTML stripped,
  scripts/styles removed, capped at 20k chars). The model typically searches, then fetches
  a promising result to read it in full.

SearXNG setup:

1. **Enable JSON output** (off by default). In `/etc/searxng/settings.yml`, ensure:

   ```yaml
   search:
     formats:
       - html
       - json
   ```

   Without this, requests return HTTP 403.

2. **Start the service:** `sudo systemctl start searxng` (add `enable` to persist).

3. Point egodeath at it with `EGODEATH_SEARXNG_URL` if it isn't on the default
   `http://127.0.0.1:8888`.

### Edit undo

Every `write_file` tool call snapshots the file's prior contents first (up to 50 levels).
`/undo` rolls back the most recent write — restoring the previous content, or deleting the
file if it didn't exist before. Pairs well with the tool-approval prompt: approve a write,
inspect the result, `/undo` if you don't like it.

### Throughput sparkline

The dashboard draws a live Unicode sparkline (`▁▂▃▄▅▆▇█`) of recent generation
tokens/sec next to the `pp / gen` figures, so you can see throughput trends at a glance
(rendered when the terminal is wide enough).

### MCP servers

egodeath can load external [MCP](https://modelcontextprotocol.io) servers and expose
their tools to the model, so capabilities can be added by config instead of recompiling.

Define servers in `.egodeath/mcp.json` (or point `EGODEATH_MCP_CONFIG` elsewhere), using
the standard `mcpServers` schema — configs from Claude Desktop / other MCP clients work
unchanged:

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/project"]
    }
  }
}
```

At startup egodeath spawns each server (stdio transport), runs the MCP handshake, and
discovers its tools. They appear to the model namespaced as `mcp__<server>__<tool>`. Run
`/mcp` to list connected servers and their tools. MCP tools are treated as untrusted, so
they go through the same approval prompt as `exec_shell`/`write_file` (bypass with `/auto`).

Requires a runtime for the server commands (e.g. `node`/`npx` or `python`). Server stderr
is discarded so it can't corrupt the TUI. Current support is stdio transport + tools
(resources/prompts and HTTP transport are not yet implemented).

### Sessions (save / resume)

`/save [name]` writes the full conversation (system prompt, messages, tool calls and
results) as JSON; `/load [name]` restores it and repaints the on-screen transcript. With no
name, the default `last` slot is used; a bare name is stored under `<.egodeath or ~/.egodeath>/sessions/<name>.json`,
and a value containing `/` is treated as an explicit path. The command palette `/save` and
`/load` act on the default slot. Loading is refused while the agent is mid-turn.

---

## Keyboard Reference

Press **F1** at any time to open the in-app help overlay with a full keybinding reference.

### Cursor movement

| Key | Action |
|---|---|
| **Ctrl+A** | Beginning of line |
| **Ctrl+E** | End of line |
| **Ctrl+B** | Back one character |
| **Ctrl+F** | Forward one character (opens fuzzy search when triggered at rest) |
| **← →** / **Home** / **End** | Move cursor |

### Editing

| Key | Action |
|---|---|
| **Ctrl+U** | Kill entire line |
| **Ctrl+K** | Kill from cursor to end of line |
| **Ctrl+W** | Delete word backwards |
| **Ctrl+D** | Delete character forward |
| **Backspace** | Delete character backward |

### Submission

| Key | Action |
|---|---|
| **Enter** | Submit prompt |
| **Ctrl+J** / **Shift+Enter** | Insert newline (multi-line input) |
| **Paste** | Bracketed paste — pastes > 300 chars or with newlines show `[Pasted · N lines · M chars]`; Enter sends, Ctrl+U cancels |

### Navigation

| Key | Action |
|---|---|
| **PgUp / PgDn** | Scroll chat history |
| **Ctrl+L** | Jump to bottom of history |
| **Ctrl+F** | Open fuzzy search (Up/Down navigate matches, ESC cancels) |
| **Ctrl+P** | Open command palette |

### Reasoning panel

| Key | Action |
|---|---|
| **Ctrl+T** | Toggle show / hide |
| **Ctrl+Y** | Scroll up one page |
| **Ctrl+N** | Scroll down one page |

### Command history

| Key | Action |
|---|---|
| **Ctrl+R** | Recall previous command |
| **Ctrl+X** | Save current input to history and clear |

### Other

| Key | Action |
|---|---|
| **F1** | Toggle help overlay |
| **Ctrl+G** | Toggle mouse (scroll wheel on; native terminal text selection off while active) |

> **Text selection**: mouse is off by default so click-and-drag selection works normally.
> Enable with Ctrl+G for scroll-wheel support; use Shift+drag for selection in that mode.

### Command palette (Ctrl+P)

| Command | Action |
|---|---|
| `/save` | Save the current session (default slot) |
| `/load` | Load the default session |
| `/reset` | Clear conversation and chat history |
| `/clear` | Reset scroll position |
| `/toggle-reasoning` | Toggle reasoning panel |
| `/effort` | Cycle reasoning effort (low → medium → high) |
| `/theme` | Cycle color theme |
| `/exit` | Quit |

---

## Color scheme

| Color | Meaning |
|---|---|
| Cyan | System / UI chrome |
| White | Assistant output |
| Green | Tool calls and results |
| Red (on magenta) | User input |
| Yellow/Orange | Status and activity indicators |
| Red (bright) | Errors |
| Orange | Search highlights |

---

## Known limitations

- **Input field is capped at 3 visible lines.** Long inputs scroll within the box but the
  box itself does not grow beyond 3 lines. Dynamic expansion is a planned improvement.
- **`exec_shell` is off by default and unrestricted when on.** The model cannot run shell
  commands unless you enable the tool (`/shell on` or `EGODEATH_SHELL=1`); when enabled it
  runs as the current user with no sandboxing, and each call still requires approval unless
  `/auto` is on. Your own `!<command>` passthrough is always available and independent of
  this setting. Run
  egodeath inside a container or VM if you need isolation.
---

## Roadmap

Items from the original design doc that are not yet implemented:

| Feature | Priority |
|---|---|
| Sparkline charts in dashboard (replace bar graphs) | Low |
| Token usage display per turn | Low |
| Message grouping for sequential tool calls | Low |
| Autocomplete for slash-commands and tool names | Medium |
| Undo/redo for input editing | Medium |
| Real-time throughput graph (t/s over time) | Low |
| Latency indicators for API round-trips | Low |
| Queue status when multiple requests are pending | Low |
| High contrast mode (Ctrl+H) | Low |
| Larger text / font-size mode | Low |
| Dynamic input box height | Medium |

---

## Architecture

```
main.cpp          entry point — env config, metrics callback, submit loop
agent.cpp/.hpp    orchestration: streaming, tool dispatch, memory, async workers
client.cpp/.hpp   HTTP/SSE client (libcurl), OpenAI-compatible streaming parser
tools.cpp/.hpp    filesystem + shell tools with path-traversal protection
memory.cpp/.hpp   file-backed memory system with YAML-frontmatter index files
tui.cpp/.hpp      ncurses TUI — layout, rendering, all input handling
sys_monitor.cpp   /proc polling + NVML (NVIDIA) + sysfs (AMD) GPU stats
common.hpp        shared types: UIEvent, Metrics, UIState, Style
```

Two threads run alongside the main input loop:

- **background_worker** — serialises `turn_async` invocations, dispatches tool calls
- **ui_worker** — drains the `UIEvent` queue and applies TUI updates

---

## License

MIT
