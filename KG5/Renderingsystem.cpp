#include "RenderingSystem.h"
#include <stdexcept>
#include <cmath>
#include <array>

static void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("DirectX call failed");
}

bool RenderingSystem::Init(HWND hwnd, int width, int height)
{
    m_width = width; m_height = height;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try
    {
        CreateDevice();
        CreateCommandObjects();
        CreateSwapChain(hwnd, width, height);
        CreateDescriptorHeaps();
        CreateRenderTargetViews();

        m_gbuffer.Create(
            m_device.Get(), (UINT)width, (UINT)height,
            m_rtvHeap.Get(), FRAME_COUNT, m_rtvDescSize,
            m_dsvHeap.Get(), 0, m_dsvDescSize,
            m_srvHeap.Get(), MAX_TEXTURES + 1, m_srvDescSize);

        CreateFence();
        CompileShaders();
        CreateGeometryRootSignature();
        CreateLightingRootSignature();
        CreateGeometryPSO();
        CreateLightingPSO();
        CreateTessRootSignature();
        CreateTessPSO();
        CreateDefaultGeometry();
        CreateGeometryCB();
        CreateLightingCB();
        CreateTessCB();

        // ---- CSM init ----
        if (!m_shadowSys.Init(m_device.Get()))
            OutputDebugStringA("ShadowMapSystem init FAILED\n");

        // SRV for shadow map array goes AFTER tess slots in srvHeap:
        //   0           : null
        //   1..512      : textures
        //   513..515    : GBuffer SRVs
        //   516..518    : tess (disp, norm, color)
        //   519         : CSM array
        m_csmSrvSlot = 1 + MAX_TEXTURES + GBuffer::RT_COUNT + 3;
        CD3DX12_CPU_DESCRIPTOR_HANDLE csmCpu(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
            m_csmSrvSlot, m_srvDescSize);
        m_shadowSys.CreateSrvInExternalHeap(csmCpu);

        m_lightData.AmbientColor = { 0.08f, 0.08f, 0.12f, 1.f };
        SetDirectionalLight({ 0.3f, -1.f, 0.5f }, { 1.f, 0.92f, 0.75f }, 1.f);

        ThrowIfFailed(m_cmdList->Close());
        ID3D12CommandList* cmds[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, cmds);
        WaitForGPU();
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA(e.what());
        return false;
    }
    m_initialized = true;
    return true;
}

void RenderingSystem::CreateDevice()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
#endif
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
        m_factory->EnumAdapterByGpuPreference(i,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
        ++i)
    {
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) break;
    }
    if (!m_device)
        ThrowIfFailed(D3D12CreateDevice(nullptr,
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
}

void RenderingSystem::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_cmdQueue)));
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocators[i])));
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_cmdList)));
}

void RenderingSystem::CreateSwapChain(HWND hwnd, int w, int h)
{
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = w; sc.Height = h;
    sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc = { 1, 0 };
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = FRAME_COUNT;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
        m_cmdQueue.Get(), hwnd, &sc, nullptr, nullptr, &sc1));
    ThrowIfFailed(sc1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void RenderingSystem::CreateDescriptorHeaps()
{
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = FRAME_COUNT + GBuffer::RT_COUNT;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        d.NumDescriptors = 1;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_dsvHeap)));
        m_dsvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        // 1 null + MAX_TEXTURES + GBuffer + 3 tess + 1 CSM
        d.NumDescriptors = 1 + MAX_TEXTURES + GBuffer::RT_COUNT + 3 + 1;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_srvHeap)));
        m_srvDescSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(nullptr, &nullDesc,
            m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    }
}

void RenderingSystem::CreateRenderTargetViews()
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE h(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, h);
        h.Offset(1, m_rtvDescSize);
    }
}

void RenderingSystem::CreateFence()
{
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValues[m_frameIndex] = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
}

