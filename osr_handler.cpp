#include "osr_handler.h"
#include "include/cef_browser.h"

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
    m_frameBuffer.Shutdown();
    m_inputBuffer.Shutdown();
}

void OsrHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
    rect = CefRect(0, 0, static_cast<int>(m_width), static_cast<int>(m_height));
}

void OsrHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
    const RectList& dirty_rects, const void* buffer,
    int width, int height)
{
    if (type != PET_VIEW) return;

    const size_t data_size = static_cast<size_t>(width) * height * 4;
    m_frameBuffer.WriteFrame(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        buffer,
        data_size
    );
}

void OsrHandler::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
}

void OsrHandler::PumpInput(CefRefPtr<CefBrowser> browser)
{
    InputEvent evt;
    while (m_inputBuffer.ReadEvent(evt))
    {
        CefRefPtr<CefBrowserHost> host = browser->GetHost();

        switch (evt.type)
        {
        case InputEventType::MouseMove:
        {
            CefMouseEvent mouse_evt;
            mouse_evt.x = evt.mouse.x;
            mouse_evt.y = evt.mouse.y;
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
            host->SendMouseClickEvent(mouse_evt, btn, is_up, 1);
            break;
        }
        case InputEventType::MouseScroll:
        {
            CefMouseEvent mouse_evt;
            mouse_evt.x = evt.scroll.x;
            mouse_evt.y = evt.scroll.y;
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
            host->SendKeyEvent(key_evt);
            break;
        }
        case InputEventType::KeyChar:
        {
            CefKeyEvent key_evt;
            key_evt.type = KEYEVENT_CHAR;
            key_evt.windows_key_code = evt.char_event.character;
            host->SendKeyEvent(key_evt);
            break;
        }
        }
    }
}