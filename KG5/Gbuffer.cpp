#include "GBuffer.h"
#include <stdexcept>

static void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("GBuffer DX12 call failed");
}

bool GBuffer::Create(
    ID3D12Device* device,
    UINT width, UINT height,
    ID3D12DescriptorHeap* rtvHeap, UINT rtvOffset, UINT rtvDescSize,
    ID3D12DescriptorHeap* dsvHeap, UINT dsvOffset, UINT dsvDescSize,
    ID3D12DescriptorHeap* srvHeap, UINT srvOffset, UINT srvDescSize)
{
    for (UINT i = 0; i < RT_COUNT; ++i)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = FORMATS[i];
        desc.SampleDesc = { 1, 0 };
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = FORMATS[i];

        CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            IID_PPV_ARGS(&m_rts[i])));

        m_states[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvH(
            rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            rtvOffset + i, rtvDescSize);
        device->CreateRenderTargetView(m_rts[i].Get(), nullptr, rtvH);
        m_rtvHandles[i] = rtvH;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = FORMATS[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvH(
            srvHeap->GetCPUDescriptorHandleForHeapStart(),
            srvOffset + i, srvDescSize);
        device->CreateShaderResourceView(m_rts[i].Get(), &srvDesc, srvH);

        if (i == 0)
        {
            m_srvGpuStart = CD3DX12_GPU_DESCRIPTOR_HANDLE(
                srvHeap->GetGPUDescriptorHandleForHeapStart(),
                srvOffset, srvDescSize);
        }
    }

    {
        D3D12_RESOURCE_DESC dd{};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = width;
        dd.Height = height;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc = { 1, 0 };
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE dcv{};
        dcv.Format = DXGI_FORMAT_D32_FLOAT;
        dcv.DepthStencil.Depth = 1.0f;

        CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv,
            IID_PPV_ARGS(&m_depth)));

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvH(
            dsvHeap->GetCPUDescriptorHandleForHeapStart(),
            dsvOffset, dsvDescSize);
        device->CreateDepthStencilView(m_depth.Get(), nullptr, dsvH);
        m_dsvHandle = dsvH;
    }

    return true;
}

void GBuffer::Resize(
    ID3D12Device* device,
    UINT width, UINT height,
    ID3D12DescriptorHeap* rtvHeap, UINT rtvOffset, UINT rtvDescSize,
    ID3D12DescriptorHeap* dsvHeap, UINT dsvOffset, UINT dsvDescSize,
    ID3D12DescriptorHeap* srvHeap, UINT srvOffset, UINT srvDescSize)
{
    Release();
    Create(device, width, height,
        rtvHeap, rtvOffset, rtvDescSize,
        dsvHeap, dsvOffset, dsvDescSize,
        srvHeap, srvOffset, srvDescSize);
}

void GBuffer::Release()
{
    for (auto& rt : m_rts) rt.Reset();
    m_depth.Reset();
}

void GBuffer::TransitionToRenderTarget(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER barriers[RT_COUNT];
    UINT count = 0;
    for (UINT i = 0; i < RT_COUNT; ++i)
    {
        if (m_states[i] != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            barriers[count++] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_rts[i].Get(),
                m_states[i],
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_states[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
    }
    if (count > 0)
        cmd->ResourceBarrier(count, barriers);
}

void GBuffer::TransitionToShaderResource(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER barriers[RT_COUNT];
    UINT count = 0;
    for (UINT i = 0; i < RT_COUNT; ++i)
    {
        if (m_states[i] != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        {
            barriers[count++] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_rts[i].Get(),
                m_states[i],
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_states[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }
    if (count > 0)
        cmd->ResourceBarrier(count, barriers);
}

void GBuffer::ClearAndSetRenderTargets(ID3D12GraphicsCommandList* cmd,
    UINT width, UINT height)
{
    const float black[] = { 0.f, 0.f, 0.f, 0.f };
    for (UINT i = 0; i < RT_COUNT; ++i)
        cmd->ClearRenderTargetView(m_rtvHandles[i], black, 0, nullptr);
    cmd->ClearDepthStencilView(m_dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cmd->OMSetRenderTargets(RT_COUNT, m_rtvHandles, FALSE, &m_dsvHandle);

    D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
    D3D12_RECT     sc{ 0, 0, (LONG)width,  (LONG)height };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
}