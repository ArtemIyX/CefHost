#include "D3D11Device.h"
#include "include/cef_browser.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_cookie.h"
#include "include/cef_frame.h"
#include "include/cef_load_handler.h"
#include "include/cef_menu_model.h"
#include "include/cef_render_handler.h"
#include "include/internal/cef_ptr.h"
#include "include/internal/cef_string.h"
#include "include/internal/cef_types.h"
#include "include/internal/cef_types_wrappers.h"
#include "include/internal/cef_win.h"
#include "osr_handler.h"
#include "shm/SharedMemoryLayout.h"
#include <chrono>
#include <combaseapi.h>
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgiformat.h>
#include <handleapi.h>
#include <mutex>
#include <string>
#include <synchapi.h>
#include <thread>
#include <Windows.h>
#include <wrl/client.h>

namespace
{
ULONG_PTR SelectAffinityMask(ULONG_PTR processMask, uint32_t logicalIndex)
{
	if (processMask == 0) return 0;
	uint32_t count = 0;
	for (uint32_t i = 0; i < sizeof(ULONG_PTR) * 8; ++i)
	{
		if (processMask & (static_cast<ULONG_PTR>(1) << i))
			++count;
	}
	if (count == 0) return 0;
	const uint32_t target = logicalIndex % count;
	uint32_t seen = 0;
	for (uint32_t i = 0; i < sizeof(ULONG_PTR) * 8; ++i)
	{
		const ULONG_PTR bit = static_cast<ULONG_PTR>(1) << i;
		if ((processMask & bit) == 0) continue;
		if (seen == target) return bit;
		++seen;
	}
	return 0;
}

void TryPinCurrentThread(uint32_t logicalIndex)
{
	ULONG_PTR processMask = 0, systemMask = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
		return;
	const ULONG_PTR mask = SelectAffinityMask(processMask, logicalIndex);
	if (mask != 0)
		SetThreadAffinityMask(GetCurrentThread(), mask);
}
}


extern D3D11Device g_D3D11Device;

OsrHandler::OsrHandler(uint32_t width, uint32_t height, uint32_t targetFps)
	: m_width(width), m_height(height)
{
	UpdateBeginFrameIntervalFromFps(targetFps);
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
	{
		header->protocol_magic = SHM_PROTOCOL_MAGIC;
		header->version = SHM_PROTOCOL_VERSION;
		header->slot_count = BUFFER_COUNT;
		header->write_slot = 0;
		header->sequence = 0;
		header->frame_id = 0;
		header->present_id = 0;
		header->flags = FRAME_FLAG_FULL_FRAME;
		header->dirty_count = 0;
	}

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

	m_cachedTextureView.Reset();  m_cachedHandleView  = nullptr;
	m_cachedTexturePopup.Reset(); m_cachedHandlePopup = nullptr;

	m_frameBuffer.Shutdown();
	m_inputBuffer.Shutdown();
	m_controlBuffer.Shutdown();
}

bool OsrHandler::EnsureSharedTextures(uint32_t width, uint32_t height, bool* outRecreated)
{
	if (outRecreated) *outRecreated = false;

	bool ready = (m_sharedWidth == width && m_sharedHeight == height);
	if (ready)
	{
		for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
		{
			if (!m_sharedTexture[i])
			{
				ready = false;
				break;
			}
		}
	}
	if (ready)
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
	m_writeSlot = 0;
	m_forceFullFrame.store(true, std::memory_order_relaxed);
	m_warmupFullFrames = 3;
	if (outRecreated) *outRecreated = true;

	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
	{
		header->protocol_magic = SHM_PROTOCOL_MAGIC;
		header->version = SHM_PROTOCOL_VERSION;
		header->slot_count = BUFFER_COUNT;
		header->width = width;
		header->height = height;
		header->write_slot = m_writeSlot;
		header->flags = FRAME_FLAG_FULL_FRAME | FRAME_FLAG_RESIZED;
		header->dirty_count = 0;
	}

	fprintf(stdout, "[OsrHandler] Shared textures created: slots=%u size=%ux%u\n", BUFFER_COUNT, width, height);
	return true;
}

void OsrHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
	const RectList& dirtyRects, const CefAcceleratedPaintInfo& info)
{
	using namespace std::chrono;
	const uint64_t copyStartUs = duration_cast<microseconds>(
		steady_clock::now().time_since_epoch()).count();

	ID3D11DeviceContext* context = g_D3D11Device.GetContext();
	if (!m_device1 || !context) return;

	auto& cachedHandle  = (type == PET_POPUP) ? m_cachedHandlePopup  : m_cachedHandleView;
	auto& cachedTexture = (type == PET_POPUP) ? m_cachedTexturePopup : m_cachedTextureView;

	ComPtr<ID3D11Texture2D> cefTexture;
	if (info.shared_texture_handle != cachedHandle || !cachedTexture)
	{
		HRESULT hr = m_device1->OpenSharedResource1(info.shared_texture_handle, IID_PPV_ARGS(&cefTexture));
		if (FAILED(hr))
		{
			fprintf(stderr, "[OsrHandler] OpenSharedResource1 failed: 0x%08X\n", hr);
			return;
		}
		cachedHandle  = info.shared_texture_handle;
		cachedTexture = cefTexture;
	}
	else
	{
		cefTexture = cachedTexture;
	}

	D3D11_TEXTURE2D_DESC cefDesc;
	cefTexture->GetDesc(&cefDesc);

	if (type == PET_POPUP)
	{
		// Only cache popup content. PET_VIEW composites it into shared textures,
		// eliminating the duplicate Flush/writeSlot-flip/SetEvent per frame.
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
			HRESULT hr = g_D3D11Device.GetDevice()->CreateTexture2D(&desc, nullptr, &m_popupTexture);
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

	bool recreated = false;
	if (!EnsureSharedTextures(cefDesc.Width, cefDesc.Height, &recreated))
		return;

	const uint32_t backSlot = (m_writeSlot + 1u) % BUFFER_COUNT;

	// Full copy CEF texture to back buffer. The old approach (dirty-rect copy +
	// post-flip sync-back) raced with UE5: the sync-back wrote to the old front
	// buffer while the consumer was still reading it, causing ghosting artifacts.
	// Full copy costs ~16us at 1080p and eliminates the race entirely.
	context->CopyResource(m_sharedTexture[backSlot].Get(), cefTexture.Get());

	DirtyRect collectedRects[MAX_DIRTY_RECTS];
	uint32_t  nRects   = 0;
	bool      overflow = false;

	auto addDirty = [&](int x, int y, int w, int h)
	{
		if (nRects < MAX_DIRTY_RECTS)
			collectedRects[nRects++] = { x, y, w, h };
		else
			overflow = true;
	};

	for (const auto& r : dirtyRects)
		addDirty(r.x, r.y, r.width, r.height);

	// Track popup area as dirty for consumer
	{
		const CefRect& pr = m_popupVisible ? m_popupRect : m_popupClearRect;
		if (pr.width > 0 && pr.height > 0)
		{
			addDirty(pr.x, pr.y, pr.width, pr.height);
			if (!m_popupVisible)
				m_popupClearRect = {};
		}
	}

	// Composite popup on back buffer
	if (m_popupVisible)
	{
		std::lock_guard<std::mutex> popupLock(m_popupTextureMutex);
		if (m_popupTexture && m_popupRect.width > 0 && m_popupRect.height > 0)
		{
			D3D11_BOX box = {
				0, 0, 0,
				static_cast<UINT>(m_popupRect.width), static_cast<UINT>(m_popupRect.height), 1
			};
			context->CopySubresourceRegion(m_sharedTexture[backSlot].Get(), 0,
				m_popupRect.x, m_popupRect.y, 0, m_popupTexture.Get(), 0, &box);
		}
	}

	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
	{
		uint32_t frameFlags = FRAME_FLAG_NONE;
		bool forceFull = m_forceFullFrame.exchange(false, std::memory_order_relaxed);
		if (recreated)
			frameFlags |= FRAME_FLAG_RESIZED;
		if (m_warmupFullFrames > 0)
		{
			forceFull = true;
			--m_warmupFullFrames;
		}
		if (m_keyframeInterval > 0 && (m_nextFrameId % m_keyframeInterval) == 0)
			forceFull = true;
		if (overflow)
		{
			forceFull = true;
			frameFlags |= FRAME_FLAG_OVERFLOW;
		}

		if (forceFull)
		{
			frameFlags |= FRAME_FLAG_FULL_FRAME;
			header->dirty_count = 0;
			m_statForcedFullFrames.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			frameFlags |= FRAME_FLAG_DIRTY_ONLY;
			header->dirty_count = static_cast<uint8_t>(nRects);
			for (uint32_t i = 0; i < nRects; ++i)
			{
				header->dirty_rects[i] = collectedRects[i];
				const uint64_t area = static_cast<uint64_t>(collectedRects[i].w > 0 ? collectedRects[i].w : 0) *
					static_cast<uint64_t>(collectedRects[i].h > 0 ? collectedRects[i].h : 0);
				m_statDirtyRectAreaSum.fetch_add(area, std::memory_order_relaxed);
			}
			m_statDirtyRectCountSum.fetch_add(nRects, std::memory_order_relaxed);
		}

		context->Flush();
		m_writeSlot = backSlot;

		header->version = SHM_PROTOCOL_VERSION;
		header->protocol_magic = SHM_PROTOCOL_MAGIC;
		header->slot_count = BUFFER_COUNT;
		header->width = cefDesc.Width;
		header->height = cefDesc.Height;
		header->write_slot = m_writeSlot;
		header->flags = frameFlags;
		header->present_id = m_nextFrameId;
		header->frame_id = m_nextFrameId++;
		std::atomic_thread_fence(std::memory_order_release);
		header->sequence++;
		SetEvent(m_frameBuffer.GetEvent());

		m_statProducedFrames.fetch_add(1, std::memory_order_relaxed);
		const uint64_t copyEndUs = duration_cast<microseconds>(
			steady_clock::now().time_since_epoch()).count();
		const uint64_t copySubmitUs = (copyEndUs > copyStartUs) ? (copyEndUs - copyStartUs) : 0;
		m_statCopySubmitUsSum.fetch_add(copySubmitUs, std::memory_order_relaxed);
		uint64_t prevMax = m_statCopySubmitUsMax.load(std::memory_order_relaxed);
		while (copySubmitUs > prevMax &&
			!m_statCopySubmitUsMax.compare_exchange_weak(prevMax, copySubmitUs, std::memory_order_relaxed))
		{
		}

		uint64_t lastLogUs = m_lastTelemetryLogUs.load(std::memory_order_relaxed);
		if (lastLogUs == 0)
		{
			m_lastTelemetryLogUs.store(copyEndUs, std::memory_order_relaxed);
		}
		else if (copyEndUs - lastLogUs >= 2000000ULL &&
			m_lastTelemetryLogUs.compare_exchange_weak(lastLogUs, copyEndUs, std::memory_order_relaxed))
		{
			const uint64_t produced = m_statProducedFrames.exchange(0, std::memory_order_relaxed);
			const uint64_t forced = m_statForcedFullFrames.exchange(0, std::memory_order_relaxed);
			const uint64_t dirtyCount = m_statDirtyRectCountSum.exchange(0, std::memory_order_relaxed);
			const uint64_t dirtyArea = m_statDirtyRectAreaSum.exchange(0, std::memory_order_relaxed);
			const uint64_t copyUsSum = m_statCopySubmitUsSum.exchange(0, std::memory_order_relaxed);
			const uint64_t copyUsMax = m_statCopySubmitUsMax.exchange(0, std::memory_order_relaxed);
			const uint64_t avgCopyUs = (produced > 0) ? (copyUsSum / produced) : 0;
			const uint64_t avgDirtyRects = (produced > 0) ? (dirtyCount / produced) : 0;
			const uint64_t avgDirtyArea = (produced > 0) ? (dirtyArea / produced) : 0;
			fprintf(stdout,
				"[OsrTelemetry] frames=%llu forced_full=%llu dirty_rects_avg=%llu dirty_area_avg=%llu copy_us_avg=%llu copy_us_max=%llu\n",
				static_cast<unsigned long long>(produced),
				static_cast<unsigned long long>(forced),
				static_cast<unsigned long long>(avgDirtyRects),
				static_cast<unsigned long long>(avgDirtyArea),
				static_cast<unsigned long long>(avgCopyUs),
				static_cast<unsigned long long>(copyUsMax));
		}
	}
}

void OsrHandler::TrySendBeginFrame()
{
	using namespace std::chrono;
	uint64_t now = duration_cast<microseconds>(
		steady_clock::now().time_since_epoch()).count();
	uint64_t prev = m_lastBeginFrameUs.load(std::memory_order_relaxed);
	if (now - prev < 1000) return;
	if (!m_lastBeginFrameUs.compare_exchange_weak(prev, now, std::memory_order_relaxed))
		return;
	CefRefPtr<CefBrowser> b = m_browser;
	if (b) b->GetHost()->SendExternalBeginFrame();
}

void OsrHandler::UpdateBeginFrameIntervalFromFps(uint32_t fps)
{
	const uint32_t clamped = (fps < 1u) ? 1u : (fps > 240u ? 240u : fps);
	const uint64_t intervalNs = 1000000000ULL / static_cast<uint64_t>(clamped);
	m_beginFrameIntervalNs.store(intervalNs, std::memory_order_relaxed);
	m_smoothedConsumerCadenceUs.store(0, std::memory_order_relaxed);
}

void OsrHandler::UpdateBeginFrameIntervalFromConsumerCadenceUs(uint32_t cadenceUs)
{
	if (cadenceUs == 0) return;

	const uint32_t minUs = 1000000u / 240u;
	const uint32_t maxUs = 1000000u / 15u;
	uint32_t clamped = cadenceUs;
	if (clamped < minUs) clamped = minUs;
	if (clamped > maxUs) clamped = maxUs;

	const uint32_t prev = m_smoothedConsumerCadenceUs.load(std::memory_order_relaxed);
	const uint32_t smooth = (prev == 0) ? clamped : ((prev * 7u + clamped * 3u) / 10u);
	m_smoothedConsumerCadenceUs.store(smooth, std::memory_order_relaxed);

	// Keep producer slightly slower than consumer cadence to reduce burst/drop feedback loops.
	const uint64_t targetUs = static_cast<uint64_t>(smooth) + static_cast<uint64_t>(smooth) / 20ULL;
	m_beginFrameIntervalNs.store(targetUs * 1000ULL, std::memory_order_relaxed);
}

void OsrHandler::StartRenderLoop()
{
	m_running = true;

	m_renderThread = std::thread([this]()
		{
			if (m_enableThreadTuning)
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
				TryPinCurrentThread(0);
			}

			using clock = std::chrono::steady_clock;
			auto next = clock::now();

			while (m_running)
			{
				const uint64_t intervalNs = m_beginFrameIntervalNs.load(std::memory_order_relaxed);
				const auto kFrame = std::chrono::nanoseconds(static_cast<long long>(intervalNs));
				next += kFrame;
				if (!m_paused)
					TrySendBeginFrame();
				std::this_thread::sleep_until(next);

				// If scheduler drifts badly (pause/stall), resync to avoid burst catch-up.
				const auto now = clock::now();
				if (now > next + kFrame)
					next = now;
			}
		});

	m_inputThread = std::thread([this]()
		{
			if (m_enableThreadTuning)
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
				TryPinCurrentThread(1);
			}

			LARGE_INTEGER freq, start, now;
			QueryPerformanceFrequency(&freq);
			// Spin for 0.5ms before falling back to a blocking wait.
			const LONGLONG spinTicks = freq.QuadPart / 2000;

			while (m_running)
			{
				if (m_inputBuffer.HasPendingEvents())
				{
					PumpInput();
					continue;
				}

				// Spin phase: catch events that arrive within 0.5ms.
				QueryPerformanceCounter(&start);
				bool found = false;
				while (m_running)
				{
					if (m_inputBuffer.HasPendingEvents()) { found = true; break; }
					QueryPerformanceCounter(&now);
					if (now.QuadPart - start.QuadPart >= spinTicks) break;
					YieldProcessor();
				}

				if (found)
					PumpInput();
				else
					WaitForSingleObject(m_inputBuffer.GetEvent(), 100);
			}
		});

	m_controlThread = std::thread([this]()
		{
			if (m_enableThreadTuning)
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
				TryPinCurrentThread(2);
			}
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
	if (!show && m_popupRect.width > 0)
		m_popupClearRect = m_popupRect;
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
	m_forceFullFrame.store(true, std::memory_order_relaxed);
	m_warmupFullFrames = 3;
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
			e.modifiers = m_mouseModifiers;
			host->SendMouseMoveEvent(e, false);
			break;
		}
		case InputEventType::MouseDown:
		case InputEventType::MouseUp:
		{
			auto btn = static_cast<CefBrowserHost::MouseButtonType>(evt.mouse.button);
			bool isUp = evt.type == InputEventType::MouseUp;
			uint32_t flag = (btn == MBT_LEFT)   ? EVENTFLAG_LEFT_MOUSE_BUTTON
			              : (btn == MBT_MIDDLE)  ? EVENTFLAG_MIDDLE_MOUSE_BUTTON
			                                     : EVENTFLAG_RIGHT_MOUSE_BUTTON;
			if (isUp) m_mouseModifiers &= ~flag;
			else      m_mouseModifiers |= flag;
			CefMouseEvent e; e.x = evt.mouse.x; e.y = evt.mouse.y;
			e.modifiers = m_mouseModifiers;
			host->SendMouseClickEvent(e, btn, isUp, 1);
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
	TrySendBeginFrame();
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
			host->SetWindowlessFrameRate(static_cast<int>(evt.frame_rate.value));
			UpdateBeginFrameIntervalFromFps(evt.frame_rate.value);
			break;
		case ControlEventType::SetConsumerCadenceUs:
			UpdateBeginFrameIntervalFromConsumerCadenceUs(evt.cadence_us.value);
			break;
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
