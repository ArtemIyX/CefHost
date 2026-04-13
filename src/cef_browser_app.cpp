#include "cef_browser_app.h"
#include "include/cef_browser.h"

void CefHostBrowserApp::OnContextInitialized()
{
    CefRefPtr<OsrHandler> handler = new OsrHandler(
        m_config.Width, m_config.Height, static_cast<uint32_t>(m_config.FrameRate));
    handler->SetThreadTuningEnabled(m_config.EnableThreadTuning);
    if (!handler->Init())
        return;

    // OSR + shared texture mode are required by UE consumer path.
    CefWindowInfo window_info;
    window_info.SetAsWindowless(nullptr);
    window_info.shared_texture_enabled = true;
    window_info.external_begin_frame_enabled = true;

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = m_config.FrameRate;
    browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0);

    CefBrowserHost::CreateBrowser(
        window_info, handler, m_config.StartupUrl,
        browser_settings, nullptr, nullptr);
}
