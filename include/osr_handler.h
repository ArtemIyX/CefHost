#pragma once
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_display_handler.h"
#include "shm/SharedFrameBuffer.h"
#include "shm/SharedInputBuffer.h"
#include "shm/SharedControlBuffer.h"
#include "shm/SharedConsoleBuffer.h"
#include "D3D11Device.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>

using Microsoft::WRL::ComPtr;

/**
 * @brief Off-screen rendering handler that bridges CEF frames/input with shared memory.
 *
 * Implements CEF client/handler interfaces and publishes GPU-backed frame metadata
 * to a consumer process via shared memory and named shared textures.
 */
class OsrHandler : public CefClient, public CefLifeSpanHandler, public CefRenderHandler, public CefContextMenuHandler, public CefDisplayHandler, public CefLoadHandler
{
public:
	/**
	 * @brief Creates handler with initial view size and target frame cadence.
	 * @param width Initial viewport width in pixels.
	 * @param height Initial viewport height in pixels.
	 * @param targetFps Producer cadence hint (1..240 typical).
	 */
	OsrHandler(uint32_t width, uint32_t height, uint32_t targetFps = 60);
	~OsrHandler() = default;

	/** @brief Returns CEF render handler interface. */
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
	/** @brief Returns CEF lifespan handler interface. */
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	/** @brief Returns CEF context-menu handler interface. */
	CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
	/** @brief Returns CEF display handler interface. */
	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
	/** @brief Returns CEF load handler interface. */
	CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

	/** @brief Notifies main-frame load start. */
	void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override;
	/** @brief Notifies main-frame load completion. */
	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code) override;
	/** @brief Notifies main-frame load failure. */
	void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code, const CefString &error_text, const CefString &failed_url) override;

	/** @brief Publishes cursor type changes to shared frame header. */
	bool OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor,
						cef_cursor_type_t type, const CefCursorInfo &custom_cursor_info) override;

	/** @brief Removes default context-menu entries. */
	void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
							 CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model) override;

	/** @brief Tracks popup visibility to control popup composition path. */
	void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
	/** @brief Updates popup rectangle used for composition/publish. */
	void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect &rect) override;
	/** @brief Forwards JS console events into shared console ring. */
	bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
						  const CefString &message, const CefString &source, int line) override;

	/** @brief Provides current CEF view rectangle. */
	void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override;

	/**
	 * @brief Handles accelerated paint callbacks using shared GPU textures.
	 * @param browser Browser emitting paint.
	 * @param type View or popup paint type.
	 * @param dirtyRects Dirty rectangles from CEF.
	 * @param info Shared-handle metadata for source texture.
	 */
	void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
							const RectList &dirtyRects, const CefAcceleratedPaintInfo &info) override;

	/** @brief Software paint path is unused (GPU path only). */
	void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
				 const RectList &dirty_rects, const void *buffer, int width, int height) override
	{
	}

	/** @brief Starts render/input/control loops when browser host is created. */
	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
	/** @brief Stops worker loops before browser object destruction. */
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

	/**
	 * @brief Applies viewport resize and requests CEF resize notification.
	 * @param width New width in pixels.
	 * @param height New height in pixels.
	 */
	void Resize(uint32_t width, uint32_t height);
	/** @brief Enables/disables thread priority/affinity tuning. */
	void SetThreadTuningEnabled(bool enabled) { m_enableThreadTuning = enabled; }
	/** @brief Enables/disables cadence adaptation from consumer feedback. */
	void SetCadenceFeedbackEnabled(bool enabled) { m_enableCadenceFeedback = enabled; }
	/** @brief Initializes shared memory channels and D3D resources. */
	bool Init();
	/** @brief Releases resources and stops background threads. */
	void Shutdown();

