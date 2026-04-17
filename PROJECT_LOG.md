# Project Log

Use this file to capture project changes so future LLM sessions can quickly understand:
- what changed,
- why it changed,
- when it changed,
- and what should happen next.
- do not add that - `cmake --build build --config Release` passes after the change.

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

---

## 2026-04-13 15:46

### Changed
- Tightened begin-frame throttle in `OsrHandler::TrySendBeginFrame()`:
  - now gates by current dynamic interval (`m_beginFrameIntervalNs`) instead of fixed `1000us`.
  - uses CAS loop to avoid races while preserving throttle window.
- Removed input-loop forced begin-frame trigger:
  - deleted `TrySendBeginFrame()` call at end of `PumpInput()`.

### Why
- Prevent producer overrun at 60 Hz scenarios where input activity was effectively bypassing pacing and causing frame-id gaps on consumer side.

### Impact
- Begin-frame production now follows adaptive cadence consistently.

---

## 2026-04-13 16:08

### Changed
- Restored low-latency input-triggered begin-frame nudge in `PumpInput()`:
  - sends an immediate begin frame on interactive input (mouse click/wheel, key down/up, char),
  - but hard-gated to avoid burst spam (`<= 1 nudge / min(cadence, 2ms)`).
- Reduced cadence slowdown bias in adaptive pacing from `+5%` to `+1%`.
- Added host runtime config flag:
  - `EnableCadenceFeedback` (default `false`).
  - CLI opt-in: `--enable-cadence-feedback`.
- `SetConsumerCadenceUs` control event is now ignored unless cadence feedback is enabled.

### Why
- Fix large input latency caused by over-throttled producer cadence while preserving anti-gap stability.

### Impact
- Default behavior is latency-first, with optional adaptive pacing opt-in.
---

## 2026-04-13 16:34

### Changed
- Added host-side idle ghost-repair watchdog in `OsrHandler`:
  - tracks last published frame timestamp and whether last publish was dirty-only.
  - if stream is idle after dirty publish (`~150ms`), requests `Invalidate(PET_VIEW)` once per cooldown.
- Hooked watchdog into render loop next to begin-frame scheduling.

### Why
- Recover from under-invalidation cases where final cleanup paint is missed and stale regions persist while scene is static.

### Impact
- Adds conservative self-heal path for static ghost artifacts with low overhead.

---

## 2026-04-13 17:05

### Changed
- Upgraded SHM frame protocol to v3:
  - added `gpu_fence_value` to `FrameHeader`.
- Added host-side shared GPU fence (`Global\\CEFHost_SharedFence`):
  - create `ID3D11Fence` via `ID3D11Device5`,
  - publish per-frame fence value after copy via `ID3D11DeviceContext4::Signal`.
- Reduced `context->Flush()` pressure:
  - no longer unconditional per frame,
  - flush on fallback paths and periodic interval (`m_flushIntervalFrames`), while fence signal is primary GPU completion path.

### Why
- Lower producer stalls from per-frame flush while providing explicit GPU completion ordering for cross-process consumer reads.

### Impact
- Better pacing under load and stronger producer->consumer correctness semantics.

---

## 2026-04-13 17:42

### Changed
- Fixed dedicated popup-plane update path in `OsrHandler`:
  - `PET_POPUP` now publishes frame metadata/event (`frame_id`/`sequence`) instead of waiting for next `PET_VIEW`.
  - popup-only updates now copy popup rect into `Global\\CEFHost_SharedPopupTex` and signal GPU fence for that publish.
- Reduced popup hover latency in input path:
  - `MouseMove` now triggers begin-frame nudge while popup is visible (same gated nudge path used for interactive input).

### Why
- Popup hover state (combobox/select overlays) could appear stale/laggy because popup paints were cached but not published immediately.
- Mouse move over popup items needed faster frame triggering to feel responsive.

### Impact
- Popup overlay updates now arrive immediately on popup paint events.
- Dropdown hover feedback is visibly smoother with lower perceived lag.

---

## 2026-04-14 11:40

### Changed
- Added host->UE JS console log transport over shared memory:
  - `SharedMemoryLayout.h`:
    - new SHM constants `SHM_CONSOLE_NAME`, `EVT_CONSOLE_READY`
    - new ring sizing constants (`CONSOLE_RING_CAPACITY`, `CONSOLE_MESSAGE_MAX`, `CONSOLE_SOURCE_MAX`)
    - new `ConsoleLogLevel`, `ConsoleLogEvent`, `ConsoleRingBuffer`
  - added `include/shm/SharedConsoleBuffer.h` (producer ring wrapper).
- Wired console publishing in host render handler:
  - `OsrHandler` now owns `SharedConsoleBuffer`.
  - `Init()` now initializes console buffer and `Shutdown()` closes it.
  - Implemented `OnConsoleMessage(...)` to map CEF severity to protocol level and publish message/source/line to SHM.
- Updated host CMake lists to include `SharedConsoleBuffer.h`.
- Host build validated after change.

### Why
- UE needed full JS console visibility (log/warning/error) from CEF process with a reliable IPC path.
- Existing frame/input/control channels had no console-log transport.

### Impact
- JS console messages are now exported by host in real time for UE-side logging/event dispatch.
- Protocol surface expanded with a dedicated console ring channel.

---

## 2026-04-14 12:05