void RenderingSystem::CompileShaders()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(L"GBufferShader.hlsl",
        nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &m_geoVS, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA((char*)errors->GetBufferPointer()); ThrowIfFailed(hr); }

    hr = D3DCompileFromFile(L"GBufferShader.hlsl",
        nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &m_geoPS, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA((char*)errors->GetBufferPointer()); ThrowIfFailed(hr); }

    hr = D3DCompileFromFile(L"LightingShader.hlsl",
        nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &m_lightVS, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA((char*)errors->GetBufferPointer()); ThrowIfFailed(hr); }

    hr = D3DCompileFromFile(L"LightingShader.hlsl",
        nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &m_lightPS, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA((char*)errors->GetBufferPointer()); ThrowIfFailed(hr); }

    ComPtr<ID3DBlob> tessErrors;
    D3DCompileFromFile(L"TessShader.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &m_tessVS, &tessErrors);
    D3DCompileFromFile(L"TessShader.hlsl", nullptr, nullptr, "HSMain", "hs_5_0", flags, 0, &m_tessHS, &tessErrors);
    D3DCompileFromFile(L"TessShader.hlsl", nullptr, nullptr, "DSMain", "ds_5_0", flags, 0, &m_tessDS, &tessErrors);
    D3DCompileFromFile(L"TessShader.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &m_tessPS, &tessErrors);
    if (tessErrors) OutputDebugStringA((char*)tessErrors->GetBufferPointer());
}

void RenderingSystem::CreateGeometryRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA((char*)errors->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(m_device->CreateRootSignature(0,
        serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_geoRS)));
}

void RenderingSystem::CreateLightingRootSignature()
{
    // Slot 0: CB b0 (LightingCBData с CSM полями)
    // Slot 1: SRV table t0..t2 (GBuffer)
    // Slot 2: SRV table t3 (CSM array)
    // Static samplers: s0 (point), s1 (comparison for shadow PCF)

    CD3DX12_DESCRIPTOR_RANGE gbufRange;
    gbufRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::RT_COUNT, 0);  // t0..t2

    CD3DX12_DESCRIPTOR_RANGE csmRange;
    csmRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);                   // t3

    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsDescriptorTable(1, &gbufRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[2].InitAsDescriptorTable(1, &csmRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    samplers[0].Init(0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].Init(1,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 2, samplers,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA((char*)errors->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(m_device->CreateRootSignature(0,
        serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_lightRS)));
}

void RenderingSystem::CreateGeometryPSO()
{
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { layout, _countof(layout) };
    pso.pRootSignature = m_geoRS.Get();
    pso.VS = { m_geoVS->GetBufferPointer(), m_geoVS->GetBufferSize() };
    pso.PS = { m_geoPS->GetBufferPointer(), m_geoPS->GetBufferSize() };
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = GBuffer::RT_COUNT;
    for (UINT i = 0; i < GBuffer::RT_COUNT; ++i)
        pso.RTVFormats[i] = GBuffer::FORMATS[i];
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = { 1, 0 };
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_geoPSO)));
}

void RenderingSystem::CreateLightingPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { nullptr, 0 };
    pso.pRootSignature = m_lightRS.Get();
    pso.VS = { m_lightVS->GetBufferPointer(), m_lightVS->GetBufferSize() };
    pso.PS = { m_lightPS->GetBufferPointer(), m_lightPS->GetBufferSize() };
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    D3D12_DEPTH_STENCIL_DESC dsDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState = dsDesc;

    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc = { 1, 0 };
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_lightPSO)));
}

