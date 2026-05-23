#include "ShadowMapSystem.h"
#include <cmath>
#include <stdio.h>

// ============================================================
// Init
// ============================================================
bool ShadowMapSystem::Init(ID3D12Device* device)
{
    m_device = device;
    if (!CompileShader())            return false;
    if (!CreateResource(device))     return false;
    if (!CreateDsvHeap(device))      return false;
    if (!CreateRootSignature(device))return false;
    if (!CreatePSO(device))          return false;
    if (!CreateCB(device))           return false;
    m_initialized = true;
    return true;
}

// ============================================================
// Shader
// ============================================================
bool ShadowMapSystem::CompileShader()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompileFromFile(L"ShadowMapShader.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", flags, 0, &m_vs, &err);
    if (FAILED(hr))
    {
        char buf[2048];
        if (err && err->GetBufferPointer())
            sprintf_s(buf, "ShadowMapShader compile error:\n%s", (char*)err->GetBufferPointer());
        else
            sprintf_s(buf, "ShadowMapShader.hlsl not found (HRESULT=0x%08X)", (unsigned)hr);
        MessageBoxA(nullptr, buf, "ShadowMapSystem", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

// ============================================================
// Resource (Texture2DArray, depth, R32_TYPELESS)
// ============================================================
bool ShadowMapSystem::CreateResource(ID3D12Device* device)
{
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32_TYPELESS,
        CSM_RESOLUTION, CSM_RESOLUTION,
        (UINT16)CSM_CASCADE_COUNT, 1);
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_D32_FLOAT;
    clearVal.DepthStencil.Depth = 1.0f;

    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal,
        IID_PPV_ARGS(&m_shadowArray));
    if (FAILED(hr)) return false;
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

// ============================================================
// DSV heap - один DSV на каскад
// ============================================================
bool ShadowMapSystem::CreateDsvHeap(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    d.NumDescriptors = CSM_CASCADE_COUNT;
    if (FAILED(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_dsvHeap))))
        return false;
    m_dsvDescSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (UINT i = 0; i < CSM_CASCADE_COUNT; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.MipSlice = 0;
        dsv.Texture2DArray.FirstArraySlice = i;
        dsv.Texture2DArray.ArraySize = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            m_dsvHeap->GetCPUDescriptorHandleForHeapStart(), (INT)i, m_dsvDescSize);
        device->CreateDepthStencilView(m_shadowArray.Get(), &dsv, handle);
    }
    return true;
}

// ============================================================
// Root signature: b0 = CB (только)
// ============================================================
bool ShadowMapSystem::CreateRootSignature(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER params[1];
    params[0].InitAsConstantBufferView(0);

    CD3DX12_ROOT_SIGNATURE_DESC desc(1, params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr))
    {
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        return false;
    }
    return SUCCEEDED(device->CreateRootSignature(0,
        blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&m_rs)));
}

// ============================================================
// PSO: depth-only, slope-scaled bias, без PS
// ============================================================
bool ShadowMapSystem::CreatePSO(ID3D12Device* device)
{
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { layout, _countof(layout) };
    pso.pRootSignature = m_rs.Get();
    pso.VS = { m_vs->GetBufferPointer(), m_vs->GetBufferSize() };
    // PS: no pixel shader for depth-only pass

    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.DepthBias = 500;    // умеренный bias против acne
    pso.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pso.RasterizerState.DepthBiasClamp = 0.0f;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc = { 1, 0 };

    return SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

// ============================================================
// CB pool
// ============================================================
bool ShadowMapSystem::CreateCB(ID3D12Device* device)
{
    m_cbSlotSize = (sizeof(ShadowVsCB) + 255) & ~255;
    UINT total = m_cbSlotSize * CSM_CASCADE_COUNT * FRAME_COUNT;

    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   rd = CD3DX12_RESOURCE_DESC::Buffer(total);

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb))))
        return false;

    m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped));
    return true;
}

// ============================================================
// SRV in external (renderer's) heap
// ============================================================
void ShadowMapSystem::CreateSrvInExternalHeap(D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.MostDetailedMip = 0;
    srv.Texture2DArray.FirstArraySlice = 0;
    srv.Texture2DArray.ArraySize = CSM_CASCADE_COUNT;

    m_device->CreateShaderResourceView(m_shadowArray.Get(), &srv, dst);
}

