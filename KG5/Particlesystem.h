#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include "d3dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct Particle
{
    XMFLOAT3 Position; float Life;
    XMFLOAT3 Velocity; float Size;
    XMFLOAT4 Color;
};

struct alignas(256) ParticleUpdateCB
{
    float    DeltaTime;
    float    TotalTime;
    UINT     EmitCount;
    UINT     SrcCount;
    XMFLOAT4 EmitterPos;
    XMFLOAT3 Gravity; float Pad0;
};

struct alignas(256) ParticleRenderCB
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT3   EyePos;  float Pad0;
};

class ParticleSystem
{
public:
    static constexpr UINT MAX_PARTICLES = 65536;
    static constexpr UINT FRAME_COUNT = 2;

    bool Init(ID3D12Device* device,
        DXGI_FORMAT   rtFmt = DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT   dsFmt = DXGI_FORMAT_D32_FLOAT);

    void UpdateAndDraw(
        ID3D12GraphicsCommandList* cmdList,
        UINT                       frameIndex,
        float dt, float totalTime,
        XMMATRIX view, XMMATRIX proj, XMFLOAT3 eye,
        XMFLOAT3 emitterPos, UINT emitCount,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        UINT width, UINT height);

    UINT GetLastAliveCount() const { return m_aliveTracked; }
    bool IsInitialized()     const { return m_initialized; }

private:
    bool CompileShaders();
    bool CreateRootSignatures(ID3D12Device* device);
    bool CreatePipelines(ID3D12Device* device, DXGI_FORMAT rtFmt, DXGI_FORMAT dsFmt);
    bool CreateResources(ID3D12Device* device);
    bool CreateDescriptorHeapAndViews(ID3D12Device* device);

    void InitGpuState(ID3D12GraphicsCommandList* cmdList);

    D3D12_GPU_DESCRIPTOR_HANDLE GpuH(UINT slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuH(UINT slot) const;

    ID3D12Device* m_device = nullptr;

    ComPtr<ID3D12Resource> m_particleBuf[2];
    ComPtr<ID3D12Resource> m_counterBuf[2];

    ComPtr<ID3D12Resource> m_counterZeroUpload;

    ComPtr<ID3D12Resource> m_updateCB[FRAME_COUNT];
    UINT8* m_updateCBMapped[FRAME_COUNT] = {};
    ComPtr<ID3D12Resource> m_renderCB[FRAME_COUNT];
    UINT8* m_renderCBMapped[FRAME_COUNT] = {};

    // Heap layout - pre-created UAVs, NEVER recreated:
    //   Tables for compute (alternating between two configurations):
    //     Config A (when m_srcIdx=0): slot 0 = UAV(buf0/cnt0), slot 1 = UAV(buf1/cnt1)
    //     Config B (when m_srcIdx=1): slot 2 = UAV(buf1/cnt1), slot 3 = UAV(buf0/cnt0)
    //   Slot 4: SRV(buf0)
    //   Slot 5: SRV(buf1)
    ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
    UINT m_descSize = 0;

    ComPtr<ID3DBlob> m_csEmit, m_csUpdate;
    ComPtr<ID3DBlob> m_vsRender, m_gsRender, m_psRender;

    ComPtr<ID3D12RootSignature> m_computeRS;
    ComPtr<ID3D12RootSignature> m_renderRS;

    ComPtr<ID3D12PipelineState> m_psoEmit;
    ComPtr<ID3D12PipelineState> m_psoUpdate;
    ComPtr<ID3D12PipelineState> m_psoRender;

    UINT m_srcIdx = 0;
    UINT m_dstIdx = 1;
    bool m_needsGpuInit = true;
    bool m_initialized = false;

    UINT m_aliveTracked = 0;
};