void RenderingSystem::CreateDefaultGeometry()
{
    std::array<Vertex, 24> verts = { {
        {{ -1,-1, 1 }, { 0, 0, 1 }, { 0,1 }}, {{  1,-1, 1 }, { 0, 0, 1 }, { 1,1 }},
        {{  1, 1, 1 }, { 0, 0, 1 }, { 1,0 }}, {{ -1, 1, 1 }, { 0, 0, 1 }, { 0,0 }},
        {{  1,-1,-1 }, { 0, 0,-1 }, { 0,1 }}, {{ -1,-1,-1 }, { 0, 0,-1 }, { 1,1 }},
        {{ -1, 1,-1 }, { 0, 0,-1 }, { 1,0 }}, {{  1, 1,-1 }, { 0, 0,-1 }, { 0,0 }},
        {{ -1,-1,-1 }, {-1, 0, 0 }, { 0,1 }}, {{ -1,-1, 1 }, {-1, 0, 0 }, { 1,1 }},
        {{ -1, 1, 1 }, {-1, 0, 0 }, { 1,0 }}, {{ -1, 1,-1 }, {-1, 0, 0 }, { 0,0 }},
        {{  1,-1, 1 }, { 1, 0, 0 }, { 0,1 }}, {{  1,-1,-1 }, { 1, 0, 0 }, { 1,1 }},
        {{  1, 1,-1 }, { 1, 0, 0 }, { 1,0 }}, {{  1, 1, 1 }, { 1, 0, 0 }, { 0,0 }},
        {{ -1, 1, 1 }, { 0, 1, 0 }, { 0,1 }}, {{  1, 1, 1 }, { 0, 1, 0 }, { 1,1 }},
        {{  1, 1,-1 }, { 0, 1, 0 }, { 1,0 }}, {{ -1, 1,-1 }, { 0, 1, 0 }, { 0,0 }},
        {{ -1,-1,-1 }, { 0,-1, 0 }, { 0,1 }}, {{  1,-1,-1 }, { 0,-1, 0 }, { 1,1 }},
        {{  1,-1, 1 }, { 0,-1, 0 }, { 1,0 }}, {{ -1,-1, 1 }, { 0,-1, 0 }, { 0,0 }},
    } };
    std::array<UINT, 36> idx;
    for (int f = 0; f < 6; ++f) {
        UINT b = f * 4;
        idx[f * 6 + 0] = b + 0; idx[f * 6 + 1] = b + 1; idx[f * 6 + 2] = b + 2;
        idx[f * 6 + 3] = b + 0; idx[f * 6 + 4] = b + 2; idx[f * 6 + 5] = b + 3;
    }
    MeshSubset sub{ 0, 36, 0 };
    m_subsets = { sub };
    m_gpuMaterials.clear();
    m_gpuMaterials.resize(1);
    m_gpuMaterials[0].diffuse = { 0.2f, 0.5f, 0.9f, 1.f };
    m_gpuMaterials[0].specular = { 0.8f, 0.8f, 0.8f, 32.f };
    std::vector<Vertex> v(verts.begin(), verts.end());
    std::vector<UINT>   i(idx.begin(), idx.end());
    UploadMeshToGpu(v, i);
}

void RenderingSystem::UploadMeshToGpu(const std::vector<Vertex>& verts,
    const std::vector<UINT>& indices)
{
    m_vertexBuffer.Reset(); m_indexBuffer.Reset();
    auto upload = [&](const void* data, UINT sz, ComPtr<ID3D12Resource>& buf) {
        CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)));
        void* p = nullptr; buf->Map(0, nullptr, &p); memcpy(p, data, sz); buf->Unmap(0, nullptr);
        };
    UINT vbSz = (UINT)(verts.size() * sizeof(Vertex));
    UINT ibSz = (UINT)(indices.size() * sizeof(UINT));
    upload(verts.data(), vbSz, m_vertexBuffer);
    upload(indices.data(), ibSz, m_indexBuffer);
    m_vbView = { m_vertexBuffer->GetGPUVirtualAddress(), vbSz, sizeof(Vertex) };
    m_ibView = { m_indexBuffer->GetGPUVirtualAddress(),  ibSz, DXGI_FORMAT_R32_UINT };
}

void RenderingSystem::CreateGeometryCB()
{
    m_geoCBSlotSize = (sizeof(GBufferCBData) + 255) & ~255;
    UINT total = m_geoCBSlotSize * MAX_SUBSETS * FRAME_COUNT;
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(total);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_geoCB)));
    m_geoCB->Map(0, nullptr, reinterpret_cast<void**>(&m_geoCBMapped));
}

void RenderingSystem::CreateLightingCB()
{
    m_lightCBSlotSize = (sizeof(LightingCBData) + 255) & ~255;
    UINT total = m_lightCBSlotSize * FRAME_COUNT;
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(total);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightCB)));
    m_lightCB->Map(0, nullptr, reinterpret_cast<void**>(&m_lightCBMapped));
}

