#pragma once
#include "include/cef_app.h"
#include "osr_handler.h"
#include "host_runtime_config.h"

/**
 * @brief Browser-process CEF app that boots one OSR browser instance.
 *
 * Uses HostRuntimeConfig to configure runtime behavior before browser creation.
 */
class CefHostBrowserApp : public CefApp, public CefBrowserProcessHandler
{
public:
    /**
     * @brief Constructs the app with immutable runtime config.
     * @param config Runtime options parsed from process arguments.
     */
    explicit CefHostBrowserApp(const HostRuntimeConfig &config) : m_config(config) {}

    /**
     * @brief Returns browser-process handler for this CefApp.
     * @return This instance as CefBrowserProcessHandler.
     */
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    /**
     * @brief Applies command-line switches before CEF process startup.
     * @param process_type Empty for browser process; non-empty for subprocesses.
     * @param command_line Mutable CEF command line.
     */
    void OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line) override;

    /**
     * @brief Creates OSR handler and browser once CEF context is ready.
     */
    void OnContextInitialized() override;

private:
    HostRuntimeConfig m_config;
    IMPLEMENT_REFCOUNTING(CefHostBrowserApp);
};
