#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include <string>

#include "d3dx12.h"
#include "ObjLoader.h"
#include "TextureLoader.h"
#include "GBuffer.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct Vertex { XMFLOAT3 Position; XMFLOAT3 Normal; XMFLOAT2 TexCoord; };

struct GBufferCBData
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4X4 WorldInvTranspose;
    XMFLOAT4   MaterialDiffuse;
    XMFLOAT4   MaterialSpecular; 
    int        HasTexture;
    float      TexTilingX;
    float      TexTilingY;
    float      TotalTime;
    float      TexScrollX;
    float      TexScrollY;
    float      Pad[2];           
};

static constexpr int MAX_POINT_LIGHTS = 16;
static constexpr int MAX_SPOT_LIGHTS = 4;

struct PointLightData { XMFLOAT4 Position; XMFLOAT4 Color; }; 
struct SpotLightData { XMFLOAT4 Position; XMFLOAT4 Direction; XMFLOAT4 Color; }; 


struct LightingCBData
{
    XMFLOAT4 EyePos;
    XMFLOAT4 AmbientColor;
    XMFLOAT4 DirLightDir;   
    XMFLOAT4 DirLightColor; 
    PointLightData PointLights[MAX_POINT_LIGHTS];
    SpotLightData  SpotLights[MAX_SPOT_LIGHTS];
    int NumPointLights, NumSpotLights, Pad0, Pad1;
};

struct GpuMaterial
{
    ComPtr<ID3D12Resource> texture, textureUpload;
    int      srvHeapIndex = -1;
    XMFLOAT4 diffuse = { 0.8f,0.8f,0.8f,1.f };
    XMFLOAT4 specular = { 0.5f,0.5f,0.5f,32.f }; 
    bool     hasTexture = false;
};

class RenderingSystem
{
public:
    static constexpr UINT FRAME_COUNT = 2;
    static constexpr UINT MAX_TEXTURES = 512;
    static constexpr UINT MAX_SUBSETS = 512;

    RenderingSystem() = default;
    ~RenderingSystem();

    bool Init(HWND hwnd, int width, int height);
    void OnResize(int width, int height);
    bool LoadObj(const std::string& path);

    void BeginFrame(const float clearColor[4]);
    void DrawScene(float totalTime, float dt);
    void EndFrame();

    // Освещение
    void SetAmbient(XMFLOAT3 color);
    void SetDirectionalLight(XMFLOAT3 dir, XMFLOAT3 color, float intensity = 1.f);
    void AddPointLight(XMFLOAT3 pos, XMFLOAT3 color, float radius, float intensity = 1.f);
    void AddSpotLight(XMFLOAT3 pos, XMFLOAT3 dir, XMFLOAT3 color,
        float innerDeg, float outerDeg, float intensity = 1.f);
    void ClearLights();

    void SetTexTiling(float x, float y) { m_texTiling = { x, y }; }
    void SetTexScroll(float x, float y) { m_texScroll = { x, y }; }
    void SetCamera(XMFLOAT3 eye, XMFLOAT3 target, XMFLOAT3 up = { 0,1,0 })
    {
        m_eye = eye; m_target = target; m_up = up;
    }

private:
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, int w, int h);
    void CreateDescriptorHeaps();
    void CreateRenderTargetViews();
    void CreateFence();
    void CompileShaders();
    void CreateGeometryRootSignature();
    void CreateLightingRootSignature();
    void CreateGeometryPSO();
    void CreateLightingPSO();
    void CreateDefaultGeometry();
    void CreateGeometryCB();
    void CreateLightingCB();
    void UploadMeshToGpu(const std::vector<Vertex>&, const std::vector<UINT>&);
    void LoadMaterials(const ObjMesh& mesh, const std::string& dir);
    void WaitForGPU();
    void MoveToNextFrame();
    void FlushCommandQueue();
    void DoGeometryPass(float totalTime);
    void DoLightingPass();

    ComPtr<IDXGIFactory6>             m_factory;
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    ComPtr<ID3D12CommandAllocator>    m_cmdAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<IDXGISwapChain3>           m_swapChain;
    ComPtr<ID3D12Resource>            m_renderTargets[FRAME_COUNT];
    UINT                              m_frameIndex = 0;

    
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_rtvDescSize = 0, m_dsvDescSize = 0, m_srvDescSize = 0;

    GBuffer m_gbuffer;

    ComPtr<ID3D12Fence> m_fence;
    UINT64              m_fenceValues[FRAME_COUNT]{};
    HANDLE              m_fenceEvent = nullptr;

    ComPtr<ID3D12RootSignature> m_geoRS, m_lightRS;
    ComPtr<ID3D12PipelineState> m_geoPSO, m_lightPSO;
    ComPtr<ID3DBlob>            m_geoVS, m_geoPS;
    ComPtr<ID3DBlob>            m_lightVS, m_lightPS;

    ComPtr<ID3D12Resource>   m_vertexBuffer, m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
    D3D12_INDEX_BUFFER_VIEW  m_ibView{};
    std::vector<MeshSubset>  m_subsets;
    std::vector<GpuMaterial> m_gpuMaterials;

    ComPtr<ID3D12Resource> m_geoCB;
    UINT8* m_geoCBMapped = nullptr;
    UINT                   m_geoCBSlotSize = 0;

    ComPtr<ID3D12Resource> m_lightCB;
    UINT8* m_lightCBMapped = nullptr;
    UINT                   m_lightCBSlotSize = 0;

    LightingCBData m_lightData{};

    XMFLOAT2 m_texTiling = { 1.f, 1.f };
    XMFLOAT2 m_texScroll = { 0.f, 0.f };
    XMFLOAT3 m_eye = { 0.f, 150.f, -500.f };
    XMFLOAT3 m_target = { 0.f,  80.f,    0.f };
    XMFLOAT3 m_up = { 0.f,   1.f,    0.f };

    int  m_width = 0, m_height = 0;
    bool m_initialized = false;
};