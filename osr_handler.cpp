#include "D3D11Device.h"
#include "include/cef_browser.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_cookie.h"
#include "include/cef_cookie.h"
#include "include/cef_cookie.h"
#include "include/cef_frame.h"
#include "include/cef_frame.h"
#include "include/cef_frame.h"
#include "include/cef_load_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_menu_model.h"
#include "include/cef_menu_model.h"
#include "include/cef_menu_model.h"
#include "include/cef_render_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_render_handler.h"
#include "include/internal/cef_ptr.h"
#include "include/internal/cef_ptr.h"
#include "include/internal/cef_ptr.h"
#include "include/internal/cef_string.h"
#include "include/internal/cef_string.h"
#include "include/internal/cef_string.h"
#include "include/internal/cef_types.h"
#include "include/internal/cef_types.h"
#include "include/internal/cef_types.h"
#include "include/internal/cef_types_wrappers.h"
#include "include/internal/cef_types_wrappers.h"
#include "include/internal/cef_types_wrappers.h"
#include "include/internal/cef_win.h"
#include "include/internal/cef_win.h"
#include "include/internal/cef_win.h"
#include "osr_handler.h"
#include "shm/SharedMemoryLayout.h"
#include "shm/SharedMemoryLayout.h"
#include "shm/SharedMemoryLayout.h"
#include <chrono>
#include <chrono>
#include <chrono>
#include <combaseapi.h>
#include <combaseapi.h>
#include <combaseapi.h>
#include <cstdint>
#include <cstdint>
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <d3d11.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgiformat.h>
#include <dxgiformat.h>
#include <dxgiformat.h>
#include <handleapi.h>
#include <handleapi.h>
#include <handleapi.h>
#include <mutex>
#include <mutex>
#include <mutex>
#include <string>
#include <string>
#include <string>
#include <synchapi.h>
#include <synchapi.h>
#include <synchapi.h>
#include <thread>
#include <thread>
#include <thread>
#include <Windows.h>
#include <Windows.h>
#include <Windows.h>
#include <wrl/client.h>
#include <wrl/client.h>
#include <wrl/client.h>

extern D3D11Device g_D3D11Device;

OsrHandler::OsrHandler(uint32_t width, uint32_t height)
	: m_width(width), m_height(height)
{
}

bool OsrHandler::Init()
{
	if (!m_frameBuffer.Init())   return false;
	if (!m_inputBuffer.Init())   return false;
	if (!m_controlBuffer.Init()) return false;

	HRESULT hr = g_D3D11Device.GetDevice()->QueryInterface(IID_PPV_ARGS(&m_device1));
	if (FAILED(hr))
	{
		fprintf(stderr, "[OsrHandler] QueryInterface ID3D11Device1 failed: 0x%08X\n", hr);
		return false;
	}

	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
		header->write_slot = 0;

	return true;
}

void OsrHandler::Shutdown()
{
	StopRenderLoop();

	{
		std::lock_guard<std::mutex> lock(m_textureMutex);
		for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
		{
			if (m_sharedNTHandle[i])
			{
				CloseHandle(m_sharedNTHandle[i]);
				m_sharedNTHandle[i] = nullptr;
			}
			m_sharedTexture[i].Reset();
		}
	}

	m_frameBuffer.Shutdown();
	m_inputBuffer.Shutdown();
	m_controlBuffer.Shutdown();
}

