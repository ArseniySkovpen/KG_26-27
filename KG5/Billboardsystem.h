#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include "d3dx12.h"
#include "FrustumCuller.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct TreeRecord
{
    XMFLOAT3 Position;
    float    Height;
    float    Width;
    XMFLOAT3 Color;
    AABB     Box;
};

struct TreeGPUData
{
    XMFLOAT3 WorldPos;
    float    Height;
    float    Width;
    XMFLOAT3 Color;
};

struct alignas(256) BillboardCB
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT3   EyePos;   float Pad0;
    XMFLOAT3   Ambient;  float Pad1;
    XMFLOAT3   SunDir;   float Pad2;
    XMFLOAT3   SunColor; float Pad3;
};

class BillboardSystem
{
public:
    static constexpr int   TREE_COUNT = 300;
    static constexpr int   FRAME_COUNT = 2;
    static constexpr float LOD_DISTANCE = 800.f;

    bool Init(ID3D12Device* device,
        DXGI_FORMAT   backBufFormat = DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT   depthFormat = DXGI_FORMAT_D32_FLOAT);

    void Draw(ID3D12GraphicsCommandList* cmdList,
        UINT                        frameIndex,
        XMMATRIX                    view,
        XMMATRIX                    proj,
        XMFLOAT3                    eye,
        XMFLOAT3                    sunDir,
        XMFLOAT3                    sunColor,
        XMFLOAT3                    ambient,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
        D3D12_CPU_DESCRIPTOR_HANDLE gbufferDSV,
        UINT width, UINT height);

    int GetVisibleCount() const { return m_visibleCount; }

private:
    void GenerateTrees();
    bool CreateQuadGeometry(ID3D12Device* device);
    bool CreateTreeGeometry(ID3D12Device* device);
    bool CreateBillboardPipeline(ID3D12Device* device, DXGI_FORMAT rtFmt, DXGI_FORMAT dsFmt);
    bool CreateTreePipeline(ID3D12Device* device, DXGI_FORMAT rtFmt, DXGI_FORMAT dsFmt);
    bool CreateDynamicBuffers(ID3D12Device* device);

    std::vector<TreeRecord> m_trees;
    std::vector<AABB>       m_aabbs;
    int                     m_visibleCount = 0;

    // Billboard LOD1
    ComPtr<ID3D12RootSignature> m_billRootSig;
    ComPtr<ID3D12PipelineState> m_billPSO;
    ComPtr<ID3D12Resource>      m_quadVB, m_quadIB;
    D3D12_VERTEX_BUFFER_VIEW    m_quadVBView{};
    D3D12_INDEX_BUFFER_VIEW     m_quadIBView{};

    // 3D tree LOD0
    ComPtr<ID3D12RootSignature> m_treeRootSig;
    ComPtr<ID3D12PipelineState> m_treePSO;
    ComPtr<ID3D12Resource>      m_treeVB, m_treeIB;
    D3D12_VERTEX_BUFFER_VIEW    m_treeVBView{};
    D3D12_INDEX_BUFFER_VIEW     m_treeIBView{};
    UINT                        m_treeIndexCount = 0;

    // Per-frame instance buffers (separate for each LOD)
    ComPtr<ID3D12Resource> m_instBufLOD0[FRAME_COUNT];
    ComPtr<ID3D12Resource> m_instBufLOD1[FRAME_COUNT];
    TreeGPUData* m_instLOD0[FRAME_COUNT] = {};
    TreeGPUData* m_instLOD1[FRAME_COUNT] = {};

    ComPtr<ID3D12Resource> m_cb[FRAME_COUNT];
    BillboardCB* m_cbMapped[FRAME_COUNT] = {};
};