# UI Improvement Checklist

## Current State Assessment
- [x] System dashboard (RAM/CPU/GPU monitoring)
- [x] Reasoning display area
- [x] Chat history with scrolling
- [x] Input field with mouse support
- [x] Performance metrics (tokens/s)
- ⚠️ **Input field does not wrap or expand** (hardcoded to 3 lines, needs dynamic sizing or wrapping support)

## Key UI Improvement Recommendations

### 1. Visual Enhancements
- [x] Add progress indicators for long operations (streaming, tool execution)  
  *→ Implemented: `_draw_status()` with "Streaming...", "topic: ready", custom activity status*
- [x] Implement syntax highlighting for different message types (user/agent/tools)  
  *→ Implemented: type-based coloring via `_get_color_for_type()` (user→red, assistant→white, tool→green, system→cyan)*
- [x] Add status indicators with colors for different states (idle/processing/error)  
  *→ Implemented: StatusType enum (IDLE, STREAMING, TOOL, ERROR, PROCESSING) with colored status line*
- [ ] Improve the dashboard with sparkline charts instead of simple bar graphs

### 2. Navigation & Interaction
- [x] Add fuzzy search in chat history (Ctrl+F)  
  *→ Implemented: Fuzzy matching, filtered display, highlight matches in orange, ESC to cancel*
- [x] Implement quick action shortcuts  
  *→ Implemented: Ctrl+U (clear input), Ctrl+L (clear view), Ctrl+E (edit last message)*
- [x] Add command palette (Ctrl+P) for accessing commands like `/save`, `/load`, `/reset`
  *→ Implemented: `_update_command_palette_results()`, `_render_command_palette()`, fuzzy command matching. Commands: `/save`, `/load`, `/reset`, `/clear`, `/toggle-reasoning`, `/exit`*
- [x] Improve mouse handling with click-to-copy functionality  
  *→ Implemented: Click on history line → copy to clipboard (xclip) with fallback status message*

### 3. Information Density
- [ ] Add collapsible panels (e.g., toggle reasoning display with Alt+R)
- [ ] Implement smart scrolling that auto-hides when new messages arrive
- [ ] Add message grouping for tool call sequences
- [ ] Show token usage estimates per conversation turn

### 4. User Experience
- [x] Add command history (Up/Down arrows to recall previous commands)  
  *→ Implemented: Ctrl+R (prev), Ctrl+P (next), Ctrl+X (commit+clear)*
- [ ] Implement autocomplete for commands and tool names
- [ ] Add undo capability for input editing
- [ ] Improve error feedback with visual indicators and suggestions

### 5. Performance Visualization
- [ ] Real-time throughput graph showing tokens/second over time
- [ ] Latency indicators for API calls
- [ ] Queue status display when multiple requests are pending

### 6. Accessibility
- [ ] High contrast mode toggle (Ctrl+H)
- [ ] Monospace font preference option
- [ ] Larger text mode (Ctrl+Plus/Minus)

## Implementation Priority

### High Priority (Quick Wins) — ✅ COMPLETE
- [x] Command history (Up/Down arrows to recall previous commands)
- [x] Progress indicators for streaming (immediate UX improvement)
- [x] Syntax highlighting for different message types (better readability)

### Medium Priority (Moderate Complexity) — ✅ COMPLETE
- [x] Add status indicators with colors for different states
- [x] Implement quick action shortcuts
- [x] Improve mouse handling with click-to-copy
- [x] Add fuzzy search in chat history (Ctrl+F)
- [x] **Add command palette (Ctrl+P)** ← completed in patch5_syntax

### Low Priority (Complex Features)
- [ ] Collapsible panels
- [ ] Real-time throughput graph
- [ ] High contrast mode

---

*Last updated: 2025-06-20 — after command palette implementation (Ctrl+P), synced with MEMORY_UI_IMPLEMENTATION.md. Noted: Input field wrapping/expansion is pending.*