bool OsrHandler::EnsureSharedTextures(uint32_t width, uint32_t height)
{
	if (m_sharedTexture[0] && m_sharedTexture[1] &&
		m_sharedWidth == width && m_sharedHeight == height)
		return true;

	for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
	{
		if (m_sharedNTHandle[i])
		{
			CloseHandle(m_sharedNTHandle[i]);
			m_sharedNTHandle[i] = nullptr;
		}
		m_sharedTexture[i].Reset();
	}

	ID3D11Device* device = g_D3D11Device.GetDevice();
	if (!device) return false;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

	for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
	{
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_sharedTexture[i]);
		if (FAILED(hr))
		{
			fprintf(stderr, "[OsrHandler] CreateTexture2D[%u] failed: 0x%08X\n", i, hr);
			return false;
		}

		ComPtr<IDXGIResource1> dxgiRes;
		hr = m_sharedTexture[i].As(&dxgiRes);
		if (FAILED(hr))
		{
			fprintf(stderr, "[OsrHandler] IDXGIResource1[%u] failed: 0x%08X\n", i, hr);
			return false;
		}

		// Named handle � UE opens by name, no duplication needed
		wchar_t name[64];
		swprintf(name, 64, L"Global\\CEFHost_SharedTex_%u", i);

		hr = dxgiRes->CreateSharedHandle(
			nullptr, DXGI_SHARED_RESOURCE_READ, name, &m_sharedNTHandle[i]);
		if (FAILED(hr))
		{
			fprintf(stderr, "[OsrHandler] CreateSharedHandle[%u] failed: 0x%08X\n", i, hr);
			return false;
		}

		fprintf(stdout, "[OsrHandler] Created named shared handle[%u]: %ls\n", i, name);
	}

	m_sharedWidth = width;
	m_sharedHeight = height;

	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
	{
		header->width = width;
		header->height = height;
	}

	fprintf(stdout, "[OsrHandler] Double-buffered shared textures created %ux%u\n", width, height);
	return true;
}

void OsrHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
	const RectList& dirtyRects, const CefAcceleratedPaintInfo& info)
{
	ID3D11DeviceContext* context = g_D3D11Device.GetContext();
	if (!m_device1 || !context) return;

	ComPtr<ID3D11Texture2D> cefTexture;
	HRESULT hr = m_device1->OpenSharedResource1(info.shared_texture_handle, IID_PPV_ARGS(&cefTexture));
	if (FAILED(hr))
	{
		fprintf(stderr, "[OsrHandler] OpenSharedResource1 failed: 0x%08X\n", hr);
		return;
	}

	D3D11_TEXTURE2D_DESC cefDesc;
	cefTexture->GetDesc(&cefDesc);

	if (type == PET_POPUP)
	{
		std::lock_guard<std::mutex> popupLock(m_popupTextureMutex);

		if (!m_popupTexture || m_popupTexWidth != cefDesc.Width || m_popupTexHeight != cefDesc.Height)
		{
			m_popupTexture.Reset();
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width            = cefDesc.Width;
			desc.Height           = cefDesc.Height;
			desc.MipLevels        = 1;
			desc.ArraySize        = 1;
			desc.Format           = cefDesc.Format;
			desc.SampleDesc.Count = 1;
			desc.Usage            = D3D11_USAGE_DEFAULT;
			desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
			hr = g_D3D11Device.GetDevice()->CreateTexture2D(&desc, nullptr, &m_popupTexture);
			if (FAILED(hr))
			{
				fprintf(stderr, "[OsrHandler] CreateTexture2D popup failed: 0x%08X\n", hr);
				return;
			}
			m_popupTexWidth  = cefDesc.Width;
			m_popupTexHeight = cefDesc.Height;
		}

		context->CopyResource(m_popupTexture.Get(), cefTexture.Get());
		return;
	}

	if (type != PET_VIEW) return;

	std::lock_guard<std::mutex> lock(m_textureMutex);

	if (!EnsureSharedTextures(cefDesc.Width, cefDesc.Height))
		return;

	// Copy current write slot (preserves last frame with baked popup) then overwrite with fresh view
	const uint32_t backSlot = 1u - m_writeSlot;
	context->CopyResource(m_sharedTexture[backSlot].Get(), cefTexture.Get());

	// Composite popup on top if visible
	if (m_popupVisible)
	{
		std::lock_guard<std::mutex> popupLock(m_popupTextureMutex);
		if (m_popupTexture && m_popupRect.width > 0 && m_popupRect.height > 0)
		{
			D3D11_BOX srcBox = {};
			srcBox.left   = 0;
			srcBox.top    = 0;
			srcBox.front  = 0;
			srcBox.right  = static_cast<UINT>(m_popupRect.width);
			srcBox.bottom = static_cast<UINT>(m_popupRect.height);
			srcBox.back   = 1;
			context->CopySubresourceRegion(
				m_sharedTexture[backSlot].Get(), 0,
				static_cast<UINT>(m_popupRect.x), static_cast<UINT>(m_popupRect.y), 0,
				m_popupTexture.Get(), 0, &srcBox);
		}
	}

	context->Flush();

	// Flip
	m_writeSlot = backSlot;

	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
	{
		header->write_slot = m_writeSlot;
		header->sequence++;
		SetEvent(m_frameBuffer.GetEvent());
	}
}

