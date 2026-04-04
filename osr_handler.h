#pragma once
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "shm/FrameBuffer.h"
#include "shm/InputBuffer.h"

class OsrHandler : public CefClient, public CefRenderHandler
{
public:
    OsrHandler(uint32_t width, uint32_t height);
    ~OsrHandler() = default;

    // CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
        const RectList& dirty_rects, const void* buffer,
        int width, int height) override;

    // Call from main loop to resize
    void Resize(uint32_t width, uint32_t height);

    // Call from main loop to pump input → browser
    void PumpInput(CefRefPtr<CefBrowser> browser);

    bool Init();
    void Shutdown();

private:
    uint32_t    m_width;
    uint32_t    m_height;
    FrameBuffer m_frameBuffer;
    InputBuffer m_inputBuffer;

    IMPLEMENT_REFCOUNTING(OsrHandler);
};