bool RenderingSystem::LoadObj(const std::string& path)
{
    if (m_initialized) FlushCommandQueue();

    ObjMesh mesh;
    if (!ObjLoader::Load(path, mesh)) return false;

    std::vector<Vertex> verts(mesh.vertices.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        verts[i].Position = mesh.vertices[i].Position;
        verts[i].Normal = mesh.vertices[i].Normal;
        verts[i].TexCoord = mesh.vertices[i].TexCoord;
    }
    m_subsets = mesh.subsets;

    std::string dir;
    size_t p = path.find_last_of("/\\");
    if (p != std::string::npos) dir = path.substr(0, p + 1);

    ThrowIfFailed(m_cmdAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr));

    LoadMaterials(mesh, dir);
    UploadMeshToGpu(verts, mesh.indices);

    ThrowIfFailed(m_cmdList->Close());
    ID3D12CommandList* cmds[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmds);
    WaitForGPU();

    for (auto& mat : m_gpuMaterials) mat.textureUpload.Reset();
    return true;
}

void RenderingSystem::LoadMaterials(const ObjMesh& mesh, const std::string& baseDir)
{
    m_gpuMaterials.clear();
    if (mesh.materials.empty()) {
        m_gpuMaterials.resize(1);
        return;
    }
    m_gpuMaterials.reserve(mesh.materials.size());
    m_gpuMaterials.resize(mesh.materials.size());
    int srvSlot = 0;

    for (size_t i = 0; i < mesh.materials.size(); ++i)
    {
        const Material& src = mesh.materials[i];
        GpuMaterial& dst = m_gpuMaterials[i];
        dst.diffuse = src.diffuse;
        dst.specular = { src.specular.x, src.specular.y, src.specular.z, src.shininess };

        if (!src.diffuseTexture.empty())
        {
            std::wstring wpath(baseDir.begin(), baseDir.end());
            std::wstring wtex(src.diffuseTexture.begin(), src.diffuseTexture.end());
            wpath += wtex;

            std::wstring msg = L"Loading: " + wpath + L"\n";
            OutputDebugStringW(msg.c_str());

            TextureLoader::TextureData td;
            if (TextureLoader::LoadFromFile(wpath, td) &&
                TextureLoader::CreateTexture(m_device.Get(), m_cmdList.Get(),
                    td, dst.texture, dst.textureUpload))
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Format = td.format;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;

                CD3DX12_CPU_DESCRIPTOR_HANDLE srvH(
                    m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                    1 + srvSlot, m_srvDescSize);
                m_device->CreateShaderResourceView(dst.texture.Get(), &srvDesc, srvH);

                dst.srvHeapIndex = 1 + srvSlot;
                dst.hasTexture = true;
                ++srvSlot;
            }
        }
    }
}

void RenderingSystem::SetAmbient(XMFLOAT3 c)
{
    m_lightData.AmbientColor = { c.x, c.y, c.z, 1.f };
}

void RenderingSystem::SetDirectionalLight(XMFLOAT3 dir, XMFLOAT3 color, float intensity)
{
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dir));
    XMStoreFloat4(&m_lightData.DirLightDir, XMVectorSetW(d, 0.f));
    m_lightData.DirLightColor = { color.x, color.y, color.z, intensity };
}

void RenderingSystem::AddPointLight(XMFLOAT3 pos, XMFLOAT3 color, float radius, float intensity)
{
    if (m_lightData.NumPointLights >= MAX_POINT_LIGHTS) return;
    PointLightData& l = m_lightData.PointLights[m_lightData.NumPointLights++];
    l.Position = { pos.x, pos.y, pos.z, radius };
    l.Color = { color.x, color.y, color.z, intensity };
}

void RenderingSystem::AddSpotLight(XMFLOAT3 pos, XMFLOAT3 dir,
    XMFLOAT3 color, float innerDeg, float outerDeg,
    float intensity)
{
    if (m_lightData.NumSpotLights >= MAX_SPOT_LIGHTS) return;
    float innerCos = cosf(XMConvertToRadians(innerDeg));
    float outerCos = cosf(XMConvertToRadians(outerDeg));
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dir));
    XMFLOAT3 nd; XMStoreFloat3(&nd, d);
    SpotLightData& l = m_lightData.SpotLights[m_lightData.NumSpotLights++];
    l.Position = { pos.x, pos.y, pos.z, innerCos };
    l.Direction = { nd.x,  nd.y,  nd.z,  outerCos };
    l.Color = { color.x, color.y, color.z, intensity };
}

void RenderingSystem::ClearLights()
{
    m_lightData.NumPointLights = 0;
    m_lightData.NumSpotLights = 0;
    m_lightData.DirLightColor.w = 0.f;
}

