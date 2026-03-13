#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

class GBuffer
{
public:
    static constexpr UINT RT_COUNT = 3;

    static constexpr DXGI_FORMAT FORMATS[RT_COUNT] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,        
        DXGI_FORMAT_R16G16B16A16_FLOAT,    
        DXGI_FORMAT_R32G32B32A32_FLOAT,    
    };

    bool Create(
        ID3D12Device* device,
        UINT width, UINT height,
        ID3D12DescriptorHeap* rtvHeap, UINT rtvOffset, UINT rtvDescSize,
        ID3D12DescriptorHeap* dsvHeap, UINT dsvOffset, UINT dsvDescSize,
        ID3D12DescriptorHeap* srvHeap, UINT srvOffset, UINT srvDescSize);

    void Resize(
        ID3D12Device* device,
        UINT width, UINT height,
        ID3D12DescriptorHeap* rtvHeap, UINT rtvOffset, UINT rtvDescSize,
        ID3D12DescriptorHeap* dsvHeap, UINT dsvOffset, UINT dsvDescSize,
        ID3D12DescriptorHeap* srvHeap, UINT srvOffset, UINT srvDescSize);

    void Release();

    void TransitionToRenderTarget(ID3D12GraphicsCommandList* cmd);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd);
    void ClearAndSetRenderTargets(ID3D12GraphicsCommandList* cmd, UINT width, UINT height);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(UINT i)   const { return m_rtvHandles[i]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV()          const { return m_dsvHandle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVTableGPU()  const { return m_srvGpuStart; }

private:
    ComPtr<ID3D12Resource> m_rts[RT_COUNT];
    ComPtr<ID3D12Resource> m_depth;

    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandles[RT_COUNT] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpuStart = {};

    D3D12_RESOURCE_STATES m_states[RT_COUNT] = {};
};