#include "osr_handler.h"
#include "include/cef_browser.h"
#include "D3D11Device.h"
#include <cstdio>
#include <dxgi1_2.h>

extern D3D11Device g_D3D11Device;

OsrHandler::OsrHandler(uint32_t width, uint32_t height)
    : m_width(width), m_height(height)
{
}

bool OsrHandler::Init()
{
    if (!m_frameBuffer.Init())  return false;
    if (!m_inputBuffer.Init())  return false;
    if (!m_controlBuffer.Init()) return false;

    // Cache ID3D11Device1 for OpenSharedResource1
    HRESULT hr = g_D3D11Device.GetDevice()->QueryInterface(IID_PPV_ARGS(&m_device1));
    if (FAILED(hr))
    {
        fprintf(stderr, "[OsrHandler] QueryInterface ID3D11Device1 failed: 0x%08X\n", hr);
        return false;
    }

    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header)
    {
        header->cef_pid = GetCurrentProcessId();
        header->shared_handle[0] = 0;
        header->shared_handle[1] = 0;
        header->write_slot = 0;
    }

    return true;
}

void OsrHandler::Shutdown()
{
    StopRenderLoop();

    {
        std::lock_guard<std::mutex> lock(m_textureMutex);
        for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
        {
            if (m_sharedNTHandle[i])
            {
                CloseHandle(m_sharedNTHandle[i]);
                m_sharedNTHandle[i] = nullptr;
            }
            m_sharedTexture[i].Reset();
        }
    }

    m_frameBuffer.Shutdown();
    m_inputBuffer.Shutdown();
    m_controlBuffer.Shutdown();
}

bool OsrHandler::EnsureSharedTextures(uint32_t width, uint32_t height)
{
    if (m_sharedTexture[0] && m_sharedTexture[1] &&
        m_sharedWidth == width && m_sharedHeight == height)
        return true;

    // Release old
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
    {
        if (m_sharedNTHandle[i])
        {
            CloseHandle(m_sharedNTHandle[i]);
            m_sharedNTHandle[i] = nullptr;
        }
        m_sharedTexture[i].Reset();
    }

    ID3D11Device* device = g_D3D11Device.GetDevice();
    if (!device) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
    {
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_sharedTexture[i]);
        if (FAILED(hr))
        {
            fprintf(stderr, "[OsrHandler] CreateTexture2D[%u] failed: 0x%08X\n", i, hr);
            return false;
        }

        ComPtr<IDXGIResource1> dxgiRes;
        hr = m_sharedTexture[i].As(&dxgiRes);
        if (FAILED(hr))
        {
            fprintf(stderr, "[OsrHandler] IDXGIResource1[%u] failed: 0x%08X\n", i, hr);
            return false;
        }

        hr = dxgiRes->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &m_sharedNTHandle[i]);
        if (FAILED(hr))
        {
            fprintf(stderr, "[OsrHandler] CreateSharedHandle[%u] failed: 0x%08X\n", i, hr);
            return false;
        }
    }

    m_sharedWidth = width;
    m_sharedHeight = height;

    // Write both handles into shared memory so UE can open them
    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header)
    {
        header->shared_handle[0] = reinterpret_cast<uint64_t>(m_sharedNTHandle[0]);
        header->shared_handle[1] = reinterpret_cast<uint64_t>(m_sharedNTHandle[1]);
        header->width = width;
        header->height = height;
    }

    fprintf(stdout, "[OsrHandler] Double-buffered shared textures created %ux%u\n", width, height);
    return true;
}

void OsrHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
    const RectList& dirtyRects, const CefAcceleratedPaintInfo& info)
{
    if (type != PET_VIEW) return;

    ID3D11DeviceContext* context = g_D3D11Device.GetContext();
    if (!m_device1 || !context) return;

    // Open CEF's transient texture
    ComPtr<ID3D11Texture2D> cefTexture;
    HRESULT hr = m_device1->OpenSharedResource1(info.shared_texture_handle, IID_PPV_ARGS(&cefTexture));
    if (FAILED(hr))
    {
        fprintf(stderr, "[OsrHandler] OpenSharedResource1 failed: 0x%08X\n", hr);
        return;
    }

    D3D11_TEXTURE2D_DESC cefDesc;
    cefTexture->GetDesc(&cefDesc);

    std::lock_guard<std::mutex> lock(m_textureMutex);

    if (!EnsureSharedTextures(cefDesc.Width, cefDesc.Height))
        return;

    // Write into the back buffer (opposite of last completed slot)
    const uint32_t backSlot = 1u - m_writeSlot;

    context->CopyResource(m_sharedTexture[backSlot].Get(), cefTexture.Get());
    context->Flush();

    // Flip: back becomes front
    m_writeSlot = backSlot;

    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header)
    {
        header->write_slot = m_writeSlot; // UE reads from this slot
        header->sequence++;
        SetEvent(m_frameBuffer.GetEvent());
    }
}

void OsrHandler::StartRenderLoop()
{
    m_running = true;

    m_renderThread = std::thread([this]()
        {
            while (m_running)
            {
                CefRefPtr<CefBrowser> browser = m_browser;
                if (browser && !m_paused)
                    browser->GetHost()->Invalidate(PET_VIEW);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });

    m_inputThread = std::thread([this]()
        {
            while (m_running)
            {
                WaitForSingleObject(m_inputBuffer.GetEvent(), 100);
                if (m_running) PumpInput();
            }
        });

    m_controlThread = std::thread([this]()
        {
            while (m_running)
            {
                WaitForSingleObject(m_controlBuffer.GetEvent(), 100);
                if (m_running) PumpControl();
            }
        });
}

void OsrHandler::StopRenderLoop()
{
    m_running = false;

    SetEvent(m_inputBuffer.GetEvent());
    SetEvent(m_controlBuffer.GetEvent());

    if (m_renderThread.joinable())  m_renderThread.join();
    if (m_inputThread.joinable())   m_inputThread.join();
    if (m_controlThread.joinable()) m_controlThread.join();
}

void OsrHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type)
{
    if (!frame->IsMain()) return;
    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header) header->load_state = CefLoadState::Loading;
}

void OsrHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code)
{
    if (!frame->IsMain()) return;
    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header)
        header->load_state = (http_status_code >= 400) ? CefLoadState::Error : CefLoadState::Ready;
}

void OsrHandler::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    ErrorCode error_code, const CefString& error_text, const CefString& failed_url)
{
    if (!frame->IsMain()) return;
    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header) header->load_state = CefLoadState::Error;
}

bool OsrHandler::OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor,
    cef_cursor_type_t type, const CefCursorInfo& custom_cursor_info)
{
    FrameHeader* header = m_frameBuffer.GetHeader();
    if (header)
        header->cursor_type = static_cast<CefCursorType>(type);
    return true;
}

void OsrHandler::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model)
{
    model->Clear();
}

void OsrHandler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
{
    m_popupVisible = show;
    if (!show) { m_popupBuffer.clear(); m_popupRect = {}; }
}

void OsrHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect)
{
    m_popupRect = rect;
}

void OsrHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
    rect = CefRect(0, 0, static_cast<int>(m_width), static_cast<int>(m_height));
}

void OsrHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    m_browser = browser;
    browser->GetHost()->SetFocus(true);
    StartRenderLoop();
}

void OsrHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
    StopRenderLoop();
    m_browser = nullptr;
}

void OsrHandler::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    if (m_browser)
        m_browser->GetHost()->WasResized();
}

