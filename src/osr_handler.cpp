#include "D3D11Device.h"
#include "include/cef_browser.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_cookie.h"
#include "include/cef_frame.h"
#include "include/cef_load_handler.h"
#include "include/cef_menu_model.h"
#include "include/cef_parser.h"
#include "include/cef_render_handler.h"
#include "include/internal/cef_ptr.h"
#include "include/internal/cef_string.h"
#include "include/internal/cef_types.h"
#include "include/internal/cef_types_wrappers.h"
#include "include/internal/cef_win.h"
#include "osr_handler.h"
#include "shm/SharedMemoryLayout.h"
#include <algorithm>
#include <chrono>
#include <combaseapi.h>
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dxgiformat.h>
#include <handleapi.h>
#include <mmsystem.h>
#include <mutex>
#include <string>
#include <synchapi.h>
#include <thread>
#include <Windows.h>
#include <wrl/client.h>

#pragma comment(lib, "winmm.lib")

namespace
{
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
	#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

	ULONG_PTR SelectAffinityMask(ULONG_PTR processMask, uint32_t logicalIndex)
	{
		if (processMask == 0)
			return 0;
		uint32_t count = 0;
		for (uint32_t i = 0; i < sizeof(ULONG_PTR) * 8; ++i)
		{
			if (processMask & (static_cast<ULONG_PTR>(1) << i))
				++count;
		}
		if (count == 0)
			return 0;
		const uint32_t target = logicalIndex % count;
		uint32_t seen = 0;
		for (uint32_t i = 0; i < sizeof(ULONG_PTR) * 8; ++i)
		{
			const ULONG_PTR bit = static_cast<ULONG_PTR>(1) << i;
			if ((processMask & bit) == 0)
				continue;
			if (seen == target)
				return bit;
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

	template <size_t N>
	void CopyCefStringToUtf16Array(const CefString& in, char16_t (&out)[N])
	{
		if constexpr (N == 0)
			return;
		for (size_t i = 0; i < N; ++i)
			out[i] = 0;
		const std::u16string value = in.ToString16();
		const size_t copyCount = (value.size() < (N - 1)) ? value.size() : (N - 1);
		for (size_t i = 0; i < copyCount; ++i)
			out[i] = value[i];
	}

	bool IsAsciiAlpha(char16_t c)
	{
		return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
	}

	std::u16string HexEscapeByte(uint8_t b)
	{
		static constexpr char16_t kHex[] = u"0123456789ABCDEF";
		std::u16string out;
		out.push_back(u'%');
		out.push_back(kHex[(b >> 4) & 0xF]);
		out.push_back(kHex[b & 0xF]);
		return out;
	}

	std::u16string EncodeFileUrlPath(const std::u16string& path)
	{
		// Keep path separators/drive colon intact. Escape only problematic ASCII bytes.
		std::u16string out;
		out.reserve(path.size() + 16);
		for (const char16_t c : path)
		{
			if (c == u' ')
			{
				out += u"%20";
			}
			else if (c == u'#')
			{
				out += u"%23";
			}
			else if (c == u'?')
			{
				out += u"%3F";
			}
			else if (c == u'%')
			{
				out += u"%25";
			}
			else if (c < 0x20)
			{
				out += HexEscapeByte(static_cast<uint8_t>(c));
			}
			else
			{
				out.push_back(c);
			}
		}
		return out;
	}

	CefString MakeFileUrlFromPath(const CefString& rawPath)
	{
		std::u16string path = rawPath.ToString16();
		for (auto& ch : path)
		{
			if (ch == u'\\')
				ch = u'/';
		}

		const std::u16string filePrefix = u"file://";
		if (path.rfind(filePrefix, 0) == 0)
		{
			return CefString(path);
		}

		// UNC path: //server/share/file.html
		if (path.size() >= 2 && path[0] == u'/' && path[1] == u'/')
		{
			return CefString(u"file:" + EncodeFileUrlPath(path));
		}

		// Windows drive path: C:/...
		if (path.size() >= 3 && IsAsciiAlpha(path[0]) && path[1] == u':' && path[2] == u'/')
		{
			return CefString(u"file:///" + EncodeFileUrlPath(path));
		}

		// Absolute path without drive (rare on Windows).
		if (!path.empty() && path[0] == u'/')
		{
			return CefString(u"file://" + EncodeFileUrlPath(path));
		}

		// Fallback: treat as local relative path.
		return CefString(u"file:///" + EncodeFileUrlPath(path));
	}

	const char* ControlEventTypeToString(ControlEventType type)
	{
		switch (type)
		{
			case ControlEventType::GoBack:
				return "GoBack";
			case ControlEventType::GoForward:
				return "GoForward";
			case ControlEventType::StopLoad:
				return "StopLoad";
			case ControlEventType::Reload:
				return "Reload";
			case ControlEventType::SetURL:
				return "SetURL";
			case ControlEventType::SetPaused:
				return "SetPaused";
			case ControlEventType::SetHidden:
				return "SetHidden";
			case ControlEventType::SetFocus:
				return "SetFocus";
			case ControlEventType::SetZoomLevel:
				return "SetZoomLevel";
			case ControlEventType::SetFrameRate:
				return "SetFrameRate";
			case ControlEventType::ScrollTo:
				return "ScrollTo";
			case ControlEventType::Resize:
				return "Resize";
			case ControlEventType::SetMuted:
				return "SetMuted";
			case ControlEventType::OpenDevTools:
				return "OpenDevTools";
			case ControlEventType::CloseDevTools:
				return "CloseDevTools";
			case ControlEventType::SetInputEnabled:
				return "SetInputEnabled";
			case ControlEventType::ExecuteJS:
				return "ExecuteJS";
			case ControlEventType::ClearCookies:
				return "ClearCookies";
			case ControlEventType::SetConsumerCadenceUs:
				return "SetConsumerCadenceUs";
			case ControlEventType::SetMaxInFlightBeginFrames:
				return "SetMaxInFlightBeginFrames";
			case ControlEventType::SetFlushIntervalFrames:
				return "SetFlushIntervalFrames";
			case ControlEventType::SetKeyframeIntervalUs:
				return "SetKeyframeIntervalUs";
			case ControlEventType::OpenLocalFile:
				return "OpenLocalFile";
			case ControlEventType::LoadHtmlString:
				return "LoadHtmlString";
			default:
				return "Unknown";
		}
	}
} // namespace

extern D3D11Device g_D3D11Device;

OsrHandler::OsrHandler(uint32_t width, uint32_t height, uint32_t targetFps)
	: m_width(width), m_height(height)
{
	UpdateBeginFrameIntervalFromFps(targetFps);
}

bool OsrHandler::Init()
{
	if (!m_frameBuffer.Init())
		return false;
	if (!m_inputBuffer.Init())
		return false;
	if (!m_controlBuffer.Init())
		return false;
	if (!m_consoleBuffer.Init())
		return false;

	HRESULT hr = g_D3D11Device.GetDevice()->QueryInterface(IID_PPV_ARGS(&m_device1));
	if (FAILED(hr))
	{
		fprintf(stderr, "[OsrHandler] QueryInterface ID3D11Device1 failed: 0x%08X\n", hr);
		return false;
	}

	ComPtr<ID3D11Device5> device5;
	if (SUCCEEDED(g_D3D11Device.GetDevice()->QueryInterface(IID_PPV_ARGS(&device5))))
	{
		ComPtr<ID3D11DeviceContext4> context4;
		if (SUCCEEDED(g_D3D11Device.GetContext()->QueryInterface(IID_PPV_ARGS(&context4))))
		{
			ComPtr<ID3D11Fence> fence;
			hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
			if (SUCCEEDED(hr))
			{
				HANDLE fenceHandle = nullptr;
				hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, SHM_GPU_FENCE_NAME, &fenceHandle);
				if (SUCCEEDED(hr))
				{
					m_device5 = device5;
					m_context4 = context4;
					m_sharedFence = fence;
					m_sharedFenceHandle = fenceHandle;
					fprintf(stdout, "[OsrHandler] Shared GPU fence created: %ls\n", SHM_GPU_FENCE_NAME);
				}
				else
				{
					fprintf(stderr, "[OsrHandler] CreateSharedHandle(fence) failed: 0x%08X\n", hr);
				}
			}
			else
			{
				fprintf(stderr, "[OsrHandler] CreateFence failed: 0x%08X\n", hr);
			}
		}
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
		header->gpu_fence_value = 0;
		header->flags = FRAME_FLAG_FULL_FRAME;
		header->popup_visible = 0;
		header->popup_rect = { 0, 0, 0, 0 };
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
		if (m_sharedPopupHandle)
		{
			CloseHandle(m_sharedPopupHandle);
			m_sharedPopupHandle = nullptr;
		}
		m_sharedPopupTexture.Reset();
	}

	m_cachedTextureView.Reset();
	m_cachedHandleView = nullptr;
	m_cachedTexturePopup.Reset();
	m_cachedHandlePopup = nullptr;
	if (m_sharedFenceHandle)
	{
		CloseHandle(m_sharedFenceHandle);
		m_sharedFenceHandle = nullptr;
	}
	m_sharedFence.Reset();
	m_context4.Reset();
	m_device5.Reset();

	m_frameBuffer.Shutdown();
	m_inputBuffer.Shutdown();
	m_controlBuffer.Shutdown();
	m_consoleBuffer.Shutdown();
}

bool OsrHandler::EnsureSharedTextures(uint32_t width, uint32_t height, bool* outRecreated)
{
	if (outRecreated)
		*outRecreated = false;

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
	if (!device)
		return false;

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

	// Dedicated popup plane texture (same size as view; UE copies only popup rect).
	{
		if (m_sharedPopupHandle)
		{
			CloseHandle(m_sharedPopupHandle);
			m_sharedPopupHandle = nullptr;
		}
		m_sharedPopupTexture.Reset();

		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_sharedPopupTexture);
		if (FAILED(hr))
		{
			fprintf(stderr, "[OsrHandler] CreateTexture2D popup plane failed: 0x%08X\n", hr);
			return false;
		}

		ComPtr<IDXGIResource1> popupDxgi;
		hr = m_sharedPopupTexture.As(&popupDxgi);
		if (FAILED(hr))
		{
			fprintf(stderr, "[OsrHandler] IDXGIResource1 popup plane failed: 0x%08X\n", hr);
			return false;
		}

		hr = popupDxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ,
			L"Global\\CEFHost_SharedPopupTex", &m_sharedPopupHandle);
		if (FAILED(hr))
		{
			fprintf(stderr, "[OsrHandler] CreateSharedHandle popup plane failed: 0x%08X\n", hr);
			return false;
		}
	}

	m_sharedWidth = width;
	m_sharedHeight = height;
	m_writeSlot = 0;
	m_forceFullFrame.store(true, std::memory_order_relaxed);
	m_warmupFullFrames = 3;
	if (outRecreated)
		*outRecreated = true;

	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
	{
		header->protocol_magic = SHM_PROTOCOL_MAGIC;
		header->version = SHM_PROTOCOL_VERSION;
		header->slot_count = BUFFER_COUNT;
		header->width = width;
		header->height = height;
		header->write_slot = m_writeSlot;
		header->gpu_fence_value = 0;
		header->flags = FRAME_FLAG_FULL_FRAME | FRAME_FLAG_RESIZED;
		header->popup_visible = 0;
		header->popup_rect = { 0, 0, 0, 0 };
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
		steady_clock::now().time_since_epoch())
									 .count();
	m_lastPaintUs.store(copyStartUs, std::memory_order_relaxed);
	const uint64_t lastBeginUs = m_lastBeginFrameUs.load(std::memory_order_relaxed);
	if (lastBeginUs > 0 && copyStartUs >= lastBeginUs)
	{
		const uint64_t beginToPaintUs = copyStartUs - lastBeginUs;
		// Ignore extreme outliers (pause/breakpoint/tab hidden) in moving telemetry.
		if (beginToPaintUs <= 1000000ULL)
		{
			m_statBeginToPaintUsSum.fetch_add(beginToPaintUs, std::memory_order_relaxed);
			uint64_t prevMax = m_statBeginToPaintUsMax.load(std::memory_order_relaxed);
			while (beginToPaintUs > prevMax && !m_statBeginToPaintUsMax.compare_exchange_weak(prevMax, beginToPaintUs, std::memory_order_relaxed))
			{
			}
		}
	}

	ID3D11DeviceContext* context = g_D3D11Device.GetContext();
	if (!m_device1 || !context)
		return;

	auto& cachedHandle = (type == PET_POPUP) ? m_cachedHandlePopup : m_cachedHandleView;
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
		cachedHandle = info.shared_texture_handle;
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
		std::lock_guard<std::mutex> lock(m_textureMutex);
		std::lock_guard<std::mutex> popupLock(m_popupTextureMutex);

		if (!m_popupTexture || m_popupTexWidth != cefDesc.Width || m_popupTexHeight != cefDesc.Height)
		{
			m_popupTexture.Reset();
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = cefDesc.Width;
			desc.Height = cefDesc.Height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = cefDesc.Format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			HRESULT hr = g_D3D11Device.GetDevice()->CreateTexture2D(&desc, nullptr, &m_popupTexture);
			if (FAILED(hr))
			{
				fprintf(stderr, "[OsrHandler] CreateTexture2D popup failed: 0x%08X\n", hr);
				return;
			}
			m_popupTexWidth = cefDesc.Width;
			m_popupTexHeight = cefDesc.Height;
		}

		context->CopyResource(m_popupTexture.Get(), cefTexture.Get());

		// In dedicated popup-plane mode publish popup-only frames as well.
		// Without this, hover-only popup updates can stall until a PET_VIEW arrives.
		if (m_usePopupDedicatedPlane && m_sharedPopupTexture && m_sharedWidth > 0 && m_sharedHeight > 0)
		{
			const bool popupVisible = m_popupVisible.load(std::memory_order_relaxed);
			const CefRect pr = popupVisible ? m_popupRect : m_popupClearRect;
			const int32_t x0 = std::max(0, std::min(pr.x, static_cast<int32_t>(m_sharedWidth)));
			const int32_t y0 = std::max(0, std::min(pr.y, static_cast<int32_t>(m_sharedHeight)));
			const int32_t x1 = std::max(0, std::min(pr.x + pr.width, static_cast<int32_t>(m_sharedWidth)));
			const int32_t y1 = std::max(0, std::min(pr.y + pr.height, static_cast<int32_t>(m_sharedHeight)));
			const bool validRect = (x1 > x0 && y1 > y0);

			if (popupVisible && validRect)
			{
				D3D11_BOX box = {
					0, 0, 0,
					static_cast<UINT>(x1 - x0), static_cast<UINT>(y1 - y0), 1
				};
				context->CopySubresourceRegion(m_sharedPopupTexture.Get(), 0, x0, y0, 0, m_popupTexture.Get(), 0, &box);
			}

			uint64_t gpuFenceValue = 0;
			bool signaledFence = false;
			if (m_context4 && m_sharedFence)
			{
				gpuFenceValue = m_nextGpuFenceValue++;
				HRESULT shr = m_context4->Signal(m_sharedFence.Get(), gpuFenceValue);
				signaledFence = SUCCEEDED(shr);
				if (!signaledFence)
					gpuFenceValue = 0;
			}
			if (!signaledFence)
				context->Flush();

			FrameHeader* header = m_frameBuffer.GetHeader();
			if (header)
			{
				header->version = SHM_PROTOCOL_VERSION;
				header->protocol_magic = SHM_PROTOCOL_MAGIC;
				header->slot_count = BUFFER_COUNT;
				header->width = m_sharedWidth;
				header->height = m_sharedHeight;
				header->write_slot = m_writeSlot;
				header->flags = FRAME_FLAG_POPUP_PLANE | FRAME_FLAG_DIRTY_ONLY;
				header->popup_visible = popupVisible ? 1 : 0;
				header->popup_rect = { m_popupRect.x, m_popupRect.y, m_popupRect.width, m_popupRect.height };
				header->present_id = m_nextFrameId;
				header->gpu_fence_value = gpuFenceValue;
				header->frame_id = m_nextFrameId++;

				if (validRect)
				{
					header->dirty_count = 1;
					header->dirty_rects[0] = { x0, y0, x1 - x0, y1 - y0 };
				}
				else
				{
					header->dirty_count = 0;
				}

				std::atomic_thread_fence(std::memory_order_release);
				header->sequence++;
				SetEvent(m_frameBuffer.GetEvent());
				m_statProducedFrames.fetch_add(1, std::memory_order_relaxed);
				m_paintsCompleted.fetch_add(1, std::memory_order_relaxed);
			}

			if (!popupVisible)
				m_popupClearRect = {};
		}
		return;
	}

	if (type != PET_VIEW)
		return;

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
	uint32_t nRects = 0;
	bool overflow = false;

	auto addDirty = [&](int x, int y, int w, int h) {
		if (nRects < MAX_DIRTY_RECTS)
			collectedRects[nRects++] = { x, y, w, h };
		else
			overflow = true;
	};

	for (const auto& r : dirtyRects)
		addDirty(r.x, r.y, r.width, r.height);

	// Track popup area as dirty for consumer only when popup is composited into main plane.
	if (!m_usePopupDedicatedPlane)
	{
		const CefRect& pr = m_popupVisible ? m_popupRect : m_popupClearRect;
		if (pr.width > 0 && pr.height > 0)
		{
			addDirty(pr.x, pr.y, pr.width, pr.height);
			if (!m_popupVisible)
				m_popupClearRect = {};
		}
	}

	// Composite popup on main back buffer (legacy mode).
	if (!m_usePopupDedicatedPlane && m_popupVisible)
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

	// Dedicated popup plane: copy popup texture into shared popup plane texture.
	if (m_usePopupDedicatedPlane && m_popupVisible)
	{
		std::lock_guard<std::mutex> popupLock(m_popupTextureMutex);
		if (m_sharedPopupTexture && m_popupTexture && m_popupRect.width > 0 && m_popupRect.height > 0)
		{
			D3D11_BOX box = {
				0, 0, 0,
				static_cast<UINT>(m_popupRect.width), static_cast<UINT>(m_popupRect.height), 1
			};
			context->CopySubresourceRegion(m_sharedPopupTexture.Get(), 0,
				m_popupRect.x, m_popupRect.y, 0, m_popupTexture.Get(), 0, &box);
		}
	}

	FrameHeader* header = m_frameBuffer.GetHeader();
	if (header)
	{
		uint32_t frameFlags = FRAME_FLAG_NONE;
		const bool forceFullManual = m_forceFullFrame.exchange(false, std::memory_order_relaxed);
		bool forceFull = forceFullManual;
		bool forceFullRecreate = false;
		bool forceFullOverflow = false;
		if (recreated)
		{
			frameFlags |= FRAME_FLAG_RESIZED;
			forceFull = true;
			forceFullRecreate = true;
		}
		if (m_usePopupDedicatedPlane)
			frameFlags |= FRAME_FLAG_POPUP_PLANE;
		if (m_warmupFullFrames > 0)
		{
			forceFull = true;
			--m_warmupFullFrames;
		}
		if (overflow)
		{
			forceFull = true;
			forceFullOverflow = true;
			frameFlags |= FRAME_FLAG_OVERFLOW;
		}

		if (forceFull)
		{
			frameFlags |= FRAME_FLAG_FULL_FRAME;
			header->dirty_count = 0;
			m_statForcedFullFrames.fetch_add(1, std::memory_order_relaxed);
			if (forceFullManual)
				m_statForcedFullManual.fetch_add(1, std::memory_order_relaxed);
			if (forceFullRecreate)
				m_statForcedFullRecreate.fetch_add(1, std::memory_order_relaxed);
			if (forceFullOverflow)
				m_statForcedFullOverflow.fetch_add(1, std::memory_order_relaxed);
			m_waitingIdleRepair.store(false, std::memory_order_relaxed);
			m_lastKeyframeUs = copyStartUs;
		}
		else
		{
			frameFlags |= FRAME_FLAG_DIRTY_ONLY;
			header->dirty_count = static_cast<uint8_t>(nRects);
			for (uint32_t i = 0; i < nRects; ++i)
			{
				header->dirty_rects[i] = collectedRects[i];
				const uint64_t area = static_cast<uint64_t>(collectedRects[i].w > 0 ? collectedRects[i].w : 0) * static_cast<uint64_t>(collectedRects[i].h > 0 ? collectedRects[i].h : 0);
				m_statDirtyRectAreaSum.fetch_add(area, std::memory_order_relaxed);
			}
			m_statDirtyRectCountSum.fetch_add(nRects, std::memory_order_relaxed);
			m_waitingIdleRepair.store(true, std::memory_order_relaxed);
			m_lastDirtyPublishUs.store(copyStartUs, std::memory_order_relaxed);
		}

		uint64_t gpuFenceValue = 0;
		bool signaledFence = false;
		if (m_context4 && m_sharedFence)
		{
			gpuFenceValue = m_nextGpuFenceValue++;
			HRESULT shr = m_context4->Signal(m_sharedFence.Get(), gpuFenceValue);
			signaledFence = SUCCEEDED(shr);
			if (!signaledFence)
				gpuFenceValue = 0;
		}

		++m_framesSinceFlush;
		const uint32_t flushIntervalFrames = m_flushIntervalFrames.load(std::memory_order_relaxed);
		const bool shouldFlush = !signaledFence || recreated || forceFull || (flushIntervalFrames > 0 && m_framesSinceFlush >= flushIntervalFrames);
		if (shouldFlush)
		{
			context->Flush();
			m_framesSinceFlush = 0;
		}

		m_writeSlot = backSlot;

		header->version = SHM_PROTOCOL_VERSION;
		header->protocol_magic = SHM_PROTOCOL_MAGIC;
		header->slot_count = BUFFER_COUNT;
		header->width = cefDesc.Width;
		header->height = cefDesc.Height;
		header->write_slot = m_writeSlot;
		header->flags = frameFlags;
		header->popup_visible = (m_usePopupDedicatedPlane && m_popupVisible) ? 1 : 0;
		header->popup_rect = { m_popupRect.x, m_popupRect.y, m_popupRect.width, m_popupRect.height };
		header->present_id = m_nextFrameId;
		header->gpu_fence_value = gpuFenceValue;
		header->frame_id = m_nextFrameId++;
		std::atomic_thread_fence(std::memory_order_release);
		header->sequence++;
		SetEvent(m_frameBuffer.GetEvent());

		m_statProducedFrames.fetch_add(1, std::memory_order_relaxed);
		m_paintsCompleted.fetch_add(1, std::memory_order_relaxed);
		const uint64_t copyEndUs = duration_cast<microseconds>(
			steady_clock::now().time_since_epoch())
									   .count();
		const uint64_t copySubmitUs = (copyEndUs > copyStartUs) ? (copyEndUs - copyStartUs) : 0;
		m_lastPublishUs.store(copyEndUs, std::memory_order_relaxed);
		m_statCopySubmitUsSum.fetch_add(copySubmitUs, std::memory_order_relaxed);
		uint64_t prevMax = m_statCopySubmitUsMax.load(std::memory_order_relaxed);
		while (copySubmitUs > prevMax && !m_statCopySubmitUsMax.compare_exchange_weak(prevMax, copySubmitUs, std::memory_order_relaxed))
		{
		}

		uint64_t lastLogUs = m_lastTelemetryLogUs.load(std::memory_order_relaxed);
		if (lastLogUs == 0)
		{
			m_lastTelemetryLogUs.store(copyEndUs, std::memory_order_relaxed);
		}
		else if (copyEndUs - lastLogUs >= 2000000ULL && m_lastTelemetryLogUs.compare_exchange_weak(lastLogUs, copyEndUs, std::memory_order_relaxed))
		{
			const uint64_t windowUs = (copyEndUs > lastLogUs) ? (copyEndUs - lastLogUs) : 1ULL;
			const uint64_t produced = m_statProducedFrames.exchange(0, std::memory_order_relaxed);
			const uint64_t forced = m_statForcedFullFrames.exchange(0, std::memory_order_relaxed);
			const uint64_t forcedManual = m_statForcedFullManual.exchange(0, std::memory_order_relaxed);
			const uint64_t forcedRecreate = m_statForcedFullRecreate.exchange(0, std::memory_order_relaxed);
			const uint64_t forcedOverflow = m_statForcedFullOverflow.exchange(0, std::memory_order_relaxed);
			const uint64_t dirtyCount = m_statDirtyRectCountSum.exchange(0, std::memory_order_relaxed);
			const uint64_t dirtyArea = m_statDirtyRectAreaSum.exchange(0, std::memory_order_relaxed);
			const uint64_t copyUsSum = m_statCopySubmitUsSum.exchange(0, std::memory_order_relaxed);
			const uint64_t copyUsMax = m_statCopySubmitUsMax.exchange(0, std::memory_order_relaxed);
			const uint64_t beginSent = m_statBeginFramesSentWindow.exchange(0, std::memory_order_relaxed);
			const uint64_t beginToPaintUsSum = m_statBeginToPaintUsSum.exchange(0, std::memory_order_relaxed);
			const uint64_t beginToPaintUsMax = m_statBeginToPaintUsMax.exchange(0, std::memory_order_relaxed);
			const uint64_t schedMissCount = m_statSchedMissCount.exchange(0, std::memory_order_relaxed);
			const uint64_t schedLateUsSum = m_statSchedLateUsSum.exchange(0, std::memory_order_relaxed);
			const uint64_t schedLateUsMax = m_statSchedLateUsMax.exchange(0, std::memory_order_relaxed);
			const uint64_t sentFps = (beginSent * 1000000ULL) / windowUs;
			const uint64_t paintFps = (produced * 1000000ULL) / windowUs;
			const uint64_t windowMs = windowUs / 1000ULL;
			const uint64_t avgCopyUs = (produced > 0) ? (copyUsSum / produced) : 0;
			const uint64_t avgBeginToPaintUs = (produced > 0) ? (beginToPaintUsSum / produced) : 0;
			const uint64_t avgSchedLateUs = (schedMissCount > 0) ? (schedLateUsSum / schedMissCount) : 0;
			const uint64_t avgDirtyRects = (produced > 0) ? (dirtyCount / produced) : 0;
			const uint64_t avgDirtyArea = (produced > 0) ? (dirtyArea / produced) : 0;
			const uint64_t sentNow = m_beginFramesSent.load(std::memory_order_relaxed);
			const uint64_t doneNow = m_paintsCompleted.load(std::memory_order_relaxed);
			const uint64_t inFlightNow = (sentNow > doneNow) ? (sentNow - doneNow) : 0;
			const uint64_t intervalNsNow = m_beginFrameIntervalNs.load(std::memory_order_relaxed);
			const uint64_t intervalUsNow = (intervalNsNow > 0ULL) ? (intervalNsNow / 1000ULL) : 0ULL;
			const uint64_t producerFpsNow = (intervalNsNow > 0ULL) ? (1000000000ULL / intervalNsNow) : 0ULL;
			fprintf(stdout,
				"[OsrTelemetry] window_ms=%llu sent_fps=%llu paint_fps=%llu frames=%llu begin_sent=%llu in_flight=%llu forced_full=%llu forced_manual=%llu forced_recreate=%llu forced_overflow=%llu interval_us=%llu producer_fps=%llu sched_miss=%llu sched_late_us_avg=%llu sched_late_us_max=%llu dirty_rects_avg=%llu dirty_area_avg=%llu copy_us_avg=%llu copy_us_max=%llu begin_to_paint_us_avg=%llu begin_to_paint_us_max=%llu\n",
				static_cast<unsigned long long>(windowMs),
				static_cast<unsigned long long>(sentFps),
				static_cast<unsigned long long>(paintFps),
				static_cast<unsigned long long>(produced),
				static_cast<unsigned long long>(beginSent),
				static_cast<unsigned long long>(inFlightNow),
				static_cast<unsigned long long>(forced),
				static_cast<unsigned long long>(forcedManual),
				static_cast<unsigned long long>(forcedRecreate),
				static_cast<unsigned long long>(forcedOverflow),
				static_cast<unsigned long long>(intervalUsNow),
				static_cast<unsigned long long>(producerFpsNow),
				static_cast<unsigned long long>(schedMissCount),
				static_cast<unsigned long long>(avgSchedLateUs),
				static_cast<unsigned long long>(schedLateUsMax),
				static_cast<unsigned long long>(avgDirtyRects),
				static_cast<unsigned long long>(avgDirtyArea),
				static_cast<unsigned long long>(avgCopyUs),
				static_cast<unsigned long long>(copyUsMax),
				static_cast<unsigned long long>(avgBeginToPaintUs),
				static_cast<unsigned long long>(beginToPaintUsMax));
		}
	}
}

