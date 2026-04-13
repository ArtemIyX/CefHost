#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <stdio.h>

using Microsoft::WRL::ComPtr;

class D3D11Device
{
public:
	bool Init()
	{
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
		flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		D3D_FEATURE_LEVEL featureLevel;
		HRESULT hr = D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			flags,
			nullptr, 0,
			D3D11_SDK_VERSION,
			&m_device,
			&featureLevel,
			&m_context
		);

		if (FAILED(hr))
		{
			fprintf(stderr, "[D3D11Device] D3D11CreateDevice failed: 0x%08X\n", hr);
			return false;
		}

		hr = m_device.As(&m_dxgiDevice);
		if (FAILED(hr))
		{
			fprintf(stderr, "[D3D11Device] Failed to get IDXGIDevice: 0x%08X\n", hr);
			return false;
		}

		ComPtr<IDXGIAdapter> adapter;
		hr = m_dxgiDevice->GetAdapter(&adapter);
		if (FAILED(hr))
		{
			fprintf(stderr, "[D3D11Device] GetAdapter failed: 0x%08X\n", hr);
			return false;
		}

		hr = adapter->GetParent(IID_PPV_ARGS(&m_dxgiFactory));
		if (FAILED(hr))
		{
			fprintf(stderr, "[D3D11Device] GetParent(IDXGIFactory2) failed: 0x%08X\n", hr);
			return false;
		}

		fprintf(stdout, "[D3D11Device] Initialized.\n");
		return true;
	}

	void Shutdown()
	{
		m_context.Reset();
		m_dxgiDevice.Reset();
		m_dxgiFactory.Reset();
		m_device.Reset();
	}

	ID3D11Device* GetDevice()      const { return m_device.Get(); }
	ID3D11DeviceContext* GetContext()     const { return m_context.Get(); }
	IDXGIFactory2* GetDXGIFactory() const { return m_dxgiFactory.Get(); }

private:
	ComPtr<ID3D11Device>        m_device;
	ComPtr<ID3D11DeviceContext> m_context;
	ComPtr<IDXGIDevice>         m_dxgiDevice;
	ComPtr<IDXGIFactory2>       m_dxgiFactory;
};