void OsrHandler::StartRenderLoop()
{
	m_running = true;

	m_renderThread = std::thread([this]()
		{
			while (m_running)
			{
				CefRefPtr<CefBrowser> browser = m_browser;
				if (browser && !m_paused)
					browser->GetHost()->Invalidate(PET_VIEW);
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}
		});

	m_inputThread = std::thread([this]()
		{
			while (m_running)
			{
				WaitForSingleObject(m_inputBuffer.GetEvent(), 100);
				if (m_running) PumpInput();
			}
		});

	m_controlThread = std::thread([this]()
		{
			while (m_running)
			{
				WaitForSingleObject(m_controlBuffer.GetEvent(), 100);
				if (m_running) PumpControl();
			}
		});
}

void OsrHandler::StopRenderLoop()
{
	m_running = false;

	SetEvent(m_inputBuffer.GetEvent());
	SetEvent(m_controlBuffer.GetEvent());

	if (m_renderThread.joinable())  m_renderThread.join();
	if (m_inputThread.joinable())   m_inputThread.join();
	if (m_controlThread.joinable()) m_controlThread.join();
}

void OsrHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type)
{
	if (!frame->IsMain()) return;
	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header) header->load_state = CefLoadState::Loading;
}

void OsrHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code)
{
	if (!frame->IsMain()) return;
	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
		header->load_state = (http_status_code >= 400) ? CefLoadState::Error : CefLoadState::Ready;
}

void OsrHandler::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	ErrorCode error_code, const CefString& error_text, const CefString& failed_url)
{
	if (!frame->IsMain()) return;
	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header) header->load_state = CefLoadState::Error;
}

bool OsrHandler::OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor,
	cef_cursor_type_t type, const CefCursorInfo& custom_cursor_info)
{
	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
		header->cursor_type = static_cast<CefCursorType>(type);
	return true;
}

void OsrHandler::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model)
{
	model->Clear();
}

void OsrHandler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
{
	m_popupVisible = show;
	if (!show) { m_popupRect = {}; }
}

void OsrHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect)
{
	m_popupRect = rect;
}

void OsrHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
	rect = CefRect(0, 0, static_cast<int>(m_width), static_cast<int>(m_height));
}

void OsrHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	m_browser = browser;
	browser->GetHost()->SetFocus(true);
	StartRenderLoop();
}

void OsrHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	StopRenderLoop();
	m_browser = nullptr;
}

void OsrHandler::Resize(uint32_t width, uint32_t height)
{
	m_width = width;
	m_height = height;
	if (m_browser)
		m_browser->GetHost()->WasResized();
}

