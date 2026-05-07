#include <iostream>
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "cef_browser_app.h"
#include "D3d11device.h"
#include "host_runtime_config.h"
#include "shm/SharedMemoryLayout.h"
#include <filesystem>
#include <cstdio>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

D3D11Device g_D3D11Device;

int main(int argc, char* argv[])
{
	// Parse runtime knobs before CEF starts.
	HostRuntimeConfig config = HostRuntimeConfig::FromArgs(argc, argv);
	if (config.ShowHelp)
	{
		HostRuntimeConfig::PrintUsage();
		return 0;
	}

	CefMainArgs main_args(::GetModuleHandle(nullptr));

	CefRefPtr<CefHostBrowserApp> app(new CefHostBrowserApp(config));

	int exit_code = CefExecuteProcess(main_args, app, nullptr);
	if (exit_code >= 0)
		return exit_code;

	if (!g_D3D11Device.Init())
	{
		fprintf(stderr, "[main] Failed to initialize D3D11 device.\n");
		return 1;
	}

	// Raise system timer granularity so begin-frame pacing has lower jitter.
	timeBeginPeriod(1);

	CefSettings settings;
	settings.no_sandbox = true;
	settings.windowless_rendering_enabled = true;
	const std::wstring sessionSuffix = SanitizeSessionId(config.SessionId);

	const char* localAppData = std::getenv("LOCALAPPDATA");
	std::filesystem::path profileBase = localAppData && *localAppData
		? std::filesystem::path(localAppData)
		: std::filesystem::temp_directory_path();
	profileBase /= "CEF_HOST";
	profileBase /= "profiles";
	profileBase /= sessionSuffix.empty() ? std::wstring(L"legacy") : sessionSuffix;
	const std::filesystem::path rootCachePath = profileBase / "root";
	const std::filesystem::path cachePath = rootCachePath / "default";
	std::error_code ec;
	std::filesystem::create_directories(cachePath, ec);
	if (ec)
	{
		fprintf(stderr, "[main] Failed to create profile dirs: %s (err=%d)\n",
			cachePath.string().c_str(), ec.value());
	}

	CefString(&settings.root_cache_path) = rootCachePath.wstring();
	CefString(&settings.cache_path) = cachePath.wstring();
	fprintf(stdout, "[main] SessionId raw='%s' sanitized='%ls'\n",
		config.SessionId.c_str(),
		sessionSuffix.empty() ? L"<empty>" : sessionSuffix.c_str());
	fprintf(stdout, "[main] CEF root_cache_path='%ls'\n", rootCachePath.wstring().c_str());
	fprintf(stdout, "[main] CEF cache_path='%ls'\n", cachePath.wstring().c_str());

	CefInitialize(main_args, settings, app, nullptr);
	CefRunMessageLoop();
	CefShutdown();

	// Must symmetrically restore timer granularity before process exit.
	timeEndPeriod(1);

	g_D3D11Device.Shutdown();

	return 0;
}
