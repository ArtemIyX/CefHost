#pragma once
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_display_handler.h"
#include "shm/SharedFrameBuffer.h"
#include "shm/SharedInputBuffer.h"
#include "shm/SharedControlBuffer.h"
#include "D3D11Device.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>

using Microsoft::WRL::ComPtr;

class OsrHandler : public CefClient, public CefLifeSpanHandler, public CefRenderHandler,
	public CefContextMenuHandler, public CefDisplayHandler, public CefLoadHandler
{
public:
	OsrHandler(uint32_t width, uint32_t height, uint32_t targetFps = 60);
	~OsrHandler() = default;

	CefRefPtr<CefRenderHandler>      GetRenderHandler()      override { return this; }
	CefRefPtr<CefLifeSpanHandler>    GetLifeSpanHandler()    override { return this; }
	CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
	CefRefPtr<CefDisplayHandler>     GetDisplayHandler()     override { return this; }
	CefRefPtr<CefLoadHandler>        GetLoadHandler()        override { return this; }

	void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override;
	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code) override;
	void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code, const CefString& error_text, const CefString& failed_url) override;

	bool OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor,
		cef_cursor_type_t type, const CefCursorInfo& custom_cursor_info) override;

	void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model) override;

	void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
	void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override;

	void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;

	void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
		const RectList& dirtyRects, const CefAcceleratedPaintInfo& info) override;

	void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
		const RectList& dirty_rects, const void* buffer, int width, int height) override {
	}

	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

	void Resize(uint32_t width, uint32_t height);
	void SetThreadTuningEnabled(bool enabled) { m_enableThreadTuning = enabled; }
	void SetCadenceFeedbackEnabled(bool enabled) { m_enableCadenceFeedback = enabled; }
	bool Init();
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
	bool EnsureSharedTextures(uint32_t width, uint32_t height, bool* outRecreated = nullptr);

	uint32_t              m_width;
	uint32_t              m_height;
	SharedFrameBuffer     m_frameBuffer;
	SharedInputBuffer     m_inputBuffer;
	SharedControlBuffer   m_controlBuffer;
	CefRefPtr<CefBrowser> m_browser;
	std::thread           m_renderThread;
	std::thread           m_inputThread;
	std::thread           m_controlThread;
	std::atomic<bool>     m_running{ false };
	std::atomic<bool>     m_inputEnabled{ true };
	uint32_t              m_mouseModifiers{ 0 };
	std::atomic<bool>     m_paused{ false };
	std::atomic<uint64_t> m_lastBeginFrameUs{ 0 };
	std::atomic<uint64_t> m_beginFrameIntervalNs{ 16666667ULL };
	std::atomic<uint32_t> m_smoothedConsumerCadenceUs{ 0 };
	std::atomic<bool>     m_forceFullFrame{ true };
	bool                  m_enableThreadTuning{ true };
	bool                  m_enableCadenceFeedback{ false };
	std::atomic<uint64_t> m_lastTelemetryLogUs{ 0 };
	std::atomic<uint64_t> m_statProducedFrames{ 0 };
	std::atomic<uint64_t> m_statForcedFullFrames{ 0 };
	std::atomic<uint64_t> m_statDirtyRectCountSum{ 0 };
	std::atomic<uint64_t> m_statDirtyRectAreaSum{ 0 };
	std::atomic<uint64_t> m_statCopySubmitUsSum{ 0 };
	std::atomic<uint64_t> m_statCopySubmitUsMax{ 0 };
	std::atomic<uint64_t> m_lastPublishUs{ 0 };
	std::atomic<uint64_t> m_lastDirtyPublishUs{ 0 };
	std::atomic<uint64_t> m_lastRepairInvalidateUs{ 0 };
	std::atomic<bool>     m_waitingIdleRepair{ false };
	uint64_t              m_nextFrameId{ 1 };
	uint32_t              m_keyframeInterval{ 120 };
	uint32_t              m_warmupFullFrames{ 3 };

	CefRect              m_popupRect;
	CefRect              m_popupClearRect;   // area to refresh from cefTexture after popup hides
	std::atomic<bool>    m_popupVisible{ false };

	// Popup GPU texture
	ComPtr<ID3D11Texture2D> m_popupTexture;
	std::mutex              m_popupTextureMutex;
	uint32_t                m_popupTexWidth  = 0;
	uint32_t                m_popupTexHeight = 0;

	// Shared texture ring
	static constexpr uint32_t BUFFER_COUNT = SHM_FRAME_SLOT_COUNT;
	std::array<ComPtr<ID3D11Texture2D>, BUFFER_COUNT> m_sharedTexture;
	std::array<HANDLE, BUFFER_COUNT>                  m_sharedNTHandle{};
	uint32_t                m_writeSlot = 0;
	uint32_t                m_sharedWidth = 0;
	uint32_t                m_sharedHeight = 0;
	std::mutex              m_textureMutex;

	ComPtr<ID3D11Device1> m_device1;

	// Cache last opened shared texture per paint type to avoid OpenSharedResource1 on every frame
	HANDLE                  m_cachedHandleView  = nullptr;
	ComPtr<ID3D11Texture2D> m_cachedTextureView;
	HANDLE                  m_cachedHandlePopup = nullptr;
	ComPtr<ID3D11Texture2D> m_cachedTexturePopup;

	IMPLEMENT_REFCOUNTING(OsrHandler);
};