void OsrHandler::TrySendBeginFrame()
{
	using namespace std::chrono;
	uint64_t now = duration_cast<microseconds>(
		steady_clock::now().time_since_epoch())
					   .count();

	// Backpressure: optional cap for in-flight begin-frames.
	const uint64_t sent = m_beginFramesSent.load(std::memory_order_relaxed);
	const uint64_t done = m_paintsCompleted.load(std::memory_order_relaxed);
	const uint64_t inFlight = (sent > done) ? (sent - done) : 0;
	const uint32_t maxInFlight = m_maxInFlightBeginFrames.load(std::memory_order_relaxed);
	if (maxInFlight > 0 && inFlight >= static_cast<uint64_t>(maxInFlight))
		return;

	CefRefPtr<CefBrowser> b = m_browser;
	if (b)
	{
		m_lastBeginFrameUs.store(now, std::memory_order_relaxed);
		b->GetHost()->SendExternalBeginFrame();
		m_beginFramesSent.fetch_add(1, std::memory_order_relaxed);
		m_statBeginFramesSentWindow.fetch_add(1, std::memory_order_relaxed);
	}
}

void OsrHandler::TryIdleRepairInvalidate()
{
	// Disabled: idle repair invalidation can inject cadence disturbances during animation.
	return;
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
	if (cadenceUs == 0)
		return;

	const uint32_t minUs = 1000000u / 240u;
	const uint32_t maxUs = 1000000u / 15u;
	uint32_t clamped = cadenceUs;
	if (clamped < minUs)
		clamped = minUs;
	if (clamped > maxUs)
		clamped = maxUs;

	const uint32_t prev = m_smoothedConsumerCadenceUs.load(std::memory_order_relaxed);
	const uint32_t smooth = (prev == 0) ? clamped : ((prev * 7u + clamped * 3u) / 10u);
	m_smoothedConsumerCadenceUs.store(smooth, std::memory_order_relaxed);

	// Keep producer near consumer cadence; tiny slowdown keeps stability without adding visible lag.
	const uint64_t targetUs = static_cast<uint64_t>(smooth) + static_cast<uint64_t>(smooth) / 100ULL;
	m_beginFrameIntervalNs.store(targetUs * 1000ULL, std::memory_order_relaxed);
}

