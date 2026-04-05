#pragma once
#include "include/cef_app.h"
#include "osr_handler.h"

class SimpleApp : public CefApp, public CefBrowserProcessHandler
{
public:
    SimpleApp() = default;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnContextInitialized() override;

private:
    IMPLEMENT_REFCOUNTING(SimpleApp);
};