void OsrHandler::PumpInput()
{
    CefRefPtr<CefBrowser> browser = m_browser;
    if (!browser) return;
    CefRefPtr<CefBrowserHost> host = browser->GetHost();
    InputEvent evt;
    while (m_inputBuffer.ReadEvent(evt))
    {
        if (!m_inputEnabled) continue;
        switch (evt.type)
        {
        case InputEventType::MouseMove:
        {
            CefMouseEvent e; e.x = evt.mouse.x; e.y = evt.mouse.y;
            host->SendMouseMoveEvent(e, false);
            break;
        }
        case InputEventType::MouseDown:
        case InputEventType::MouseUp:
        {
            CefMouseEvent e; e.x = evt.mouse.x; e.y = evt.mouse.y;
            host->SendMouseClickEvent(e,
                static_cast<CefBrowserHost::MouseButtonType>(evt.mouse.button),
                evt.type == InputEventType::MouseUp, 1);
            break;
        }
        case InputEventType::MouseScroll:
        {
            CefMouseEvent e; e.x = evt.scroll.x; e.y = evt.scroll.y;
            host->SendMouseWheelEvent(e,
                static_cast<int>(evt.scroll.delta_x),
                static_cast<int>(evt.scroll.delta_y));
            break;
        }
        case InputEventType::KeyDown:
        case InputEventType::KeyUp:
        {
            CefKeyEvent e;
            e.type = (evt.type == InputEventType::KeyDown) ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
            e.windows_key_code = static_cast<int>(evt.key.windows_key_code);
            e.modifiers = evt.key.modifiers;
            host->SendKeyEvent(e);
            break;
        }
        case InputEventType::KeyChar:
        {
            CefKeyEvent e;
            e.type = KEYEVENT_CHAR;
            e.windows_key_code = evt.char_event.character;
            host->SendKeyEvent(e);
            break;
        }
        }
    }
}

void OsrHandler::PumpControl()
{
    CefRefPtr<CefBrowser> browser = m_browser;
    if (!browser) return;
    CefRefPtr<CefBrowserHost> host = browser->GetHost();

    ControlEvent evt;
    while (m_controlBuffer.ReadEvent(evt))
    {
        switch (evt.type)
        {
        case ControlEventType::GoBack:      browser->GoBack();   break;
        case ControlEventType::GoForward:   browser->GoForward(); break;
        case ControlEventType::StopLoad:    browser->StopLoad(); break;
        case ControlEventType::Reload:      browser->Reload();   break;
        case ControlEventType::SetURL:
            browser->GetMainFrame()->LoadURL(CefString(evt.string.text)); break;
        case ControlEventType::SetPaused:   m_paused = evt.flag.value; break;
        case ControlEventType::SetHidden:   host->WasHidden(evt.flag.value); break;
        case ControlEventType::SetFocus:    host->SetFocus(evt.flag.value); break;
        case ControlEventType::SetZoomLevel:
            host->SetZoomLevel(static_cast<double>(evt.zoom.value)); break;
        case ControlEventType::SetFrameRate:
            host->SetWindowlessFrameRate(static_cast<int>(evt.frame_rate.value)); break;
        case ControlEventType::ScrollTo:
            browser->GetMainFrame()->ExecuteJavaScript(
                CefString("window.scrollTo(" + std::to_string(evt.scroll.x) + "," +
                    std::to_string(evt.scroll.y) + ")"), CefString(), 0);
            break;
        case ControlEventType::Resize:
            Resize(evt.resize.width, evt.resize.height); break;
        case ControlEventType::SetMuted:
            host->SetAudioMuted(evt.flag.value); break;
        case ControlEventType::OpenDevTools:
        {
            CefWindowInfo wi; wi.SetAsPopup(nullptr, "DevTools");
            host->ShowDevTools(wi, nullptr, CefBrowserSettings(), CefPoint());
        }
        break;
        case ControlEventType::CloseDevTools: host->CloseDevTools(); break;
        case ControlEventType::SetInputEnabled: m_inputEnabled = evt.flag.value; break;
        case ControlEventType::ExecuteJS:
            browser->GetMainFrame()->ExecuteJavaScript(
                CefString(evt.string.text), CefString(), 0); break;
        case ControlEventType::ClearCookies:
            CefCookieManager::GetGlobalManager(nullptr)->DeleteCookies(
                CefString(), CefString(), nullptr); break;
        }
    }
}