void OsrHandler::StartRenderLoop()
{
	if (!m_timerPeriodRaised && timeBeginPeriod(1) == TIMERR_NOERROR)
		m_timerPeriodRaised = true;

	m_running = true;

	m_renderThread = std::thread([this]() {
		if (m_enableThreadTuning)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		}

		LARGE_INTEGER qpf{};
		QueryPerformanceFrequency(&qpf);
		const LONGLONG freq = (qpf.QuadPart > 0) ? qpf.QuadPart : 1LL;
		const LONGLONG spinTicks = (freq / 2000LL > 0) ? (freq / 2000LL) : 1LL; // ~0.5ms

		auto nowTicks = []() -> LONGLONG {
			LARGE_INTEGER t{};
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		};

		HANDLE hiResTimer = CreateWaitableTimerExW(
			nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!hiResTimer)
		{
			hiResTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		}

		auto waitUntil = [&](LONGLONG targetTick) {
			while (m_running)
			{
				const LONGLONG now = nowTicks();
				LONGLONG remaining = targetTick - now;
				if (remaining <= 0)
					break;

				// Coarse sleep first, then short spin to reduce wake jitter.
				if (remaining > spinTicks)
				{
					const LONGLONG sleepTicks = remaining - spinTicks;
					if (hiResTimer)
					{
						LONGLONG due100ns = -((sleepTicks * 10000000LL) / freq);
						if (due100ns == 0)
							due100ns = -1;
						LARGE_INTEGER due{};
						due.QuadPart = due100ns;
						if (SetWaitableTimerEx(hiResTimer, &due, 0, nullptr, nullptr, nullptr, 0))
						{
							WaitForSingleObject(hiResTimer, INFINITE);
						}
						else
						{
							DWORD sleepMs = static_cast<DWORD>((sleepTicks * 1000LL) / freq);
							if (sleepMs == 0)
								sleepMs = 1;
							Sleep(sleepMs);
						}
					}
					else
					{
						DWORD sleepMs = static_cast<DWORD>((sleepTicks * 1000LL) / freq);
						if (sleepMs == 0)
							sleepMs = 1;
						Sleep(sleepMs);
					}
				}
				else
				{
					YieldProcessor();
				}
			}
		};

		LONGLONG nextTick = nowTicks();

		while (m_running)
		{
			const uint64_t intervalNs = m_beginFrameIntervalNs.load(std::memory_order_relaxed);
			const uint64_t ns = (intervalNs > 0ULL) ? intervalNs : 16666666ULL;
			LONGLONG frameTicks = static_cast<LONGLONG>((ns * static_cast<uint64_t>(freq) + 999999999ULL) / 1000000000ULL);
			if (frameTicks <= 0)
				frameTicks = 1;

			nextTick += frameTicks;
			waitUntil(nextTick);
			if (!m_running)
				break;

			const LONGLONG wakeTick = nowTicks();
			if (wakeTick > nextTick)
			{
				const uint64_t lateUs = static_cast<uint64_t>(((wakeTick - nextTick) * 1000000LL) / freq);
				if (lateUs > 500ULL)
				{
					m_statSchedMissCount.fetch_add(1, std::memory_order_relaxed);
					m_statSchedLateUsSum.fetch_add(lateUs, std::memory_order_relaxed);
					uint64_t prevMax = m_statSchedLateUsMax.load(std::memory_order_relaxed);
					while (lateUs > prevMax && !m_statSchedLateUsMax.compare_exchange_weak(prevMax, lateUs, std::memory_order_relaxed))
					{
					}
				}
			}

			if (!m_paused)
			{
				TrySendBeginFrame();
				TryIdleRepairInvalidate();
			}

			// Resync only on major stalls to avoid aggressive skip/drop behavior.
			const LONGLONG afterTick = nowTicks();
			if (afterTick > nextTick + frameTicks * 4)
			{
				nextTick = afterTick;
			}
		}

		if (hiResTimer)
			CloseHandle(hiResTimer);
	});

	m_inputThread = std::thread([this]() {
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
				if (m_inputBuffer.HasPendingEvents())
				{
					found = true;
					break;
				}
				QueryPerformanceCounter(&now);
				if (now.QuadPart - start.QuadPart >= spinTicks)
					break;
				YieldProcessor();
			}

			if (found)
				PumpInput();
			else
				WaitForSingleObject(m_inputBuffer.GetEvent(), 100);
		}
	});

	m_controlThread = std::thread([this]() {
		if (m_enableThreadTuning)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
			TryPinCurrentThread(2);
		}
		while (m_running)
		{
			WaitForSingleObject(m_controlBuffer.GetEvent(), 100);
			if (m_running)
				PumpControl();
		}
	});
}

