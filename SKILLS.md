# SKILLS.md - CEF_HOST Knowledge Base

## 0. Critical Constraints (Read First)

- Single-session only.
- Multi-instance/multi-session use is not supported yet because IPC object names are fixed global names (`CEFHost_*`, `Global\\CEFHost_*`) and collide.
- This host is intended to be paired with an Unreal Web UI consumer plugin.

## 1. Project Purpose

`CEF_HOST` is a Windows CEF off-screen rendering host that:
- renders Chromium using GPU-accelerated paint,
- publishes frame metadata through shared memory,
- shares D3D11 textures via named NT handles,
- consumes input/control from external clients through lock-free rings.

Primary consumer: Unreal Engine plugin process.

## 2. Runtime Stack

- CEF windowless rendering.
- D3D11 device/context for texture copy and shared handles.
- Shared memory ring buffers for frame/input/control/console IPC.
- External begin-frame driven cadence loop.

CEF runtime currently used in this workspace:
- `146.0.10+g8219561+chromium-146.0.7680.179` (from `build/Release/libcef.dll`).

## 3. Main Components

### `D3D11Device`
- Global singleton-like owner: `g_D3D11Device`.
- Initializes `ID3D11Device`, `ID3D11DeviceContext`, `IDXGIDevice`, `IDXGIFactory2`.
- Must be initialized before `OsrHandler::Init()`.

### `CefHostBrowserApp`
- Browser process app and process handler.
- Applies anti-throttle Chromium switches.
- Creates one windowless OSR browser with:
  - `shared_texture_enabled = true`
  - `external_begin_frame_enabled = true`

### `OsrHandler`
- Core integration class.
- Implements:
  - `CefRenderHandler`
  - `CefLifeSpanHandler`
  - `CefContextMenuHandler`
  - `CefDisplayHandler`
  - `CefLoadHandler`
- Owns:
  - shared channels (`SharedFrameBuffer`, `SharedInputBuffer`, `SharedControlBuffer`, `SharedConsoleBuffer`)
  - texture ring and popup plane resources
  - render/input/control worker threads
  - cadence/backpressure/telemetry state

## 4. IPC Contract (`SharedMemoryLayout.h`)

This file is the binary protocol contract. Host and consumer must stay in sync.

Channels and names:
- Frame mapping: `CEFHost_Frame`, event `CEFHost_FrameReady`
- Input mapping: `CEFHost_Input`, event `CEFHost_InputReady`
- Control mapping: `CEFHost_Control`, event `CEFHost_ControlReady`
- Console mapping: `CEFHost_Console`, event `CEFHost_ConsoleReady`
- Shutdown event: `CEFHost_Shutdown`
- Shared fence: `Global\\CEFHost_SharedFence`

Shared textures:
- Ring slots: `Global\\CEFHost_SharedTex_0..N-1` (`N = SHM_FRAME_SLOT_COUNT`, currently `3`)
- Popup plane: `Global\\CEFHost_SharedPopupTex`

Important frame metadata:
- `protocol_magic`, `version`, `slot_count`
- `width`, `height`, `write_slot`
- `frame_id`, `present_id`, `sequence`
- `gpu_fence_value`
- `flags` (`FULL_FRAME`, `DIRTY_ONLY`, `OVERFLOW`, `RESIZED`, `POPUP_PLANE`)
- `dirty_rects`, popup state/rect, cursor/load state

## 5. Rendering Pipeline (Current)

Hot path: `OsrHandler::OnAcceleratedPaint`.

High-level flow:
1. Open/reuse CEF shared texture handle.
2. For `PET_VIEW`:
   - ensure shared texture ring exists at current size,
   - copy full CEF texture into next ring slot (race-safe strategy),
   - collect dirty rect metadata and popup metadata,
   - optionally copy popup texture to dedicated popup plane,
   - signal shared fence when available, fallback to flush when needed,
   - publish `FrameHeader` and signal frame event.
3. For `PET_POPUP`:
   - update cached popup texture,
   - in popup-plane mode publish popup-only metadata/event too.

Notes:
- CPU pixel path exists for layout compatibility but GPU path is primary.
- Frame publishes are release-fenced before sequence increment.

## 6. Threads and Scheduling

`OsrHandler` worker threads:
- `RenderThreadMain()`:
  - high-resolution timer + short spin tail,
  - sends external begin frames with in-flight backpressure,
  - keeps cadence from target FPS/cadence controls.
- `InputThreadMain()`:
  - low-latency spin-then-wait strategy,
  - pumps input ring and nudges begin frame on interaction.
- `ControlThreadMain()`:
  - waits on control event and applies commands.

Thread tuning can be disabled with `--no-thread-tuning`.

## 7. Control/Input Features

Control events include:
- navigation: back/forward/stop/reload/url
- state: paused/hidden/focus/muted/input-enabled
- view: resize/scroll/zoom/fps
- tools: open/close devtools
- script/content: execute JS, load HTML string, open local file
- transport tuning: max in-flight begin frames, flush interval, keyframe interval

Input events:
- mouse move/down/up/wheel
- key down/up/char

Console transport:
- CEF console messages are pushed into console ring with level/source/line/message.

## 8. Build and Run

Build command used by project rule:

```powershell
cmake --build build --config Release
```

Runtime args:
- `--url`
- `--size`, `--width`, `--height`
- `--fps`
- `--no-thread-tuning`
- `--enable-cadence-feedback`
- `--help`

## 9. Non-Negotiable Engineering Rules

- Do not break binary compatibility of `SharedMemoryLayout.h` without synchronized consumer update.
- Do not introduce per-frame CPU readback fallback in normal path.
- Keep `OnAcceleratedPaint` and publish ordering correctness-first.
- Keep cross-thread state changes atomic/ordered.
- Keep object names stable unless you also update the consumer.

## 10. Typical Safe Change Workflow

1. Change host code.
2. Rebuild `Release`.
3. Verify frame protocol fields still correct.
4. Verify Unreal consumer still reads slot/flags/fence correctly.
5. Update `PROJECT_LOG.md`.

## 11. Files With Highest Coupling

- `include/shm/SharedMemoryLayout.h`
- `include/osr_handler.h`
- `src/osr_handler.cpp`
- `include/shm/SharedFrameBuffer.h`
- `include/shm/SharedInputBuffer.h`
- `include/shm/SharedControlBuffer.h`
- `include/shm/SharedConsoleBuffer.h`

