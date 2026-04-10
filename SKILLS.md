# SKILLS.md - Project Knowledge Base

**Project Name:** CEF OSR Host (DirectX 11 Accelerated Shared Texture + Shared Memory IPC)

**Purpose:**  
High-performance, low-latency off-screen rendering of Chromium (CEF) that exposes frames as D3D11 shared textures (named NT handles) and uses lock-free shared memory rings for input/control events. Designed to be embedded in another process (most commonly Unreal Engine 5) with zero-copy GPU texture sharing and minimal CPU overhead.

---

## 1. Core Architecture Overview

- **D3D11Device** (singleton/global `g_D3D11Device`):  
  Creates the D3D11 device + immediate context with `D3D11_CREATE_DEVICE_BGRA_SUPPORT` (and DEBUG layer in debug builds). Also acquires `IDXGIDevice` → `IDXGIAdapter` → `IDXGIFactory2`.

- **OsrHandler** (main class):  
  Implements multiple CEF handlers:
  - `CefRenderHandler` (accelerated paint, GetViewRect, OnPopupShow/Size, OnCursorChange)
  - `CefLoadHandler` (OnLoadStart/End/Error)
  - `CefContextMenuHandler` (disables context menu)
  - `CefLifeSpanHandler` (OnAfterCreated / OnBeforeClose)

- **Shared Memory Triple-Buffer System:**
  - **FrameBuffer** (`CEFHost_Frame`): Contains `FrameHeader` + 2× pixel buffers (double-buffered). GPU path only — CPU path is kept for layout compatibility but unused.
  - **InputBuffer** (`CEFHost_Input`): Lock-free ring buffer (`InputRingBuffer`) for mouse/keyboard events.
  - **ControlBuffer** (`CEFHost_Control`): Lock-free ring buffer (`ControlRingBuffer`) for browser control commands (URL, resize, JS, devtools, etc.).

- **Communication Flow:**
  - Host (this process) renders to D3D11 shared textures → signals `CEFHost_FrameReady` event.
  - Client (UE5) opens named shared textures by name (`Global\CEFHost_SharedTex_0/1`) and reads header from shared memory.
  - Client sends input/control events via the two ring buffers + `CEFHost_InputReady` / `CEFHost_ControlReady` events.

---

## 2. Key Classes & Responsibilities

### D3D11Device.h
- One-time initialization only.
- Must be initialized before any OsrHandler.
- Provides `GetDevice()`, `GetContext()`, `GetDXGIFactory()`.

### OsrHandler (osr_handler.cpp)
**Critical members:**
- `m_device1` (ID3D11Device1) – used for `OpenSharedResource1`
- `m_sharedTexture[2]` + named NT handles (`Global\CEFHost_SharedTex_0/1`)
- `m_popupTexture` (separate texture for popup compositing)
- `m_writeSlot` (0 or 1) – double buffering for frames
- `m_popupVisible`, `m_popupRect`, `m_popupTexWidth/Height`
- Three threads started in `OnAfterCreated`:
  - `m_renderThread` – calls `Invalidate(PET_VIEW)` at ~60 FPS (16 ms sleep)
  - `m_inputThread` – high-priority spin + `WaitForSingleObject` hybrid
  - `m_controlThread` – normal priority, waits on control event