void OsrHandler::StopRenderLoop()
{
	m_running = false;

	SetEvent(m_inputBuffer.GetEvent());
	SetEvent(m_controlBuffer.GetEvent());

	if (m_renderThread.joinable())
		m_renderThread.join();
	if (m_inputThread.joinable())
		m_inputThread.join();
	if (m_controlThread.joinable())
		m_controlThread.join();

	if (m_timerPeriodRaised)
	{
		timeEndPeriod(1);
		m_timerPeriodRaised = false;
	}
}

void OsrHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type)
{
	if (!frame->IsMain())
		return;
	FrameHeader* header = m_frameBuffer.GetHeader();
	if (!header)
		return;
	header->load_state = CefLoadState::Loading;
	std::atomic_thread_fence(std::memory_order_release);
	InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header->sequence));
	SetEvent(m_frameBuffer.GetEvent());
}

void OsrHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code)
{
	if (!frame->IsMain())
		return;
	FrameHeader* header = m_frameBuffer.GetHeader();
	if (!header)
		return;
	header->load_state = (http_status_code >= 400) ? CefLoadState::Error : CefLoadState::Ready;
	std::atomic_thread_fence(std::memory_order_release);
	InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header->sequence));
	SetEvent(m_frameBuffer.GetEvent());
}

