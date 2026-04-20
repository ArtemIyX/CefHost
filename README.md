# CEF_HOST

## Critical Notes (Read First)

- Single-session only right now.
- Running multiple host instances on the same machine/user session is **not supported**.
- Reason: shared memory/events/shared texture handles use fixed global names (`CEFHost_*`, `Global\\CEFHost_*`), so instances collide.
- Use one producer host per machine/session until namespacing is implemented.

## What This Project Is

`CEF_HOST` is a Windows CEF off-screen rendering host designed to feed Unreal Engine Web UI workflows.

It:
- runs Chromium via CEF in windowless mode,
- renders with GPU-accelerated paint (`OnAcceleratedPaint`),
- publishes frame metadata and IPC data through shared memory ring buffers,
- shares D3D11 textures by named NT handles for zero-copy GPU consumer access,
- receives input/control commands from an external consumer (typically an Unreal plugin).

## Primary Target / Intended Pairing

This host is intended to be used with the Unreal Web UI plugin (consumer side), not as a standalone desktop browser app.

Expected integration model:
- `CEF_HOST` process = render producer.
- Unreal plugin = consumer that opens shared textures, reads frame header/rings, and sends input/control events back.

## CEF Version In Use

Current version of  `build/Release/libcef.dll`:
- `146.0.10+g8219561+chromium-146.0.7680.179`

If you need newer CEF builds:
- Official CEF binary builds index: [https://cef-builds.spotifycdn.com/index.html](https://cef-builds.spotifycdn.com/index.html)
- CEF project/docs: [https://bitbucket.org/chromiumembedded/cef](https://bitbucket.org/chromiumembedded/cef)

When upgrading CEF:
- replace the local `cef/` bundle with the new binary distribution,
- reconfigure + rebuild,
- verify host/plugin protocol compatibility after upgrade.

## Quick Start

### 1. Configure

From repo root:

```powershell
cmake -S . -B build
```

### 2. Build

```powershell
cmake --build build --config Release
```

### 3. Run

```powershell
.\build\Release\Host.exe --url https://google.com --size 1920x1080 --fps 60
```

Help:

```powershell
.\build\Release\Host.exe --help
```

## Build System Notes

- CMake project: `Host`
- C++ standard: `C++17`
- Generator/toolchain: MSVC on Windows (Ninja also works)
- CEF root expected at: `./cef`
- Post-build copies:
  - CEF binaries from `cef/(Debug|Release)` to target output dir
  - CEF resources from `cef/Resources` to target output dir

Main target:
- executable: `Host`

Core source files:
- `src/main.cpp`
- `src/cef_browser_app.cpp`
- `src/osr_handler.cpp`
- `src/host_runtime_config.cpp`

## Runtime CLI Options

Supported options:
- `--url <value>`
- `--size <width>x<height>`
- `--width <value>`
- `--height <value>`
- `--fps <1..240>`
- `--no-thread-tuning`
- `--enable-cadence-feedback`
- `-h`, `--help`, `/?`

Defaults:
- `Width=1920`
- `Height=1080`
- `FrameRate=60`
- `StartupUrl=https://google.com/`
- `EnableThreadTuning=true`
- `EnableCadenceFeedback=false`

## Architecture Overview

### Rendering

- CEF windowless mode is enabled.
- Host consumes accelerated paint callbacks.
- CEF shared texture handle is opened via D3D11.
- Host copies into shared texture ring slots and publishes frame metadata.
- Optional dedicated popup plane texture is also published.

### IPC / Shared Memory

Shared channels:
- frame: `CEFHost_Frame` + event `CEFHost_FrameReady`
- input: `CEFHost_Input` + event `CEFHost_InputReady`
- control: `CEFHost_Control` + event `CEFHost_ControlReady`
- console: `CEFHost_Console` + event `CEFHost_ConsoleReady`
- shutdown event: `CEFHost_Shutdown`

Shared GPU objects:
- texture ring handles: `Global\\CEFHost_SharedTex_%u`
- popup plane: `Global\\CEFHost_SharedPopupTex`
- shared fence: `Global\\CEFHost_SharedFence`

Protocol details are defined in:
- `include/shm/SharedMemoryLayout.h`

### Frame Protocol

Important `FrameHeader` fields:
- protocol/version handshake: `protocol_magic`, `version`
- dimensions and sloting: `width`, `height`, `slot_count`, `write_slot`
- sequencing: `sequence`, `frame_id`, `present_id`
- sync: `gpu_fence_value`
- mode flags: full frame / dirty only / overflow / resized / popup plane
- dirty rect payload and popup rect/state
- load state and cursor type

### Threads

`OsrHandler` owns dedicated worker threads:
- render cadence thread
- input pump thread
- control pump thread

Thread tuning can be disabled via `--no-thread-tuning`.

## Unreal Consumer Expectations

Consumer should:
- open shared frame mapping and wait on `CEFHost_FrameReady`,
- open named shared textures by current `write_slot`,
- honor `protocol_magic` and `version`,
- use `gpu_fence_value` when available for safe cross-process texture consumption,
- publish input/control events into corresponding ring buffers,
- handle popup plane if `FRAME_FLAG_POPUP_PLANE` is set.

## Current Limitations

- Single-session only (name collisions across instances).
- Windows-only implementation (Win32 + D3D11).
- Tight coupling with consumer contract in `SharedMemoryLayout.h`.

## Notable Remarks

- This host is optimized for IPC/GPU interop reliability in engine integration scenarios.
- Keep host and Unreal consumer protocol versions in sync after any shared layout change.
- If output appears stale/incorrect, verify:
  - only one host instance is running,
  - handshake fields match,
  - consumer reads correct slot and fence sequencing.

## Dev Convenience

`build.bat` currently:

```bash
@echo off
cmake --build build --config Release
xcopy /E /Y /I build\Release\* "C:\Users\Wellsaik\source\repos\CefUiExample\Plugins\CefWebUi\Source\ThirdParty\Cef\"
```

- builds Release,
- copies output into a local Unreal plugin ThirdParty path (machine-specific).

Adjust that path before using on another machine.

## License

Project is licenced under [MIT](LICENSE)
