#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include "d3dx12.h"
#include "FrustumCuller.h"
#include "Octree.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ----------------------------------------------------------------
// Описание одного инстанса (CPU-сторона)
// ----------------------------------------------------------------
struct InstanceRecord
{
    XMFLOAT3 Position;
    float    Scale;
    XMFLOAT3 Color;
    float    Pad;
    AABB     BoundingBox;
};

// ----------------------------------------------------------------
// Данные, передаваемые на GPU (per-instance vertex stream)
// Всего 80 байт на инстанс.
// ----------------------------------------------------------------
struct alignas(16) InstanceGPUData
{
    XMFLOAT4 World0, World1, World2, World3;  // строки мировой матрицы (64 B)
    XMFLOAT3 Color;                           // (12 B)
    float    Pad;                             // (4 B)
};

// ----------------------------------------------------------------
// Constant buffer для шейдера инстансов
// ----------------------------------------------------------------
struct alignas(256) InstanceFrameCB
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT3   EyePos;     float Pad0;
    XMFLOAT3   LightDir;   float Pad1;
    XMFLOAT3   LightColor; float Pad2;
    XMFLOAT3   Ambient;    float Pad3;
};

// ================================================================
// InstanceSystem
// Инициализация: вызвать Init() ПОСЛЕ RenderingSystem::Init().
// Рендеринг:     вызвать Draw() между DrawScene() и EndFrame().
// ================================================================
class InstanceSystem
{
public:
    static constexpr int  INSTANCE_COUNT = 2000;
    static constexpr int  FRAME_COUNT = 2;
    static constexpr int  MAX_INSTANCES = INSTANCE_COUNT; // буфер

    // Создаёт объекты, PSO, буферы.
    // device        — ID3D12Device из RenderingSystem::GetDevice()
    // backBufFormat — формат swap chain (обычно DXGI_FORMAT_R8G8B8A8_UNORM)
    // depthFormat   — формат GBuffer depth (DXGI_FORMAT_D32_FLOAT)
    bool Init(ID3D12Device* device,
        DXGI_FORMAT backBufFormat = DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT);

    // Рисует видимые инстансы поверх уже отрендеренной сцены.
    // Вызывать при открытом cmdList, пока back buffer — в RT-состоянии.
    void Draw(
        ID3D12GraphicsCommandList* cmdList,
        UINT                       frameIndex,
        XMMATRIX                   view,
        XMMATRIX                   proj,
        XMFLOAT3                   eye,
        XMFLOAT3                   lightDir,   // нормализованный вектор К источнику
        XMFLOAT3                   lightColor,
        XMFLOAT3                   ambient,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
        D3D12_CPU_DESCRIPTOR_HANDLE gbufferDSV,
        UINT width, UINT height,
        bool wireframe);

    // Переключатели отсечения
    void ToggleFrustumCulling() { m_useFrustum = !m_useFrustum; }
    void ToggleOctree() { m_useOctree = !m_useOctree; }

    bool IsFrustumCullingOn() const { return m_useFrustum; }
    bool IsOctreeOn()         const { return m_useOctree; }
    int  GetVisibleCount()    const { return m_visibleCount; }

private:
    void GenerateInstances();
    bool CreatePipeline(ID3D12Device* device, DXGI_FORMAT rtFmt, DXGI_FORMAT dsFmt);
    bool CreateGeometry(ID3D12Device* device);
    bool CreateDynamicBuffers(ID3D12Device* device);

    // Culling
    std::vector<InstanceRecord> m_instances;
    std::vector<AABB>           m_aabbs;
    Octree                      m_octree;
    bool m_useFrustum = true;
    bool m_useOctree = true;
    int  m_visibleCount = 0;

    // Pipeline
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12PipelineState> m_psowire;
    ComPtr<ID3DBlob>            m_vsBlob, m_psBlob;

    // Куб — статическая геометрия, upload heap (маленький меш)
    ComPtr<ID3D12Resource>   m_cubeVB;
    ComPtr<ID3D12Resource>   m_cubeIB;
    D3D12_VERTEX_BUFFER_VIEW m_cubeVBView{};
    D3D12_INDEX_BUFFER_VIEW  m_cubeIBView{};
    UINT                     m_cubeIndexCount = 0;

    // Инстанс-буфер (один на кадр, upload heap, динамический)
    ComPtr<ID3D12Resource>  m_instBuf[FRAME_COUNT];
    InstanceGPUData* m_instMapped[FRAME_COUNT] = {};

    // Константный буфер (один на кадр)
    ComPtr<ID3D12Resource>  m_cb[FRAME_COUNT];
    InstanceFrameCB* m_cbMapped[FRAME_COUNT] = {};
};