void OsrHandler::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
	ErrorCode error_code, const CefString& error_text, const CefString& failed_url)
{
	if (!frame->IsMain())
		return;
	FrameHeader* header = m_frameBuffer.GetHeader();
	if (!header)
		return;
	header->load_state = CefLoadState::Error;
	std::atomic_thread_fence(std::memory_order_release);
	InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header->sequence));
	SetEvent(m_frameBuffer.GetEvent());
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
	if (!show)
	{
		m_popupRect = {};
	}
}

void OsrHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect)
{
	m_popupRect = rect;
}

bool OsrHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
	const CefString& message, const CefString& source, int line)
{
	ConsoleLogEvent evt{};
	if (level >= LOGSEVERITY_ERROR)
		evt.level = ConsoleLogLevel::Error;
	else if (level == LOGSEVERITY_WARNING)
		evt.level = ConsoleLogLevel::Warning;
	else
		evt.level = ConsoleLogLevel::Log;

	evt.line = line;
	CopyCefStringToUtf16Array(source, evt.source);
	CopyCefStringToUtf16Array(message, evt.message);
	m_consoleBuffer.WriteEvent(evt);
	return false;
}

void OsrHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
	rect = CefRect(0, 0, static_cast<int>(m_width), static_cast<int>(m_height));
}

void OsrHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	m_browser = browser;
	CefRefPtr<CefBrowserHost> host = browser->GetHost();
	host->SetFocus(true);
	// Align CEF internal pacing with producer pacing to avoid drift at startup.
	UpdateBeginFrameIntervalFromFps(60u);
	host->SetWindowlessFrameRate(60);
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

void OsrHandler::TryInputNudgeFrame()
{
	if (m_paused.load(std::memory_order_relaxed))
		return;

	using namespace std::chrono;
	const uint64_t now = duration_cast<microseconds>(
		steady_clock::now().time_since_epoch())
							 .count();
	const uint64_t intervalNs = m_beginFrameIntervalNs.load(std::memory_order_relaxed);
	const uint64_t cadenceUs = (intervalNs / 1000ULL > 0ULL) ? (intervalNs / 1000ULL) : 1ULL;
	const uint64_t nudgeUs = (cadenceUs < 2000ULL) ? cadenceUs : 2000ULL;

	uint64_t prev = m_lastBeginFrameUs.load(std::memory_order_relaxed);
	while (true)
	{
		if (now - prev < nudgeUs)
			break;
		if (m_lastBeginFrameUs.compare_exchange_weak(prev, now, std::memory_order_relaxed))
		{
			CefRefPtr<CefBrowser> b = m_browser;
			if (b)
				b->GetHost()->SendExternalBeginFrame();
			break;
		}
	}
}