void RenderingSystem::BeginFrame(const float /*clearColor*/[4])
{
    ThrowIfFailed(m_cmdAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr));
}

void RenderingSystem::DrawScene(float totalTime, float /*dt*/)
{
    if (!m_geoPSO || m_subsets.empty()) return;

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    // 1. Shadow pass - рендерим Sponza в каскады
    DoShadowPass();

    // 2. GBuffer + tess
    DoGeometryPass(totalTime);
    DoTessPass(totalTime);

    // 3. Lighting (читает CSM)
    DoLightingPass();
}

// ============================================================
// Shadow pass
// ============================================================
void RenderingSystem::DoShadowPass()
{
    if (!m_shadowSys.IsInitialized()) return;

    // 1. Compute cascades для текущей камеры и dir light direction
    float aspect = (float)m_width / (float)m_height;
    XMFLOAT3 lightDir = {
        m_lightData.DirLightDir.x,
        m_lightData.DirLightDir.y,
        m_lightData.DirLightDir.z
    };
    // Shadow far меньше камеры (10000) - детали на близком, грубо вдалеке
    m_shadowSys.ComputeCascades(m_eye, m_target, m_up, lightDir,
        XMConvertToRadians(60.f), aspect, 1.f, 2500.f);

    // 2. Transition + render каждый каскад
    m_shadowSys.TransitionToDepthWrite(m_cmdList.Get());

    m_cmdList->SetGraphicsRootSignature(m_shadowSys.GetShadowRootSignature());
    m_cmdList->SetPipelineState(m_shadowSys.GetShadowPSO());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbView);
    m_cmdList->IASetIndexBuffer(&m_ibView);

    XMMATRIX worldI = XMMatrixIdentity();
    for (UINT cascade = 0; cascade < CSM_CASCADE_COUNT; ++cascade)
    {
        m_shadowSys.BeginCascade(m_cmdList.Get(), cascade);
        m_shadowSys.SetCascadeCB(m_cmdList.Get(), cascade, worldI, m_frameIndex);

        for (UINT subIdx = 0; subIdx < (UINT)m_subsets.size(); ++subIdx)
        {
            const MeshSubset& sub = m_subsets[subIdx];
            if (sub.indexCount == 0) continue;
            m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
        }
    }

    // 3. Transition shadow map в SRV для lighting pass
    m_shadowSys.TransitionToShaderResource(m_cmdList.Get());
}

void RenderingSystem::DoGeometryPass(float totalTime)
{
    m_gbuffer.TransitionToRenderTarget(m_cmdList.Get());
    m_gbuffer.ClearAndSetRenderTargets(m_cmdList.Get(), (UINT)m_width, (UINT)m_height);

    m_cmdList->SetPipelineState(m_geoPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_geoRS.Get());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbView);
    m_cmdList->IASetIndexBuffer(&m_ibView);

    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view = XMMatrixLookAtLH(
        XMLoadFloat3(&m_eye), XMLoadFloat3(&m_target), XMLoadFloat3(&m_up));
    float aspect = (float)m_width / (float)m_height;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.f), aspect, 1.f, 10000.f);
    XMMATRIX wit = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

    UINT numSubsets = (UINT)m_subsets.size();
    for (UINT subIdx = 0; subIdx < numSubsets; ++subIdx)
    {
        const MeshSubset& sub = m_subsets[subIdx];
        if (sub.indexCount == 0) continue;

        int matIdx = (sub.materialIdx >= 0 && sub.materialIdx < (int)m_gpuMaterials.size())
            ? sub.materialIdx : 0;
        const GpuMaterial& mat = m_gpuMaterials.empty() ? GpuMaterial{} : m_gpuMaterials[matIdx];

        UINT slotIdx = m_frameIndex * RenderingSystem::MAX_SUBSETS + (subIdx % RenderingSystem::MAX_SUBSETS);
        UINT8* slotPtr = m_geoCBMapped + slotIdx * m_geoCBSlotSize;
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
            m_geoCB->GetGPUVirtualAddress() + slotIdx * m_geoCBSlotSize;

        GBufferCBData cb{};
        XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
        XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
        XMStoreFloat4x4(&cb.WorldInvTranspose, XMMatrixTranspose(wit));
        cb.MaterialDiffuse = mat.diffuse;
        cb.MaterialSpecular = mat.specular;
        cb.HasTexture = mat.hasTexture ? 1 : 0;
        cb.TotalTime = totalTime;
        cb.TexTilingX = m_texTiling.x;
        cb.TexTilingY = m_texTiling.y;
        cb.TexScrollX = m_texScroll.x;
        cb.TexScrollY = m_texScroll.y;
        memcpy(slotPtr, &cb, sizeof(cb));

        m_cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

        int srvIdx = (mat.hasTexture && mat.srvHeapIndex >= 0) ? mat.srvHeapIndex : 0;
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvH(
            m_srvHeap->GetGPUDescriptorHandleForHeapStart(), srvIdx, m_srvDescSize);
        m_cmdList->SetGraphicsRootDescriptorTable(1, srvH);

        m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
    }
}