// ============================================================
// Cascade computation (practical split + sphere bounding)
// ============================================================
void ShadowMapSystem::ComputeCascades(
    XMFLOAT3 eye, XMFLOAT3 target, XMFLOAT3 upV,
    XMFLOAT3 lightDirIn,
    float fovY, float aspect, float nearZ, float farZ)
{
    // 1. Practical split distances
    float splits[CSM_CASCADE_COUNT + 1];
    splits[0] = nearZ;
    splits[CSM_CASCADE_COUNT] = farZ;
    const float lambda = 0.5f;
    for (UINT i = 1; i < CSM_CASCADE_COUNT; ++i)
    {
        float si = (float)i / (float)CSM_CASCADE_COUNT;
        float logS = nearZ * powf(farZ / nearZ, si);
        float uniS = nearZ + (farZ - nearZ) * si;
        splits[i] = lambda * logS + (1.0f - lambda) * uniS;
    }
    m_cascadeFar = { splits[1], splits[2], splits[3], 0.0f };

    // 2. Camera basis
    XMVECTOR vEye = XMLoadFloat3(&eye);
    XMVECTOR vTar = XMLoadFloat3(&target);
    XMVECTOR vUp = XMLoadFloat3(&upV);
    XMVECTOR vFwd = XMVector3Normalize(XMVectorSubtract(vTar, vEye));

    m_cascadeViewMtx = XMMatrixLookAtLH(vEye, vTar, vUp);

    // 3. Light direction (нормализованный)
    XMVECTOR vLightDir = XMVector3Normalize(XMLoadFloat3(&lightDirIn));

    // Up-vector дл€ light view: если свет почти вертикальный, берЄм Z
    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (fabsf(XMVectorGetX(XMVector3Dot(vLightDir, lightUp))) > 0.99f)
        lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    const float tanHalfFov = tanf(fovY * 0.5f);

    // 4. Per-cascade light view-proj через bounding sphere.
    //    ѕростой надЄжный вариант без snap-to-texel.
    for (UINT i = 0; i < CSM_CASCADE_COUNT; ++i)
    {
        float nf = splits[i];
        float ff = splits[i + 1];
        float midZ = (nf + ff) * 0.5f;

        // ÷ентр slice'а в world space
        XMVECTOR centerWorld = XMVectorAdd(vEye, XMVectorScale(vFwd, midZ));

        // –адиус bounding sphere вокруг slice'а
        float halfDiagFar = ff * tanHalfFov * sqrtf(1.0f + aspect * aspect);
        float halfLen = (ff - nf) * 0.5f;
        float radius = sqrtf(halfDiagFar * halfDiagFar + halfLen * halfLen);

        //  амера света сильно отодвинута вдоль -lightDir, чтобы вс€
        // геометри€ сцены гарантированно попала между near и far.
        float backoff = radius * 4.0f;
        XMVECTOR lightPos = XMVectorSubtract(centerWorld,
            XMVectorScale(vLightDir, backoff));

        XMMATRIX lView = XMMatrixLookAtLH(lightPos, centerWorld, lightUp);

        // Ѕольшой запас по глубине: near чуть перед светом, far далеко за центром.
        // Ёто важно дл€ большой сцены типа Sponza - иначе геометри€
        // обрезаетс€ по глубине и не пишетс€ в shadow map.
        float nearZl = 0.5f;
        float farZl = backoff + radius * 4.0f;

        XMMATRIX lProj = XMMatrixOrthographicLH(
            2.0f * radius, 2.0f * radius,
            nearZl, farZl);

        m_lightViewProj[i] = XMMatrixMultiply(lView, lProj);
    }
}

// ============================================================
// Transitions
// ============================================================
void ShadowMapSystem::TransitionToDepthWrite(ID3D12GraphicsCommandList* cmdList)
{
    if (m_currentState == D3D12_RESOURCE_STATE_DEPTH_WRITE) return;
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowArray.Get(),
        m_currentState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->ResourceBarrier(1, &b);
    m_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

void ShadowMapSystem::TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList)
{
    if (m_currentState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowArray.Get(),
        m_currentState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &b);
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

// ============================================================
// Begin cascade (DSV slice + viewport + clear)
// ============================================================
void ShadowMapSystem::BeginCascade(ID3D12GraphicsCommandList* cmdList, UINT cascadeIdx)
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)cascadeIdx, m_dsvDescSize);

    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

    D3D12_VIEWPORT vp{ 0, 0, (float)CSM_RESOLUTION, (float)CSM_RESOLUTION, 0, 1 };
    D3D12_RECT     sc{ 0, 0, (LONG)CSM_RESOLUTION, (LONG)CSM_RESOLUTION };
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
}

// ============================================================
// SetCascadeCB
// ============================================================
void ShadowMapSystem::SetCascadeCB(ID3D12GraphicsCommandList* cmdList,
    UINT cascadeIdx, XMMATRIX worldMtx,
    UINT frameIndex)
{
    UINT slot = frameIndex * CSM_CASCADE_COUNT + cascadeIdx;
    ShadowVsCB cb;
    XMMATRIX wvp = XMMatrixMultiply(worldMtx, m_lightViewProj[cascadeIdx]);
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(wvp));

    memcpy(m_cbMapped + slot * m_cbSlotSize, &cb, sizeof(cb));
    cmdList->SetGraphicsRootConstantBufferView(0,
        m_cb->GetGPUVirtualAddress() + slot * m_cbSlotSize);
}