### Changed
- Extended control protocol with two new events in `ControlEventType`:
  - `OpenLocalFile = 22`
  - `LoadHtmlString = 23`
- Host control handling (`OsrHandler::PumpControl`) now supports:
  - `OpenLocalFile`: converts incoming path to `file://` URL (normalizes separators + URI encodes path) and loads it.
  - `LoadHtmlString`: builds a `data:text/html;charset=utf-8,...` URL from in-memory HTML and loads it.
- Added helper URL conversion logic in `src/osr_handler.cpp` and included CEF parser utilities.

### Why
- Add native control-channel support for loading local disk content and RAM-provided HTML payloads without requiring external HTTP serving.

### Impact
- UE/consumer can now request local-file navigation and direct in-memory HTML rendering through the existing control ring.
- Host behavior remains backward-compatible for existing control events.

---

## 2026-04-15 10:20

### Changed
- Tuned host pacing defaults for smoother 60 Hz consumer sync:
  - `m_maxInFlightBeginFrames` default set to `1`.
  - `m_flushIntervalFrames` default set to `2`.
  - `m_keyframeIntervalUs` default set to `150000`.
- Updated load-state propagation to UE:
  - `OnLoadStart`, `OnLoadEnd`, and `OnLoadError` now update `FrameHeader::load_state`, increment `sequence`, and signal `CEFHost_FrameReady` immediately.
  - This load-state signal is emitted even if no new paint frame is produced.

### Why
- Reduce cadence burst/skip behavior seen as horizontal animation jitter.
- Prevent UE-side load-state listeners from stalling when page state changes without a paint-triggered frame event.

### Impact
- More stable producer pacing defaults for 60 FPS host/consumer pipelines.
- UE can reliably observe `Loading/Ready/Error` transitions without waiting for a fresh render event.

---

## 2026-04-15 18:05

### Changed
- Stabilized host pacing and diagnostics for horizontal animation smoothness:
  - removed noisy `[Control]` runtime logs.
  - enforced bounded begin-frame backpressure and reset stale in-flight debt when cap changes.
  - moved default `max in-flight begin frames` to `2` for smoother bounded pipelining.
- Reworked host render scheduler:
  - replaced simple sleep loop with QPC cadence loop.
  - then upgraded wait path to high-resolution waitable timer + short spin tail.
  - disabled render-thread affinity pinning (input/control pinning unchanged).
  - softened catch-up policy to only hard-resync on major stalls.
- Removed proactive keyframe full-frame forcing from steady-state paint path.
- Extended host telemetry:
  - added forced-full cause split (`forced_manual`, `forced_recreate`, `forced_overflow`).
  - added producer cadence fields (`interval_us`, `producer_fps`).
  - added scheduler lateness stats (`sched_miss`, `sched_late_us_avg`, `sched_late_us_max`).
- Reverted temporary hard FPS lock:
  - runtime `SetFrameRate` control works again (clamped to safe range), with telemetry kept.

### Why
- Eliminate forward/backward jitter and periodic hold/catch-up stutter without breaking rendering.
- Separate transport issues from scheduler issues using explicit per-window diagnostics.

### Impact
- Queue/fence instability was removed (`in_flight` and `gaps` stabilized in tests).
- Remaining smoothness work is now narrowed to compositor/paint cadence spikes, with telemetry to target them directly.

---

## 2026-04-15 20:10

### Changed
- Added Chromium anti-throttle startup switches in `CefHostBrowserApp::OnBeforeCommandLineProcessing`:
  - `disable-background-timer-throttling`
  - `disable-renderer-backgrounding`
  - `disable-backgrounding-occluded-windows`
  - `disable-features=CalculateNativeWinOcclusion`
- Added and kept richer host telemetry in `OsrHandler`:
  - timing window and real rates (`window_ms`, `sent_fps`, `paint_fps`)
  - producer settings (`interval_us`, `producer_fps`)
  - scheduler lateness (`sched_miss`, `sched_late_us_avg`, `sched_late_us_max`)
  - forced-full cause split.
- Disabled idle-repair invalidation path (`TryIdleRepairInvalidate`) to remove cadence disturbance during motion.
- Removed duplicate interval gate in `TrySendBeginFrame` so render-loop cadence controls send rate directly.
- Tested and reverted `--disable-gpu-vsync` (A/B showed regression with larger consumer gaps).

### Why
- Verify whether host scheduling/producer throttling still caused frame drops after queue/fence fixes.
- Separate host pacing issues from UE consumer cadence limits with explicit per-window metrics.

### Impact
- Host producer now reaches near-target cadence in steady windows (`sent_fps/paint_fps ~59-60`).
- Remaining visible stutter was traced to UE-side consumption cadence (editor viewport running near ~40 FPS), not host transport/backpressure.

---

## 2026-04-17 12:52

### Changed
- Improved input responsiveness path in `OsrHandler::PumpInput()`:
  - mouse-move input now participates in input-triggered begin-frame nudging (not only popup-visible moves).
  - bounded per-pump processing to `64` events to avoid long single-pass drain stalls.
  - added periodic nudge during burst processing (`every 8 events`) plus final nudge.
  - reused existing nudge rate gate (`m_lastBeginFrameUs`) to avoid aggressive over-requesting.

### Why
- User-observed lag pattern showed slider drag and high-rate typing could freeze visual updates until queue drain or mouse release.

### Impact
- Better interactive responsiveness under continuous input while keeping frame request throttling bounded.