void RenderingSystem::DoLightingPass()
{
    m_gbuffer.TransitionToShaderResource(m_cmdList.Get());

    CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_cmdList->ResourceBarrier(1, &b);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_frameIndex, m_rtvDescSize);
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float black[] = { 0.f, 0.f, 0.f, 1.f };
    m_cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);

    D3D12_VIEWPORT vp{ 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT     sc{ 0, 0, m_width, m_height };
    m_cmdList->RSSetViewports(1, &vp);
    m_cmdList->RSSetScissorRects(1, &sc);

    m_cmdList->SetPipelineState(m_lightPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_lightRS.Get());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_lightData.EyePos = { m_eye.x, m_eye.y, m_eye.z, 1.f };

    // ---- CSM data ----
    XMStoreFloat4x4(&m_lightData.CsmView,
        XMMatrixTranspose(m_shadowSys.GetCsmView()));
    for (UINT i = 0; i < CSM_CASCADE_COUNT; ++i)
        XMStoreFloat4x4(&m_lightData.CsmLightViewProj[i],
            XMMatrixTranspose(m_shadowSys.GetLightViewProj(i)));
    m_lightData.CsmCascadeFar = m_shadowSys.GetCascadeFarSplits();

    UINT8* lightSlot = m_lightCBMapped + m_frameIndex * m_lightCBSlotSize;
    memcpy(lightSlot, &m_lightData, sizeof(m_lightData));
    D3D12_GPU_VIRTUAL_ADDRESS lightAddr =
        m_lightCB->GetGPUVirtualAddress() + m_frameIndex * m_lightCBSlotSize;

    m_cmdList->SetGraphicsRootConstantBufferView(0, lightAddr);
    m_cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetSRVTableGPU());

    // ---- CSM SRV table ----
    CD3DX12_GPU_DESCRIPTOR_HANDLE csmGpu(
        m_srvHeap->GetGPUDescriptorHandleForHeapStart(),
        m_csmSrvSlot, m_srvDescSize);
    m_cmdList->SetGraphicsRootDescriptorTable(2, csmGpu);

    m_cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::EndFrame()
{
    CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_cmdList->ResourceBarrier(1, &b);
    ThrowIfFailed(m_cmdList->Close());
    ID3D12CommandList* cmds[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmds);
    ThrowIfFailed(m_swapChain->Present(1, 0));
    MoveToNextFrame();
}

void RenderingSystem::WaitForGPU()
{
    const UINT64 val = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), val));
    m_fenceValues[m_frameIndex]++;
    if (m_fence->GetCompletedValue() < val) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(val, m_fenceEvent));
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }
}

void RenderingSystem::MoveToNextFrame()
{
    const UINT64 cur = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), cur));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }
    m_fenceValues[m_frameIndex] = cur + 1;
}

void RenderingSystem::FlushCommandQueue() { WaitForGPU(); }

