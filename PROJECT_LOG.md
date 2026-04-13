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

---

## 2026-04-13 13:20

### Changed
- Refactored startup config flow for easier runtime usage:
  - Added `host_runtime_config.h/.cpp`.
  - Added CLI args parsing in `main.cpp`.
  - `SimpleApp` now receives config instead of hardcoded values.
- Runtime options now support:
  - `--url`
  - `--size WxH` (or `--width`, `--height`)
  - `--fps`
  - `--no-shared-texture`
  - `--no-external-begin-frame`
  - `--help`
- Restructured `CMakeLists.txt`:
  - Explicit `HOST_SOURCES` / `HOST_HEADERS` lists.
  - Added `host_runtime_config.cpp`.
  - Added `CMAKE_CXX_STANDARD_REQUIRED ON` and `CMAKE_CXX_EXTENSIONS OFF`.

### Why
- Remove hardcoded runtime parameters and make host behavior configurable without code edits.
- Make project layout in CMake clearer and easier to maintain.

### Impact
- Default behavior unchanged (`1920x1080`, `120 FPS`, shared texture + external begin frame enabled, `https://testufo.com/`).
- Host can now be tuned from command line for quick testing and integration.
- `cmake --build build --config Release` passes after refactor.

---

## 2026-04-13 13:34

### Changed
- Removed runtime toggles for disabling host rendering mode:
  - deleted `SharedTextureEnabled` and `ExternalBeginFrameEnabled` from runtime config.
  - removed related CLI options from usage.
  - host setup now always sets:
    - `window_info.shared_texture_enabled = true`
    - `window_info.external_begin_frame_enabled = true`

### Why
- Lock host to required production rendering path and avoid unsupported runtime combinations.

### Impact
- Shared texture mode and external begin frame are now always enabled.
- `cmake --build build --config Release` passes after the change.

---

## 2026-04-13 13:49

### Changed
- Restructured project files into folders for easier navigation:
  - sources moved to `src/`
  - public/local headers moved to `include/`
  - SHM headers moved to `include/shm/`
- Updated `CMakeLists.txt` source/header paths and include directories to match new layout.

### Why
- Improve maintainability and reduce flat root clutter.

### Impact
- Build system uses the new tree cleanly.
- `cmake --build build --config Release` passes after restructure.

---

## 2026-04-13 14:10

### Changed
- Renamed `SimpleApp` to `CefHostBrowserApp`:
  - `include/cef_browser_app.h`
  - `src/cef_browser_app.cpp`
  - updated references in `src/main.cpp` and `CMakeLists.txt`.
- Renamed SHM buffer classes/files to clearer style:
  - `FrameBuffer` -> `SharedFrameBuffer` (`include/shm/SharedFrameBuffer.h`)
  - `InputBuffer` -> `SharedInputBuffer` (`include/shm/SharedInputBuffer.h`)
  - `ControlBuffer` -> `SharedControlBuffer` (`include/shm/SharedControlBuffer.h`)
  - updated includes/usages in `include/osr_handler.h` and CMake.
- Added concise comments in startup/config/SHM paths for easier onboarding.

### Why
- Make names self-descriptive and consistent with project intent.
- Improve readability for future maintenance.

### Impact
- No behavior change.
- `cmake --build build --config Release` passes after renames.

---

## 2026-04-13 14:32

### Changed
- Added explicit shared-protocol handshake fields:
  - `protocol_magic` in `FrameHeader` (`SHM_PROTOCOL_MAGIC = 'CEFH'`).
- Host now writes handshake data during init and frame publish.
- UE reader now validates handshake:
  - startup check after mapping shared memory (fail-fast with clear log on mismatch).
  - runtime check in read loop (stop reader on mismatch).

### Why
- Detect protocol mismatch early and avoid undefined behavior/ghosting from incompatible layouts.

### Impact
- Clear fail-fast behavior if host/UE protocol versions diverge.
- `cmake --build build --config Release` passes after host-side handshake update.

---

## 2026-04-13 14:50

### Changed
- Added optional thread tuning in host runtime config:
  - new flag `EnableThreadTuning` (default `true`)
  - new CLI option `--no-thread-tuning` to disable.
- Wired thread tuning into host app setup:
  - `CefHostBrowserApp` passes config into `OsrHandler`.
- `OsrHandler` now conditionally applies tuning in render/input/control threads:
  - priority tuning (render/control above normal, input highest)
  - CPU affinity pinning to stable logical cores (best-effort).

### Why
- Reduce scheduling jitter for frame production/input handling while keeping a one-flag opt-out.

### Impact
- Thread tuning is on by default, optional to disable.
- `cmake --build build --config Release` passes after change.

---

## 2026-04-13 15:05

### Changed
- Implemented CEF-side adaptive begin-frame pacing:
  - `OsrHandler` now uses dynamic render-loop interval (`m_beginFrameIntervalNs`) instead of fixed 60 Hz sleep.
  - initial interval is derived from configured startup FPS.
  - frame-rate control (`SetFrameRate`) updates both CEF host frame rate and local begin-frame interval.
- Added control hook for consumer cadence feedback:
  - new control event `SetConsumerCadenceUs`.
  - handler applies smoothed cadence-based interval with slight producer slowdown bias to reduce burst/drop oscillation.
- Added render-loop drift resync to avoid catch-up bursts after stalls.

### Why
- Reduce frame burst/drop patterns when producer cadence diverges from actual consumer cadence.

### Impact
- Host now supports adaptive pacing inputs and better fixed-rate consistency.
- `cmake --build build --config Release` passes after change.

---

## 2026-04-13 15:22

### Changed
- Added lightweight CEF-side telemetry counters in `OsrHandler`:
  - produced frames
  - forced-full frame count
  - dirty rect count sum
  - dirty rect area sum
  - copy submit time sum/max (microseconds)
- Added periodic telemetry logging (every ~2s) in paint publish path:
  - `[OsrTelemetry] frames=... forced_full=... dirty_rects_avg=... dirty_area_avg=... copy_us_avg=... copy_us_max=...`

### Why
- Make tuning objective by exposing actual runtime behavior and copy cost.

### Impact
- No functional behavior change; only additional counters/log output.
- `cmake --build build --config Release` passes after telemetry update.
