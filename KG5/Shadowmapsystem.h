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

// ============================================================
// Cascaded Shadow Maps system.
//   - 3 cascades, 1024x1024 each, Texture2DArray<R32_TYPELESS>
//   - Practical split scheme (log + uniform mix), lambda=0.5
//   - PCF 3x3 with HW comparison (in lighting shader)
//   - Owns DSVs/CB/PSO; SRV is created in the renderer's heap.
// ============================================================

constexpr UINT CSM_CASCADE_COUNT = 3;
constexpr UINT CSM_RESOLUTION = 1024;

struct alignas(256) ShadowVsCB
{
    XMFLOAT4X4 WorldViewProj;
};

class ShadowMapSystem
{
public:
    static constexpr UINT FRAME_COUNT = 2;

    bool Init(ID3D12Device* device);

    // Считает split distances + light view-proj для каждого каскада.
    // Вызывается раз в кадр перед shadow pass.
    void ComputeCascades(
        XMFLOAT3 eye, XMFLOAT3 target, XMFLOAT3 up,
        XMFLOAT3 lightDir,
        float fovY, float aspect, float nearZ, float farZ);

    // Шадоу-пасс: TransitionToDepthWrite -> для каждого каскада
    // BeginCascade + SetCascadeCB + draw -> TransitionToShaderResource.
    void TransitionToDepthWrite(ID3D12GraphicsCommandList* cmdList);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList);

    // Установить DSV slice + viewport для каскада, очистить.
    void BeginCascade(ID3D12GraphicsCommandList* cmdList, UINT cascadeIdx);

    // Записать CB (worldMtx * lightViewProj[cascade]) и привязать.
    void SetCascadeCB(ID3D12GraphicsCommandList* cmdList,
        UINT cascadeIdx, XMMATRIX worldMtx, UINT frameIndex);

    ID3D12RootSignature* GetShadowRootSignature() const { return m_rs.Get(); }
    ID3D12PipelineState* GetShadowPSO()           const { return m_pso.Get(); }

    // Для передачи в lighting CB:
    XMMATRIX GetCsmView()                   const { return m_cascadeViewMtx; }
    XMMATRIX GetLightViewProj(UINT idx)     const { return m_lightViewProj[idx]; }
    XMFLOAT4 GetCascadeFarSplits()          const { return m_cascadeFar; }

    // Создать SRV (Texture2DArray) в чужом shader-visible heap.
    void CreateSrvInExternalHeap(D3D12_CPU_DESCRIPTOR_HANDLE dst);

    bool IsInitialized() const { return m_initialized; }

private:
    bool CreateResource(ID3D12Device* device);
    bool CreateDsvHeap(ID3D12Device* device);
    bool CreateRootSignature(ID3D12Device* device);
    bool CompileShader();
    bool CreatePSO(ID3D12Device* device);
    bool CreateCB(ID3D12Device* device);

    ID3D12Device* m_device = nullptr;

    ComPtr<ID3D12Resource>       m_shadowArray;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT                         m_dsvDescSize = 0;

    ComPtr<ID3D12RootSignature> m_rs;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3DBlob>            m_vs;

    // CB pool: CSM_CASCADE_COUNT * FRAME_COUNT slots
    ComPtr<ID3D12Resource> m_cb;
    UINT8* m_cbMapped = nullptr;
    UINT                   m_cbSlotSize = 0;

    XMMATRIX m_lightViewProj[CSM_CASCADE_COUNT];
    XMMATRIX m_cascadeViewMtx;     // view матрица камеры (для cascade selection)
    XMFLOAT4 m_cascadeFar = { 0, 0, 0, 0 };  // .xyz = far view-Z каждого каскада

    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bool m_initialized = false;
};