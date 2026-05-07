#include "cef_browser_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"

void CefHostBrowserApp::OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line)
{
	(void)process_type;
	if (!command_line)
		return;

	// Keep compositor cadence stable in headless/windowless mode.
	command_line->AppendSwitch("disable-background-timer-throttling");
	command_line->AppendSwitch("disable-renderer-backgrounding");
	command_line->AppendSwitch("disable-backgrounding-occluded-windows");
	// Prevent native occlusion heuristics from pausing rendering when no HWND is shown.
	command_line->AppendSwitchWithValue("disable-features", "CalculateNativeWinOcclusion");
}

void CefHostBrowserApp::OnContextInitialized()
{
	CefRefPtr<OsrHandler> handler = new OsrHandler(
		m_config.Width, m_config.Height, m_config.SessionId, static_cast<uint32_t>(m_config.FrameRate));
	handler->SetThreadTuningEnabled(m_config.EnableThreadTuning);
	handler->SetCadenceFeedbackEnabled(m_config.EnableCadenceFeedback);
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
