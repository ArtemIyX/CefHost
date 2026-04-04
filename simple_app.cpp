#include "simple_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"

void SimpleApp::OnContextInitialized()
{
    CefWindowInfo window_info;
    window_info.SetAsPopup(nullptr, "Host");

    CefBrowserSettings browser_settings;
    CefBrowserHost::CreateBrowser(window_info, nullptr, "https://www.google.com", browser_settings, nullptr, nullptr);
}