void RenderingSystem::OnResize(int width, int height)
{
    if (!m_initialized || (m_width == width && m_height == height)) return;
    m_width = width; m_height = height;
    FlushCommandQueue();

    for (auto& rt : m_renderTargets) rt.Reset();
    ThrowIfFailed(m_swapChain->ResizeBuffers(
        FRAME_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargetViews();

    m_gbuffer.Resize(
        m_device.Get(), (UINT)width, (UINT)height,
        m_rtvHeap.Get(), FRAME_COUNT, m_rtvDescSize,
        m_dsvHeap.Get(), 0, m_dsvDescSize,
        m_srvHeap.Get(), MAX_TEXTURES + 1, m_srvDescSize);
}

RenderingSystem::~RenderingSystem()
{
    if (m_initialized) FlushCommandQueue();
    if (m_geoCB && m_geoCBMapped)   m_geoCB->Unmap(0, nullptr);
    if (m_lightCB && m_lightCBMapped) m_lightCB->Unmap(0, nullptr);
    if (m_tessCB && m_tessCBMapped)  m_tessCB->Unmap(0, nullptr);
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
    CoUninitialize();
}

void RenderingSystem::CreateTessRootSignature()
{
    if (!m_tessVS) return;

    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsDescriptorTable(1, &srvRange,
        D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA((char*)errors->GetBufferPointer()); return; }
    m_device->CreateRootSignature(0,
        serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_tessRS));
}


void RenderingSystem::CreateTessPSO()
{
    if (!m_tessVS || !m_tessHS || !m_tessDS || !m_tessPS || !m_tessRS) return;

    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { layout, _countof(layout) };
    pso.pRootSignature = m_tessRS.Get();
    pso.VS = { m_tessVS->GetBufferPointer(), m_tessVS->GetBufferSize() };
    pso.HS = { m_tessHS->GetBufferPointer(), m_tessHS->GetBufferSize() };
    pso.DS = { m_tessDS->GetBufferPointer(), m_tessDS->GetBufferSize() };
    pso.PS = { m_tessPS->GetBufferPointer(), m_tessPS->GetBufferSize() };

    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = GBuffer::RT_COUNT;
    for (UINT i = 0; i < GBuffer::RT_COUNT; ++i)
        pso.RTVFormats[i] = GBuffer::FORMATS[i];
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = { 1, 0 };

    HRESULT hr = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_tessPSO));
    if (FAILED(hr)) OutputDebugStringA("TessPSO creation failed\n");

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    hr = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_tessWirePSO));
    if (FAILED(hr)) OutputDebugStringA("TessWirePSO creation failed\n");
}

void RenderingSystem::CreateTessCB()
{
    m_tessCBSlotSize = (sizeof(TessCBData) + 255) & ~255;
    UINT total = m_tessCBSlotSize * FRAME_COUNT;
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(total);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_tessCB)));
    m_tessCB->Map(0, nullptr, reinterpret_cast<void**>(&m_tessCBMapped));
}

