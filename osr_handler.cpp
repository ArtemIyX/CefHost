#include "osr_handler.h"
#include "include/cef_browser.h"
#include <cstdio>

OsrHandler::OsrHandler(uint32_t width, uint32_t height)
    : m_width(width), m_height(height)
{
}

bool OsrHandler::Init()
{
    if (!m_frameBuffer.Init()) return false;
    if (!m_inputBuffer.Init()) return false;
    return true;
}

void OsrHandler::Shutdown()
{
    StopRenderLoop();
    m_frameBuffer.Shutdown();
    m_inputBuffer.Shutdown();
}

void OsrHandler::StartRenderLoop()
{
    m_running = true;
    m_renderThread = std::thread([this]()
        {
            while (m_running)
            {
                // m_browser is set before StartRenderLoop() and cleared in OnBeforeClose()
                // which only fires after CEF teardown — safe to read without lock here.
                CefRefPtr<CefBrowser> browser = m_browser;
                if (browser) 
                {
                    browser->GetHost()->Invalidate(PET_VIEW);
                    PumpInput();
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 fps
            }
        });
}

void OsrHandler::StopRenderLoop()
{
    m_running = false;
    if (m_renderThread.joinable())
        m_renderThread.join();
}

bool OsrHandler::OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor, cef_cursor_type_t type, const CefCursorInfo& custom_cursor_info)
{
    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header)
        header->cursor_type = static_cast<CefCursorType>(type);
    return true;
}

void OsrHandler::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model)
{

}

void OsrHandler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
{
    m_popupVisible = show;
    if (!show)
    {
        m_popupBuffer.clear();
        m_popupRect = {};
    }
}

void OsrHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect)
{
    m_popupRect = rect;
}

void OsrHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
    rect = CefRect(0, 0, static_cast<int>(m_width), static_cast<int>(m_height));
}

void OsrHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
    const RectList& dirty_rects, const void* buffer,
    int width, int height)
{
    if (type == PET_POPUP)
    {
        const size_t size = static_cast<size_t>(width) * height * 4;
        m_popupBuffer.assign(static_cast<const uint8_t*>(buffer),
            static_cast<const uint8_t*>(buffer) + size);
        return;
    }

    // PET_VIEW — copy main frame then composite popup on top
    const size_t viewSize = static_cast<size_t>(width) * height * 4;
    std::vector<uint8_t> composite(static_cast<const uint8_t*>(buffer),
        static_cast<const uint8_t*>(buffer) + viewSize);

    if (m_popupVisible && !m_popupBuffer.empty())
    {
        const int popW = m_popupRect.width;
        const int popH = m_popupRect.height;
        const int popX = m_popupRect.x;
        const int popY = m_popupRect.y;

        for (int row = 0; row < popH; ++row)
        {
            const int dstY = popY + row;
            if (dstY < 0 || dstY >= height) continue;

            const int srcOffset = row * popW * 4;
            const int dstOffset = (dstY * width + popX) * 4;
            const int copyBytes = std::min(popW, width - popX) * 4;
            if (copyBytes <= 0) continue;

            std::memcpy(composite.data() + dstOffset,
                m_popupBuffer.data() + srcOffset,
                copyBytes);
        }
    }

   /* std::printf("[OSR] OnPaint: %dx%d popup=%s\n", width, height,
        m_popupVisible ? "yes" : "no");
    fflush(stdout);*/

    m_frameBuffer.WriteFrame(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        composite.data(),
        composite.size()
    );
}

void OsrHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    m_browser = browser;
    browser->GetHost()->SetFocus(true);
    StartRenderLoop();
}

void OsrHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
    // Stop the render loop before releasing the browser ref so the
    // thread can't call Invalidate() on a destroyed host.
    StopRenderLoop();
    m_browser = nullptr;
}

void OsrHandler::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    if (m_browser)
        m_browser->GetHost()->WasResized(); // triggers GetViewRect + repaint
}

void OsrHandler::PumpInput()
{
    CefRefPtr<CefBrowser> browser = m_browser;
    if (!browser) return;
    CefRefPtr<CefBrowserHost> host = browser->GetHost();
    InputEvent evt;
    while (m_inputBuffer.ReadEvent(evt))
    {
        switch (evt.type)
        {
        case InputEventType::MouseMove:
        {
            CefMouseEvent mouse_evt;
            mouse_evt.x = evt.mouse.x;
            mouse_evt.y = evt.mouse.y;
            std::printf("[INPUT] MouseMove x=%d y=%d\n", mouse_evt.x, mouse_evt.y);
            host->SendMouseMoveEvent(mouse_evt, false);
            break;
        }
        case InputEventType::MouseDown:
        case InputEventType::MouseUp:
        {
            CefMouseEvent mouse_evt;
            mouse_evt.x = evt.mouse.x;
            mouse_evt.y = evt.mouse.y;
            const bool is_up = (evt.type == InputEventType::MouseUp);
            const auto btn = static_cast<CefBrowserHost::MouseButtonType>(evt.mouse.button);
            std::printf("[INPUT] Mouse%s x=%d y=%d btn=%d\n",
                is_up ? "Up" : "Down", mouse_evt.x, mouse_evt.y, static_cast<int>(btn));
            host->SendMouseClickEvent(mouse_evt, btn, is_up, 1);
            break;
        }
        case InputEventType::MouseScroll:
        {
            CefMouseEvent mouse_evt;
            mouse_evt.x = evt.scroll.x;
            mouse_evt.y = evt.scroll.y;
            std::printf("[INPUT] MouseScroll x=%d y=%d dx=%.1f dy=%.1f\n",
                mouse_evt.x, mouse_evt.y, evt.scroll.delta_x, evt.scroll.delta_y);
            host->SendMouseWheelEvent(mouse_evt,
                static_cast<int>(evt.scroll.delta_x),
                static_cast<int>(evt.scroll.delta_y));
            break;
        }
        case InputEventType::KeyDown:
        case InputEventType::KeyUp:
        {
            CefKeyEvent key_evt;
            key_evt.type = (evt.type == InputEventType::KeyDown)
                ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
            key_evt.windows_key_code = static_cast<int>(evt.key.windows_key_code);
            key_evt.modifiers = evt.key.modifiers;
            std::printf("[INPUT] Key%s vk=0x%X mods=0x%X\n",
                evt.type == InputEventType::KeyDown ? "Down" : "Up",
                key_evt.windows_key_code, key_evt.modifiers);
            host->SendKeyEvent(key_evt);
            break;
        }
        case InputEventType::KeyChar:
        {
            CefKeyEvent key_evt;
            key_evt.type = KEYEVENT_CHAR;
            key_evt.windows_key_code = evt.char_event.character;
            std::printf("[INPUT] KeyChar char=0x%X ('%c')\n",
                evt.char_event.character,
                evt.char_event.character >= 32 ? static_cast<char>(evt.char_event.character) : '?');
            host->SendKeyEvent(key_evt);
            break;
        }
        }
        fflush(stdout);
    }
}