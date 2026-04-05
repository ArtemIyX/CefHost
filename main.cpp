#include <iostream>
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "simple_app.h"

int main(int argc, char* argv[])
{
    CefMainArgs main_args(::GetModuleHandle(nullptr));

    CefRefPtr<SimpleApp> app(new SimpleApp());

    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0)
        return exit_code;

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;

    CefInitialize(main_args, settings, app, nullptr);
    CefRunMessageLoop();
    CefShutdown();

    return 0;
}