bool OsrHandler::DispatchInputEvent(CefRefPtr<CefBrowserHost> host, const InputEvent& evt)
{
	switch (evt.type)
	{
		case InputEventType::MouseMove:
		{
			CefMouseEvent e;
			e.x = evt.mouse.x;
			e.y = evt.mouse.y;
			e.modifiers = m_mouseModifiers;
			host->SendMouseMoveEvent(e, false);
			return true;
		}
		case InputEventType::MouseDown:
		case InputEventType::MouseUp:
		{
			auto btn = static_cast<CefBrowserHost::MouseButtonType>(evt.mouse.button);
			bool isUp = evt.type == InputEventType::MouseUp;
			uint32_t flag = (btn == MBT_LEFT) ? EVENTFLAG_LEFT_MOUSE_BUTTON
				: (btn == MBT_MIDDLE)		  ? EVENTFLAG_MIDDLE_MOUSE_BUTTON
											  : EVENTFLAG_RIGHT_MOUSE_BUTTON;
			if (isUp)
				m_mouseModifiers &= ~flag;
			else
				m_mouseModifiers |= flag;
			CefMouseEvent e;
			e.x = evt.mouse.x;
			e.y = evt.mouse.y;
			e.modifiers = m_mouseModifiers;
			host->SendMouseClickEvent(e, btn, isUp, 1);
			return true;
		}
		case InputEventType::MouseScroll:
		{
			CefMouseEvent e;
			e.x = evt.scroll.x;
			e.y = evt.scroll.y;
			host->SendMouseWheelEvent(e,
				static_cast<int>(evt.scroll.delta_x),
				static_cast<int>(evt.scroll.delta_y));
			return true;
		}
		case InputEventType::KeyDown:
		case InputEventType::KeyUp:
		{
			CefKeyEvent e;
			e.type = (evt.type == InputEventType::KeyDown) ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
			e.windows_key_code = static_cast<int>(evt.key.windows_key_code);
			e.modifiers = evt.key.modifiers;
			host->SendKeyEvent(e);
			return true;
		}
		case InputEventType::KeyChar:
		{
			CefKeyEvent e;
			e.type = KEYEVENT_CHAR;
			e.windows_key_code = evt.char_event.character;
			host->SendKeyEvent(e);
			return true;
		}
		default:
			return false;
	}
}