**Important methods:**
- `EnsureSharedTextures()` – creates/recreates double-buffered shared textures with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`
- `OnAcceleratedPaint()` – **the hottest path**:
  - Opens CEF’s shared texture
  - Copies view to back buffer
  - Composites popup on top (if visible) using `CopySubresourceRegion`
  - Flips `m_writeSlot`
  - Updates `FrameHeader` and signals event
- `PumpInput()` / `PumpControl()` – read from ring buffers and forward to CEF

### SharedMemoryLayout.h
Contains **all** IPC data structures (must stay 100% binary compatible with client):

- `FrameHeader` (32 bytes): width, height, sequence, write_slot, cursor_type, load_state
- `InputEvent` + `InputRingBuffer` (256 slots)
- `ControlEvent` + `ControlRingBuffer` (64 slots) — supports strings up to 2048 UTF-16 characters
- All event enums and union layouts

### *Buffer.h files (FrameBuffer, InputBuffer, ControlBuffer)
- `FrameBuffer::WriteFrame` is **NOT** used in GPU path (kept for compatibility).
- All use placement-new for atomics inside shared memory.
- Use `std::memory_order_acquire/release` correctly.
- Events are manual-reset false (auto-reset).

---

## 3. Performance & Low-Latency Rules (Never Violate)

1. **Zero-copy GPU path is mandatory** — never fall back to CPU pixel copy unless debugging.
2. Double-buffering of shared textures + `m_writeSlot` flip must be respected.
3. Popup is composited **on the back buffer** before flip (so last frame always has popup baked in).
4. `context->Flush()` after every paint.
5. Input thread uses **0.5 ms spin + YieldProcessor** before falling back to `WaitForSingleObject`.
6. All shared memory writes use correct release/acquire ordering.
7. `FrameHeader::sequence` is incremented on every new frame (client must check it to detect updates).
8. Named shared handles are created with prefix `Global\CEFHost_SharedTex_%u`.

---

## 4. Coding Style & Rules (Strict)

- Use **modern C++** (C++20/23) but keep it simple and readable.
- Error handling: `fprintf(stderr, "[Class] message: 0x%08X\n", hr);` — exact format required.
- Success messages go to `stdout` with `[Class]` prefix.
- No unnecessary includes.
- Prefer `ComPtr<T>` everywhere.
- Threads are joined cleanly in `Shutdown()` / `OnBeforeClose`.
- Never block the CEF render thread.
- All CEF callbacks that can be called from any thread are handled safely (locks only where needed: texture mutex, popup mutex).
- `m_browser` is stored as `CefRefPtr<CefBrowser>` and nulled in `OnBeforeClose`.
- Use `CefString` for all strings passed to CEF.
- No `using namespace` except the one already in D3D11Device.h.

---

## 5. Unreal Engine 5 Integration Notes

- Client is expected to open the named handles `Global\CEFHost_SharedTex_0` and `_1`.
- Client reads `FrameHeader` from `CEFHost_Frame` shared memory.
- Client must respect double-buffering using `write_slot`.
- All control commands (including `ExecuteJS`, `SetURL`, `Resize`, `SetZoomLevel`, etc.) are sent through `ControlBuffer`.
- Input events are sent through `InputBuffer`.
- DevTools can be opened via control event.

---

## 6. Common Tasks & Patterns

**Adding a new control command:**
1. Add enum value to `ControlEventType`
2. Add field to the union in `ControlEvent`
3. Handle it in `PumpControl()`
4. Update client-side writer accordingly

**Changing frame format:**
- Only `DXGI_FORMAT_B8G8R8A8_UNORM` is supported.
- Update `SHM_FRAME_SIZE` and `SHM_MAX_WIDTH/HEIGHT` if needed.

**Debugging:**
- Look for `[OsrHandler]` and `[D3D11Device]` messages in console.
- In debug builds D3D11 debug layer is enabled.
- `sequence` counter helps detect dropped frames.

---

## 7. Files That Must Stay Synchronized

- `SharedMemoryLayout.h` (IPC contract)
- `FrameBuffer.h` / `InputBuffer.h` / `ControlBuffer.h`
- `osr_handler.cpp` (especially `OnAcceleratedPaint`, `EnsureSharedTextures`, `Pump*` functions)
- `D3D11Device.h`

Any change to shared memory layout **must** be reflected in both host and client (UE5) code.

---

You are now fully briefed on this project.  
When the user asks you to modify, extend, fix, or add features, you must respect **all** the above architecture, performance rules, error formatting, threading model, and shared-memory protocol exactly as implemented.

Start every response with the minimal amount of text necessary and show code first.