void OsrHandler::PumpInput()
{
	CefRefPtr<CefBrowser> browser = m_browser;
	if (!browser) return;
	CefRefPtr<CefBrowserHost> host = browser->GetHost();
	InputEvent evt;
	while (m_inputBuffer.ReadEvent(evt))
	{
		if (!m_inputEnabled) continue;
		switch (evt.type)
		{
		case InputEventType::MouseMove:
		{
			CefMouseEvent e; e.x = evt.mouse.x; e.y = evt.mouse.y;
			host->SendMouseMoveEvent(e, false);
			break;
		}
		case InputEventType::MouseDown:
		case InputEventType::MouseUp:
		{
			CefMouseEvent e; e.x = evt.mouse.x; e.y = evt.mouse.y;
			host->SendMouseClickEvent(e,
				static_cast<CefBrowserHost::MouseButtonType>(evt.mouse.button),
				evt.type == InputEventType::MouseUp, 1);
			break;
		}
		case InputEventType::MouseScroll:
		{
			CefMouseEvent e; e.x = evt.scroll.x; e.y = evt.scroll.y;
			host->SendMouseWheelEvent(e,
				static_cast<int>(evt.scroll.delta_x),
				static_cast<int>(evt.scroll.delta_y));
			break;
		}
		case InputEventType::KeyDown:
		case InputEventType::KeyUp:
		{
			CefKeyEvent e;
			e.type = (evt.type == InputEventType::KeyDown) ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
			e.windows_key_code = static_cast<int>(evt.key.windows_key_code);
			e.modifiers = evt.key.modifiers;
			host->SendKeyEvent(e);
			break;
		}
		case InputEventType::KeyChar:
		{
			CefKeyEvent e;
			e.type = KEYEVENT_CHAR;
			e.windows_key_code = evt.char_event.character;
			host->SendKeyEvent(e);
			break;
		}
		}
	}
}

void OsrHandler::PumpControl()
{
	CefRefPtr<CefBrowser> browser = m_browser;
	if (!browser) return;
	CefRefPtr<CefBrowserHost> host = browser->GetHost();

	ControlEvent evt;
	while (m_controlBuffer.ReadEvent(evt))
	{
		switch (evt.type)
		{
		case ControlEventType::GoBack:      browser->GoBack();    break;
		case ControlEventType::GoForward:   browser->GoForward(); break;
		case ControlEventType::StopLoad:    browser->StopLoad();  break;
		case ControlEventType::Reload:      browser->Reload();    break;
		case ControlEventType::SetURL:
			browser->GetMainFrame()->LoadURL(CefString(evt.string.text)); break;
		case ControlEventType::SetPaused:    m_paused = evt.flag.value; break;
		case ControlEventType::SetHidden:    host->WasHidden(evt.flag.value); break;
		case ControlEventType::SetFocus:     host->SetFocus(evt.flag.value); break;
		case ControlEventType::SetZoomLevel:
			host->SetZoomLevel(static_cast<double>(evt.zoom.value)); break;
		case ControlEventType::SetFrameRate:
			host->SetWindowlessFrameRate(static_cast<int>(evt.frame_rate.value)); break;
		case ControlEventType::ScrollTo:
			browser->GetMainFrame()->ExecuteJavaScript(
				CefString("window.scrollTo(" + std::to_string(evt.scroll.x) + "," +
					std::to_string(evt.scroll.y) + ")"), CefString(), 0);
			break;
		case ControlEventType::Resize:
			Resize(evt.resize.width, evt.resize.height); break;
		case ControlEventType::SetMuted:
			host->SetAudioMuted(evt.flag.value); break;
		case ControlEventType::OpenDevTools:
		{
			CefWindowInfo wi; wi.SetAsPopup(nullptr, "DevTools");
			host->ShowDevTools(wi, nullptr, CefBrowserSettings(), CefPoint());
		}
		break;
		case ControlEventType::CloseDevTools:   host->CloseDevTools(); break;
		case ControlEventType::SetInputEnabled: m_inputEnabled = evt.flag.value; break;
		case ControlEventType::ExecuteJS:
			browser->GetMainFrame()->ExecuteJavaScript(
				CefString(evt.string.text), CefString(), 0); break;
		case ControlEventType::ClearCookies:
			CefCookieManager::GetGlobalManager(nullptr)->DeleteCookies(
				CefString(), CefString(), nullptr); break;
		}
	}
}