void OsrHandler::PumpInput()
{
	CefRefPtr<CefBrowser> browser = m_browser;
	if (!browser)
		return;
	CefRefPtr<CefBrowserHost> host = browser->GetHost();
	InputEvent evt;
	bool shouldNudgeFrame = false;
	uint32_t processedEvents = 0;
	constexpr uint32_t kMaxEventsPerPump = 64;
	constexpr uint32_t kNudgeEveryEvents = 8;
	while (processedEvents < kMaxEventsPerPump && m_inputBuffer.ReadEvent(evt))
	{
		++processedEvents;
		if (!m_inputEnabled)
			continue;
		shouldNudgeFrame = DispatchInputEvent(host, evt) || shouldNudgeFrame;

		if (shouldNudgeFrame && (processedEvents % kNudgeEveryEvents) == 0)
		{
			TryInputNudgeFrame();
			shouldNudgeFrame = false;
		}
	}

	if (shouldNudgeFrame)
	{
		TryInputNudgeFrame();
	}
}

void OsrHandler::HandleControlSetFrameRate(CefRefPtr<CefBrowserHost> host, uint32_t requestedFps)
{
	const uint32_t applied = (requestedFps == 0) ? 60u : (requestedFps > 240u ? 240u : requestedFps);
	host->SetWindowlessFrameRate(static_cast<int>(applied));
	UpdateBeginFrameIntervalFromFps(applied);
}

void OsrHandler::HandleControlSetMaxInFlightBeginFrames(uint32_t requestedMaxInFlight)
{
	const uint32_t applied = (requestedMaxInFlight == 0u) ? 2u : requestedMaxInFlight;
	m_maxInFlightBeginFrames.store(applied, std::memory_order_relaxed);
	// Drop historical debt from previous unlimited mode so cap can stabilize immediately.
	const uint64_t sentNow = m_beginFramesSent.load(std::memory_order_relaxed);
	m_paintsCompleted.store(sentNow, std::memory_order_relaxed);
}

void OsrHandler::HandleControlLoadHtmlString(CefRefPtr<CefBrowser> browser, const char16_t* html)
{
	const CefString encoded = CefURIEncode(CefString(html), false);
	const std::u16string dataUrl = std::u16string(u"data:text/html;charset=utf-8,") + encoded.ToString16();
	browser->GetMainFrame()->LoadURL(CefString(dataUrl));
}

void OsrHandler::HandleControlScrollTo(CefRefPtr<CefBrowser> browser, int32_t x, int32_t y)
{
	browser->GetMainFrame()->ExecuteJavaScript(
		CefString("window.scrollTo(" + std::to_string(x) + "," + std::to_string(y) + ")"), CefString(), 0);
}

void OsrHandler::HandleControlOpenDevTools(CefRefPtr<CefBrowserHost> host)
{
	CefWindowInfo wi;
	wi.SetAsPopup(nullptr, "DevTools");
	host->ShowDevTools(wi, nullptr, CefBrowserSettings(), CefPoint());
}

void OsrHandler::HandleControlEvent(CefRefPtr<CefBrowser> browser, CefRefPtr<CefBrowserHost> host, const ControlEvent& evt)
{
	switch (evt.type)
	{
		case ControlEventType::GoBack:
			browser->GoBack();
			break;
		case ControlEventType::GoForward:
			browser->GoForward();
			break;
		case ControlEventType::StopLoad:
			browser->StopLoad();
			break;
		case ControlEventType::Reload:
			browser->Reload();
			break;
		case ControlEventType::SetURL:
			browser->GetMainFrame()->LoadURL(CefString(evt.string.text));
			break;
		case ControlEventType::SetPaused:
			m_paused = evt.flag.value;
			break;
		case ControlEventType::SetHidden:
			host->WasHidden(evt.flag.value);
			break;
		case ControlEventType::SetFocus:
			host->SetFocus(evt.flag.value);
			break;
		case ControlEventType::SetZoomLevel:
			host->SetZoomLevel(static_cast<double>(evt.zoom.value));
			break;
		case ControlEventType::SetFrameRate:
			HandleControlSetFrameRate(host, evt.frame_rate.value);
			break;
		case ControlEventType::SetConsumerCadenceUs:
			// Intentionally ignored for now: cadence feedback is gated while tuning is experimental.
			(void)evt;
			break;
		case ControlEventType::SetMaxInFlightBeginFrames:
			HandleControlSetMaxInFlightBeginFrames(evt.frame_rate.value);
			break;
		case ControlEventType::SetFlushIntervalFrames:
			m_flushIntervalFrames.store(evt.frame_rate.value, std::memory_order_relaxed);
			break;
		case ControlEventType::SetKeyframeIntervalUs:
			m_keyframeIntervalUs.store(evt.cadence_us.value, std::memory_order_relaxed);
			break;
		case ControlEventType::OpenLocalFile:
			browser->GetMainFrame()->LoadURL(MakeFileUrlFromPath(CefString(evt.string.text)));
			break;
		case ControlEventType::LoadHtmlString:
			HandleControlLoadHtmlString(browser, evt.string.text);
			break;
		case ControlEventType::ScrollTo:
			HandleControlScrollTo(browser, evt.scroll.x, evt.scroll.y);
			break;
		case ControlEventType::Resize:
			Resize(evt.resize.width, evt.resize.height);
			break;
		case ControlEventType::SetMuted:
			host->SetAudioMuted(evt.flag.value);
			break;
		case ControlEventType::OpenDevTools:
			HandleControlOpenDevTools(host);
			break;
		case ControlEventType::CloseDevTools:
			host->CloseDevTools();
			break;
		case ControlEventType::SetInputEnabled:
			m_inputEnabled = evt.flag.value;
			break;
		case ControlEventType::ExecuteJS:
			browser->GetMainFrame()->ExecuteJavaScript(
				CefString(evt.string.text), CefString(), 0);
			break;
		case ControlEventType::ClearCookies:
			CefCookieManager::GetGlobalManager(nullptr)->DeleteCookies(
				CefString(), CefString(), nullptr);
			break;
	}
}

void OsrHandler::PumpControl()
{
	CefRefPtr<CefBrowser> browser = m_browser;
	if (!browser)
		return;
	CefRefPtr<CefBrowserHost> host = browser->GetHost();

	ControlEvent evt;
	while (m_controlBuffer.ReadEvent(evt))
	{
		HandleControlEvent(browser, host, evt);
	}
}
