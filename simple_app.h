#pragma once
#include "include/cef_app.h"

class SimpleApp : public CefApp, public CefBrowserProcessHandler
{
public:
    SimpleApp() = default;
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnContextInitialized() override;

private:
    IMPLEMENT_REFCOUNTING(SimpleApp);
};