bool RenderingSystem::LoadTessPlane(const std::wstring& dispPath,
    const std::wstring& normalPath,
    const std::wstring& colorPath,
    XMFLOAT3 center, float size,
    float displacementScale)
{
    if (!m_initialized) return false;
    FlushCommandQueue();

    m_tessDispScale = displacementScale;

    const int N = 64;
    float half = size * 0.5f;
    float step = size / N;

    std::vector<Vertex> verts;
    std::vector<UINT>   indices;
    verts.reserve((N + 1) * (N + 1));

    for (int row = 0; row <= N; ++row)
        for (int col = 0; col <= N; ++col)
        {
            Vertex v;
            v.Position = {
                center.x - half + col * step,
                center.y,
                center.z - half + row * step
            };
            v.Normal = { 0.f, 1.f, 0.f };
            v.TexCoord = { (float)col / N, (float)row / N };
            verts.push_back(v);
        }

    for (int row = 0; row < N; ++row)
        for (int col = 0; col < N; ++col)
        {
            UINT tl = row * (N + 1) + col;
            UINT tr = tl + 1;
            UINT bl = tl + (N + 1);
            UINT br = bl + 1;
            indices.push_back(tl); indices.push_back(tr); indices.push_back(bl);
            indices.push_back(tr); indices.push_back(br); indices.push_back(bl);
        }
    m_tessIndexCount = (UINT)indices.size();

    auto upload = [&](const void* data, UINT sz, ComPtr<ID3D12Resource>& buf) {
        CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)));
        void* p = nullptr; buf->Map(0, nullptr, &p); memcpy(p, data, sz); buf->Unmap(0, nullptr);
        };

    UINT vbSz = (UINT)(verts.size() * sizeof(Vertex));
    UINT ibSz = (UINT)(indices.size() * sizeof(UINT));
    upload(verts.data(), vbSz, m_tessVB);
    upload(indices.data(), ibSz, m_tessIB);
    m_tessVBView = { m_tessVB->GetGPUVirtualAddress(), vbSz, sizeof(Vertex) };
    m_tessIBView = { m_tessIB->GetGPUVirtualAddress(), ibSz, DXGI_FORMAT_R32_UINT };

    ThrowIfFailed(m_cmdAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr));

    int dispSlot = MAX_TEXTURES + GBuffer::RT_COUNT + 1;
    int normSlot = dispSlot + 1;
    int colorSlot = dispSlot + 2;

    TextureLoader::TextureData td;

    OutputDebugStringW((L"TessPlane loading disp: " + dispPath + L"\n").c_str());
    bool dispOk = TextureLoader::LoadFromFile(dispPath, td);
    OutputDebugStringA(dispOk ? "Disp LoadFromFile OK\n" : "Disp LoadFromFile FAILED\n");
    if (dispOk && TextureLoader::CreateTexture(m_device.Get(), m_cmdList.Get(),
        td, m_tessDispTex, m_tessDispUpload))
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = td.format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        CD3DX12_CPU_DESCRIPTOR_HANDLE h(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(), dispSlot, m_srvDescSize);
        m_device->CreateShaderResourceView(m_tessDispTex.Get(), &srvDesc, h);
        m_tessDispSRV = dispSlot;
    }

    if (TextureLoader::LoadFromFile(normalPath, td) &&
        TextureLoader::CreateTexture(m_device.Get(), m_cmdList.Get(),
            td, m_tessNormTex, m_tessNormUpload))
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = td.format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        CD3DX12_CPU_DESCRIPTOR_HANDLE h(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(), normSlot, m_srvDescSize);
        m_device->CreateShaderResourceView(m_tessNormTex.Get(), &srvDesc, h);
        m_tessNormSRV = normSlot;
    }

    if (TextureLoader::LoadFromFile(colorPath, td) &&
        TextureLoader::CreateTexture(m_device.Get(), m_cmdList.Get(),
            td, m_tessColorTex, m_tessColorUpload))
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = td.format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        CD3DX12_CPU_DESCRIPTOR_HANDLE h(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(), colorSlot, m_srvDescSize);
        m_device->CreateShaderResourceView(m_tessColorTex.Get(), &srvDesc, h);
        m_tessColorSRV = colorSlot;
    }

    ThrowIfFailed(m_cmdList->Close());
    ID3D12CommandList* cmds[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmds);
    WaitForGPU();

    m_tessDispUpload.Reset();
    m_tessNormUpload.Reset();
    m_tessColorUpload.Reset();
    m_tessReady = (m_tessDispSRV >= 0 && m_tessPSO);
    return m_tessReady;
}


void RenderingSystem::DoTessPass(float totalTime)
{
    if (!m_tessReady) return;

    TessCBData cb{};
    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view = XMMatrixLookAtLH(
        XMLoadFloat3(&m_eye), XMLoadFloat3(&m_target), XMLoadFloat3(&m_up));
    float aspect = (float)m_width / (float)m_height;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.f), aspect, 1.f, 10000.f);
    XMMATRIX wit = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.WorldInvTranspose, XMMatrixTranspose(wit));
    cb.EyePos = m_eye;
    cb.DisplacementScale = m_tessDispScale;
    cb.MinDist = 100.f;
    cb.MaxDist = 1500.f;
    cb.MinTessFactor = 1.f;
    cb.MaxTessFactor = 16.f;
    cb.TotalTime = totalTime;

    UINT8* slot = m_tessCBMapped + m_frameIndex * m_tessCBSlotSize;
    memcpy(slot, &cb, sizeof(cb));
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
        m_tessCB->GetGPUVirtualAddress() + m_frameIndex * m_tessCBSlotSize;

    m_cmdList->SetPipelineState(m_tessWireframe ? m_tessWirePSO.Get() : m_tessPSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_tessRS.Get());

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_tessVBView);
    m_cmdList->IASetIndexBuffer(&m_tessIBView);

    m_cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvH(
        m_srvHeap->GetGPUDescriptorHandleForHeapStart(),
        m_tessDispSRV, m_srvDescSize);
    m_cmdList->SetGraphicsRootDescriptorTable(1, srvH);

    m_cmdList->DrawIndexedInstanced(m_tessIndexCount, 1, 0, 0, 0);
}