private:
	void PumpInput();
	void PumpControl();
	void TrySendBeginFrame();
	void TryIdleRepairInvalidate();
	void UpdateBeginFrameIntervalFromFps(uint32_t fps);
	void UpdateBeginFrameIntervalFromConsumerCadenceUs(uint32_t cadenceUs);
	void StartRenderLoop();
	void StopRenderLoop();
	bool EnsureSharedTextures(uint32_t width, uint32_t height, bool *outRecreated = nullptr);

	uint32_t m_width;
	uint32_t m_height;
	SharedFrameBuffer m_frameBuffer;
	SharedInputBuffer m_inputBuffer;
	SharedControlBuffer m_controlBuffer;
	SharedConsoleBuffer m_consoleBuffer;
	CefRefPtr<CefBrowser> m_browser;
	std::thread m_renderThread;
	std::thread m_inputThread;
	std::thread m_controlThread;
	std::atomic<bool> m_running{false};
	std::atomic<bool> m_inputEnabled{true};
	uint32_t m_mouseModifiers{0};
	std::atomic<bool> m_paused{false};
	std::atomic<uint64_t> m_lastBeginFrameUs{0};
	std::atomic<uint64_t> m_beginFrameIntervalNs{16666667ULL};
	std::atomic<uint32_t> m_smoothedConsumerCadenceUs{0};
	std::atomic<bool> m_forceFullFrame{true};
	bool m_enableThreadTuning{true};
	bool m_enableCadenceFeedback{false};
	std::atomic<uint64_t> m_lastTelemetryLogUs{0};
	std::atomic<uint64_t> m_lastPaintUs{0};
	std::atomic<uint64_t> m_statProducedFrames{0};
	std::atomic<uint64_t> m_statForcedFullFrames{0};
	std::atomic<uint64_t> m_statForcedFullManual{0};
	std::atomic<uint64_t> m_statForcedFullRecreate{0};
	std::atomic<uint64_t> m_statForcedFullOverflow{0};
	std::atomic<uint64_t> m_statDirtyRectCountSum{0};
	std::atomic<uint64_t> m_statDirtyRectAreaSum{0};
	std::atomic<uint64_t> m_statCopySubmitUsSum{0};
	std::atomic<uint64_t> m_statCopySubmitUsMax{0};
	std::atomic<uint64_t> m_statBeginFramesSentWindow{0};
	std::atomic<uint64_t> m_statBeginToPaintUsSum{0};
	std::atomic<uint64_t> m_statBeginToPaintUsMax{0};
	std::atomic<uint64_t> m_statSchedMissCount{0};
	std::atomic<uint64_t> m_statSchedLateUsSum{0};
	std::atomic<uint64_t> m_statSchedLateUsMax{0};
	std::atomic<uint64_t> m_lastPublishUs{0};
	std::atomic<uint64_t> m_lastDirtyPublishUs{0};
	std::atomic<uint64_t> m_lastRepairInvalidateUs{0};
	std::atomic<bool> m_waitingIdleRepair{false};
	std::atomic<uint64_t> m_beginFramesSent{0};
	std::atomic<uint64_t> m_paintsCompleted{0};
	uint64_t m_framesSinceFlush{0};
	uint64_t m_nextFrameId{1};
	uint64_t m_nextGpuFenceValue{1};
	uint32_t m_keyframeInterval{0};
	std::atomic<uint64_t> m_keyframeIntervalUs{0ULL};
	uint64_t m_lastKeyframeUs{0};
	std::atomic<uint32_t> m_maxInFlightBeginFrames{2};
	uint32_t m_warmupFullFrames{3};
	std::atomic<uint32_t> m_flushIntervalFrames{1};
	bool m_timerPeriodRaised{false};

	CefRect m_popupRect;
	CefRect m_popupClearRect; // area to refresh from cefTexture after popup hides
	std::atomic<bool> m_popupVisible{false};

	// Popup GPU texture
	ComPtr<ID3D11Texture2D> m_popupTexture;
	std::mutex m_popupTextureMutex;
	uint32_t m_popupTexWidth = 0;
	uint32_t m_popupTexHeight = 0;

	// Shared texture ring
	static constexpr uint32_t BUFFER_COUNT = SHM_FRAME_SLOT_COUNT;
	std::array<ComPtr<ID3D11Texture2D>, BUFFER_COUNT> m_sharedTexture;
	std::array<HANDLE, BUFFER_COUNT> m_sharedNTHandle{};
	ComPtr<ID3D11Texture2D> m_sharedPopupTexture;
	HANDLE m_sharedPopupHandle = nullptr;
	bool m_usePopupDedicatedPlane = true;
	uint32_t m_writeSlot = 0;
	uint32_t m_sharedWidth = 0;
	uint32_t m_sharedHeight = 0;
	std::mutex m_textureMutex;

	ComPtr<ID3D11Device1> m_device1;
	ComPtr<ID3D11Device5> m_device5;
	ComPtr<ID3D11DeviceContext4> m_context4;
	ComPtr<ID3D11Fence> m_sharedFence;
	HANDLE m_sharedFenceHandle = nullptr;

	// Cache last opened shared texture per paint type to avoid OpenSharedResource1 on every frame
	HANDLE m_cachedHandleView = nullptr;
	ComPtr<ID3D11Texture2D> m_cachedTextureView;
	HANDLE m_cachedHandlePopup = nullptr;
	ComPtr<ID3D11Texture2D> m_cachedTexturePopup;

	IMPLEMENT_REFCOUNTING(OsrHandler);
};
