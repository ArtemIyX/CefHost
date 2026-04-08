#pragma once
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_display_handler.h"
#include "shm/FrameBuffer.h"
#include "shm/InputBuffer.h"
#include <thread>
#include <atomic>

class OsrHandler : public CefClient, public CefLifeSpanHandler, public CefRenderHandler, public CefContextMenuHandler, public CefDisplayHandler
{
public:
    OsrHandler(uint32_t width, uint32_t height);
    ~OsrHandler() = default;

    // CefClient
    CefRefPtr<CefRenderHandler>  GetRenderHandler()  override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

    bool OnCursorChange(CefRefPtr<CefBrowser> browser,
        CefCursorHandle cursor,
        cef_cursor_type_t type,
        const CefCursorInfo& custom_cursor_info) override;
    // CefContextMenuHandler
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
        CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model) override;

    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override;

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
        const RectList& dirty_rects, const void* buffer,
        int width, int height) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // Resize viewport and notify CEF.
    void Resize(uint32_t width, uint32_t height);

    // Drain input ring and forward events to CEF. Call from any thread.
    void PumpInput();

    bool Init();
    void Shutdown();

private:
    void StartRenderLoop();
    void StopRenderLoop();

    uint32_t              m_width;
    uint32_t              m_height;
    FrameBuffer           m_frameBuffer;
    InputBuffer           m_inputBuffer;
    CefRefPtr<CefBrowser> m_browser;
    std::thread           m_renderThread;
    std::atomic<bool>     m_running{ false };

    CefRect              m_popupRect;
    std::vector<uint8_t> m_popupBuffer;
    std::atomic<bool>    m_popupVisible{ false };

    IMPLEMENT_REFCOUNTING(OsrHandler);
};