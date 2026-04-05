#include "simple_app.h"
#include "include/cef_browser.h"

void SimpleApp::OnContextInitialized()
{
    CefRefPtr<OsrHandler> handler = new OsrHandler(1920, 1080);
    if (!handler->Init())
        return;

    CefWindowInfo window_info;
    window_info.SetAsWindowless(nullptr);

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 60;

    CefBrowserHost::CreateBrowser(
        window_info, handler, "https://www.google.com",
        browser_settings, nullptr, nullptr);
}