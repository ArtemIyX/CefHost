# Project Log

Use this file to capture project changes so future LLM sessions can quickly understand:
- what changed,
- why it changed,
- when it changed,
- and what should happen next.

---

## Entry Template

### Date
YYYY-MM-DD HH:MM

### Changed
- 

### Why
- 

### Impact
- 


---

## 2026-04-13 8:57

### Changed
- Created `PROJECT_LOG.md` as an ongoing context file for this project.

### Why
- Keep a clear timeline of technical changes for future LLM and human contributors.

### Impact
- No code/runtime behavior changes.
- Improves continuity across sessions.

---

## 2026-04-13 12:45

### Changed
- Upgraded shared frame protocol to v2 in host:
  - `FrameHeader` now includes `version`, `slot_count`, `frame_id`, `present_id`, `flags`.
  - Added frame flags: `FULL_FRAME`, `DIRTY_ONLY`, `OVERFLOW`, `RESIZED`.
- Switched host shared texture model from fixed double-buffer to triple-buffer ring (`Global\\CEFHost_SharedTex_0..2`).
- Host publish path now writes monotonic `frame_id`, rotates slot with modulo, and forces full-frame on resize, overflow, startup warmup, and periodic keyframes.
- Added release-fence publish semantics before `sequence` update/event signal.
- Updated UE5 plugin consumer (in `CefUiExample/Plugins/CefWebUi/Source`) to read protocol v2 and triple-buffer slot count, and detect `frame_id` gaps to force full refresh.

### Why
- Fix dirty-rect ghosting caused by delta application on stale base frames when frames are dropped.
- Improve frame pacing under load with triple buffering while keeping correctness-first behavior.

### Impact
- Host build succeeds with new protocol/triple-buffer path.
- Consumer now correctly selects source slot from `write_slot % slot_count` and handles dropped-frame recovery.
- UE widget currently still performs full `CopyTexture` each frame; dirty rects are metadata-only in current consumer path (safe, not yet copy-optimized).
