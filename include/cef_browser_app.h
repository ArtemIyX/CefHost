#pragma once
#include "include/cef_app.h"
#include "osr_handler.h"
#include "host_runtime_config.h"

// Browser process entrypoint that creates one OSR browser with runtime config.
class CefHostBrowserApp : public CefApp, public CefBrowserProcessHandler
{
public:
    explicit CefHostBrowserApp(const HostRuntimeConfig& config) : m_config(config) {}

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnContextInitialized() override;

private:
    HostRuntimeConfig m_config;
    IMPLEMENT_REFCOUNTING(CefHostBrowserApp);
};
