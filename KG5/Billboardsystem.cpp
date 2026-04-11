#include "BillboardSystem.h"
#include <cstdlib>
#include <cmath>
#include <vector>

// ================================================================
// Шейдеры встроены как строки — никаких внешних .hlsl файлов не нужно
// ================================================================
static const char* BILLBOARD_HLSL = R"HLSL(
cbuffer BillboardCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float3   gEyePos;   float gPad0;
    float3   gAmbient;  float gPad1;
    float3   gSunDir;   float gPad2;
    float3   gSunColor; float gPad3;
};
struct VSInput
{
    float2 CornerOffset : POSITION;
    float2 UV           : TEXCOORD;
    float3 WorldPos  : INST_POS;
    float  Height    : INST_HEIGHT;
    float  Width     : INST_WIDTH;
    float3 Color     : INST_COLOR;
};
struct VSOutput
{
    float4 PosH   : SV_POSITION;
    float2 UV     : TEXCOORD;
    float3 Color  : COLOR;
};
VSOutput VSMain(VSInput vin)
{
    float3 center = vin.WorldPos + float3(0, vin.Height * 0.5f, 0);
    float4 centerV = mul(float4(center, 1.0f), gView);
    centerV.x += vin.CornerOffset.x * vin.Width  * 0.5f;
    centerV.y += vin.CornerOffset.y * vin.Height * 0.5f;
    VSOutput vout;
    vout.PosH  = mul(centerV, gProj);
    vout.UV    = vin.UV;
    vout.Color = vin.Color;
    return vout;
}
float4 PSMain(VSOutput pin) : SV_Target
{
    float2 uv = pin.UV;
    float2 crownUV  = float2(uv.x, (uv.y - 0.25f) / 0.75f);
    float2 crownPos = crownUV * 2.0f - 1.0f;
    float  crownR   = length(crownPos);
    bool isTrunk = (uv.y < 0.25f) && (abs(uv.x - 0.5f) < 0.12f);
    bool isCrown = (uv.y >= 0.25f) && (crownR < 1.0f);
    if (!isTrunk && !isCrown) discard;
    float3 color;
    if (isTrunk)
        color = float3(0.38f, 0.22f, 0.08f);
    else {
        float edge = 1.0f - smoothstep(0.6f, 1.0f, crownR);
        color = pin.Color * edge;
    }
    float3 N = float3(0, 0, -1);
    float NdotL = max(dot(N, gSunDir), 0.0f);
    float3 lit  = gAmbient * color + NdotL * gSunColor * color;
    return float4(saturate(lit), 1.0f);
}
)HLSL";

static const char* TREE_HLSL = R"HLSL(
cbuffer TreeCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float3   gEyePos;   float gPad0;
    float3   gAmbient;  float gPad1;
    float3   gSunDir;   float gPad2;
    float3   gSunColor; float gPad3;
};
struct VSInput
{
    float3 LocalPos : POSITION;
    float3 Normal   : NORMAL;
    float  MatId    : TEXCOORD;
    float3 WorldPos : INST_POS;
    float  Height   : INST_HEIGHT;
    float  Width    : INST_WIDTH;
    float3 Color    : INST_COLOR;
};
struct VSOutput
{
    float4 PosH  : SV_POSITION;
    float3 Color : COLOR;
};
VSOutput VSMain(VSInput vin)
{
    float4 baseV = mul(float4(vin.WorldPos, 1.f), gView);
    baseV.x += vin.LocalPos.x * vin.Width;
    baseV.y += vin.LocalPos.y * vin.Height;
    VSOutput vout;
    vout.PosH = mul(baseV, gProj);
    if (vin.MatId < 0.5f)
        vout.Color = float3(0.38f, 0.22f, 0.08f);
    else
        vout.Color = vin.Color;
    return vout;
}
float4 PSMain(VSOutput pin) : SV_Target
{
    return float4(pin.Color, 1.f);
}
)HLSL";


// ================================================================
// Вершина flat-дерева (один треугольник — силуэт)
// ================================================================
struct Tree3DVert { float x, y, z, nx, ny, nz, matId; };

// Один треугольник в unit-пространстве:
//   низ-левый (-0.5, 0, 0), низ-правый (0.5, 0, 0), вершина (0, 1, 0).
// В VS он поворачивается на камеру так же, как billboard —
// расширяется в view-space до проекции.
static void BuildTreeMesh(std::vector<Tree3DVert>& verts, std::vector<UINT>& idx)
{
    // Нормаль смотрит на зрителя (будет пересчитана в VS через view-space трюк)
    verts.push_back({ -0.5f, 0.f, 0.f,  0.f, 0.f, -1.f,  0.f }); // низ-левый  (ствол)
    verts.push_back({ 0.5f, 0.f, 0.f,  0.f, 0.f, -1.f,  0.f }); // низ-правый (ствол)
    verts.push_back({ 0.f,  1.f, 0.f,  0.f, 0.f, -1.f,  1.f }); // вершина    (крона)

    idx.push_back(0); idx.push_back(1); idx.push_back(2);
}

// ================================================================
// Вершина quad для billboard
// ================================================================
struct QuadVert { float cx, cy, u, v; };

static const QuadVert QUAD_VERTS[4] =
{
    {-1.f,-1.f, 0.f,1.f}, {+1.f,-1.f, 1.f,1.f},
    {+1.f,+1.f, 1.f,0.f}, {-1.f,+1.f, 0.f,0.f},
};
static const UINT QUAD_IDX[6] = { 0,1,2, 0,2,3 };

// ================================================================
static ComPtr<ID3D12Resource> UploadBuf(ID3D12Device* dev, UINT64 sz)
{
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   rd = CD3DX12_RESOURCE_DESC::Buffer(sz);
    ComPtr<ID3D12Resource> b;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&b));
    return b;
}

// ================================================================
bool BillboardSystem::Init(ID3D12Device* device,
    DXGI_FORMAT   rtFmt,
    DXGI_FORMAT   dsFmt)
{
    GenerateTrees();

    if (!CreateQuadGeometry(device)) {
        MessageBoxA(nullptr, "BillboardSystem: CreateQuadGeometry failed", "Init", MB_OK);
        return false;
    }
    if (!CreateTreeGeometry(device)) {
        MessageBoxA(nullptr, "BillboardSystem: CreateTreeGeometry failed", "Init", MB_OK);
        return false;
    }
    if (!CreateBillboardPipeline(device, rtFmt, dsFmt)) {
        MessageBoxA(nullptr,
            "BillboardSystem: Billboard PSO failed\n"
            "Убедись, что BillboardShader.hlsl лежит рядом с .exe",
            "Init", MB_OK);
        return false;
    }
    if (!CreateTreePipeline(device, rtFmt, dsFmt)) {
        MessageBoxA(nullptr,
            "BillboardSystem: Tree PSO failed\n"
            "Убедись, что TreeShader.hlsl лежит рядом с .exe",
            "Init", MB_OK);
        return false;
    }
    if (!CreateDynamicBuffers(device)) {
        MessageBoxA(nullptr, "BillboardSystem: CreateDynamicBuffers failed", "Init", MB_OK);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
void BillboardSystem::GenerateTrees()
{
    srand(77);
    m_trees.reserve(TREE_COUNT);
    m_aabbs.reserve(TREE_COUNT);

    struct Wall { float spreadMin, spreadMax, depthMin, depthMax; bool alongX; int count; };
    const Wall WALLS[] = {
        {-1500.f, 1500.f, -2000.f, -900.f, false, 75},
        {-1500.f, 1500.f,   900.f, 2000.f, false, 75},
        {-2000.f, -900.f, -1500.f, 1500.f, true,  75},
        {  900.f, 2000.f, -1500.f, 1500.f, true,  75},
    };

    for (const auto& w : WALLS)
    {
        for (int i = 0; i < w.count; ++i)
        {
            float r1 = (float)rand() / RAND_MAX;
            float r2 = (float)rand() / RAND_MAX;
            float spread = w.spreadMin + r1 * (w.spreadMax - w.spreadMin);
            float depth = w.depthMin + r2 * (w.depthMax - w.depthMin);

            TreeRecord t{};
            t.Position.x = w.alongX ? depth : spread;
            t.Position.y = 0.f;
            t.Position.z = w.alongX ? spread : depth;
            t.Height = 150.f + (float)rand() / RAND_MAX * 200.f;
            t.Width = 100.f + (float)rand() / RAND_MAX * 150.f;

            float g = 0.35f + (float)rand() / RAND_MAX * 0.45f;
            float r = 0.05f + (float)rand() / RAND_MAX * 0.20f;
            float b = 0.02f + (float)rand() / RAND_MAX * 0.12f;
            t.Color = { r, g, b };

            float hw = t.Width * 0.5f;
            float cx = t.Position.x, cz = t.Position.z;
            t.Box.Min = { cx - hw, 0.f,      cz - hw };
            t.Box.Max = { cx + hw, t.Height, cz + hw };

            m_aabbs.push_back(t.Box);
            m_trees.push_back(t);
        }
    }
}

// ----------------------------------------------------------------
bool BillboardSystem::CreateQuadGeometry(ID3D12Device* device)
{
    m_quadVB = UploadBuf(device, sizeof(QUAD_VERTS));
    m_quadIB = UploadBuf(device, sizeof(QUAD_IDX));
    if (!m_quadVB || !m_quadIB) return false;

    void* p;
    m_quadVB->Map(0, nullptr, &p); memcpy(p, QUAD_VERTS, sizeof(QUAD_VERTS)); m_quadVB->Unmap(0, nullptr);
    m_quadIB->Map(0, nullptr, &p); memcpy(p, QUAD_IDX, sizeof(QUAD_IDX));   m_quadIB->Unmap(0, nullptr);

    m_quadVBView = { m_quadVB->GetGPUVirtualAddress(), sizeof(QUAD_VERTS), sizeof(QuadVert) };
    m_quadIBView = { m_quadIB->GetGPUVirtualAddress(), sizeof(QUAD_IDX),   DXGI_FORMAT_R32_UINT };
    return true;
}

// ----------------------------------------------------------------
bool BillboardSystem::CreateTreeGeometry(ID3D12Device* device)
{
    std::vector<Tree3DVert> verts;
    std::vector<UINT>       idx;
    BuildTreeMesh(verts, idx);
    m_treeIndexCount = (UINT)idx.size();

    const UINT vbSz = (UINT)(verts.size() * sizeof(Tree3DVert));
    const UINT ibSz = (UINT)(idx.size() * sizeof(UINT));

    m_treeVB = UploadBuf(device, vbSz);
    m_treeIB = UploadBuf(device, ibSz);
    if (!m_treeVB || !m_treeIB) return false;

    void* p;
    m_treeVB->Map(0, nullptr, &p); memcpy(p, verts.data(), vbSz); m_treeVB->Unmap(0, nullptr);
    m_treeIB->Map(0, nullptr, &p); memcpy(p, idx.data(), ibSz); m_treeIB->Unmap(0, nullptr);

    m_treeVBView = { m_treeVB->GetGPUVirtualAddress(), vbSz, sizeof(Tree3DVert) };
    m_treeIBView = { m_treeIB->GetGPUVirtualAddress(), ibSz, DXGI_FORMAT_R32_UINT };
    return true;
}

// ----------------------------------------------------------------
// Вспомогательная функция: создаём root signature с одним root CBV
// ----------------------------------------------------------------
static bool MakeRootSig(ID3D12Device* device, ComPtr<ID3D12RootSignature>& rs)
{
    CD3DX12_ROOT_PARAMETER param;
    param.InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    CD3DX12_ROOT_SIGNATURE_DESC desc(1, &param, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> blob, err;
    D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    return SUCCEEDED(device->CreateRootSignature(0,
        blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
}

// ----------------------------------------------------------------
bool BillboardSystem::CreateBillboardPipeline(ID3D12Device* device,
    DXGI_FORMAT rtFmt,
    DXGI_FORMAT dsFmt)
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vs, ps, err;
    UINT srcLen = (UINT)strlen(BILLBOARD_HLSL);
    if (FAILED(D3DCompile(BILLBOARD_HLSL, srcLen, "BillboardShader",
        nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vs, &err))) {
        if (err) MessageBoxA(nullptr, (char*)err->GetBufferPointer(), "BillboardShader VS error", MB_OK);
        return false;
    }
    if (FAILED(D3DCompile(BILLBOARD_HLSL, srcLen, "BillboardShader",
        nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &err))) {
        if (err) MessageBoxA(nullptr, (char*)err->GetBufferPointer(), "BillboardShader PS error", MB_OK);
        return false;
    }
    if (!MakeRootSig(device, m_billRootSig)) return false;

    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        {"POSITION",    0, DXGI_FORMAT_R32G32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0},
        {"TEXCOORD",    0, DXGI_FORMAT_R32G32_FLOAT,    0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0},
        {"INST_POS",    0, DXGI_FORMAT_R32G32B32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_HEIGHT", 0, DXGI_FORMAT_R32_FLOAT,       1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_WIDTH",  0, DXGI_FORMAT_R32_FLOAT,       1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_COLOR",  0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 20, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_billRootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtFmt;
    pso.DSVFormat = dsFmt;
    pso.SampleDesc = { 1, 0 };
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_billPSO));
    if (FAILED(hr)) { MessageBoxA(nullptr, "Billboard PSO creation failed", "PSO Error", MB_OK); return false; }
    return true;
}

// ----------------------------------------------------------------
bool BillboardSystem::CreateTreePipeline(ID3D12Device* device,
    DXGI_FORMAT rtFmt,
    DXGI_FORMAT dsFmt)
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vs, ps, err;
    UINT srcLen2 = (UINT)strlen(TREE_HLSL);
    if (FAILED(D3DCompile(TREE_HLSL, srcLen2, "TreeShader",
        nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vs, &err))) {
        if (err) MessageBoxA(nullptr, (char*)err->GetBufferPointer(), "TreeShader VS error", MB_OK);
        return false;
    }
    if (FAILED(D3DCompile(TREE_HLSL, srcLen2, "TreeShader",
        nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &err))) {
        if (err) MessageBoxA(nullptr, (char*)err->GetBufferPointer(), "TreeShader PS error", MB_OK);
        return false;
    }
    if (!MakeRootSig(device, m_treeRootSig)) return false;

    // Stream 0: вершина 3D дерева (pos + normal + matId)
    // Stream 1: per-instance (pos + height + width + color) — тот же layout что у billboard
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        {"POSITION",    0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0},
        {"NORMAL",      0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0},
        {"TEXCOORD",    0, DXGI_FORMAT_R32_FLOAT,          0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0}, // matId
        {"INST_POS",    0, DXGI_FORMAT_R32G32B32_FLOAT,    1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_HEIGHT", 0, DXGI_FORMAT_R32_FLOAT,          1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_WIDTH",  0, DXGI_FORMAT_R32_FLOAT,          1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_COLOR",  0, DXGI_FORMAT_R32G32B32_FLOAT,    1, 20, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_treeRootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtFmt;
    pso.DSVFormat = dsFmt;
    pso.SampleDesc = { 1, 0 };
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // плоский треугольник виден с обеих сторон
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    HRESULT hr2 = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_treePSO));
    if (FAILED(hr2)) { MessageBoxA(nullptr, "Tree PSO creation failed", "PSO Error", MB_OK); return false; }
    return true;
}

// ----------------------------------------------------------------
bool BillboardSystem::CreateDynamicBuffers(ID3D12Device* device)
{
    const UINT64 instSz = sizeof(TreeGPUData) * TREE_COUNT;
    const UINT64 cbSz = sizeof(BillboardCB);

    for (int f = 0; f < FRAME_COUNT; ++f)
    {
        m_instBufLOD0[f] = UploadBuf(device, instSz);
        m_instBufLOD1[f] = UploadBuf(device, instSz);
        m_cb[f] = UploadBuf(device, cbSz);
        if (!m_instBufLOD0[f] || !m_instBufLOD1[f] || !m_cb[f]) return false;

        m_instBufLOD0[f]->Map(0, nullptr, reinterpret_cast<void**>(&m_instLOD0[f]));
        m_instBufLOD1[f]->Map(0, nullptr, reinterpret_cast<void**>(&m_instLOD1[f]));
        m_cb[f]->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped[f]));
    }
    return true;
}

// ================================================================
void BillboardSystem::Draw(
    ID3D12GraphicsCommandList* cmdList,
    UINT                        frameIndex,
    XMMATRIX                    view,
    XMMATRIX                    proj,
    XMFLOAT3                    eye,
    XMFLOAT3                    sunDir,
    XMFLOAT3                    sunColor,
    XMFLOAT3                    ambient,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
    D3D12_CPU_DESCRIPTOR_HANDLE gbufferDSV,
    UINT width, UINT height)
{
    if (!m_billPSO || !m_treePSO) return;

    const UINT fi = frameIndex % FRAME_COUNT;

    // ---- 1. Frustum culling + LOD split ----
    XMMATRIX vp = XMMatrixMultiply(view, proj);
    Frustum  f = ExtractFrustum(vp);
    XMVECTOR eyeV = XMLoadFloat3(&eye);

    int cntLOD0 = 0, cntLOD1 = 0;

    for (int i = 0; i < (int)m_trees.size(); ++i)
    {
        if (!TestAABBFrustum(f, m_aabbs[i])) continue;

        const TreeRecord& t = m_trees[i];

        // Дистанция от камеры до центра дерева
        XMVECTOR treeCenter = XMLoadFloat3(&t.Position);
        treeCenter = XMVectorAdd(treeCenter,
            XMVectorSet(0.f, t.Height * 0.5f, 0.f, 0.f));
        float dist = XMVectorGetX(XMVector3Length(
            XMVectorSubtract(eyeV, treeCenter)));

        TreeGPUData g;
        g.WorldPos = t.Position;
        g.Height = t.Height;
        g.Width = t.Width;
        g.Color = t.Color;

        if (dist < LOD_DISTANCE)
            m_instLOD0[fi][cntLOD0++] = g;  // LOD0: 3D дерево
        else
            m_instLOD1[fi][cntLOD1++] = g;  // LOD1: billboard
    }
    m_visibleCount = cntLOD0 + cntLOD1;

    // ---- 2. Constant buffer ----
    BillboardCB& cb = *m_cbMapped[fi];
    XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
    cb.EyePos = eye;
    cb.Ambient = ambient;
    cb.SunDir = sunDir;
    cb.SunColor = sunColor;

    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_cb[fi]->GetGPUVirtualAddress();

    // ---- 3. RT + Viewport (общий для обоих проходов) ----
    cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, &gbufferDSV);
    D3D12_VIEWPORT vp2{ 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
    D3D12_RECT     sc{ 0, 0, (LONG)width, (LONG)height };
    cmdList->RSSetViewports(1, &vp2);
    cmdList->RSSetScissorRects(1, &sc);

    // ==== LOD0: 3D деревья (близко) ====
    if (cntLOD0 > 0)
    {
        cmdList->SetPipelineState(m_treePSO.Get());
        cmdList->SetGraphicsRootSignature(m_treeRootSig.Get());
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_VERTEX_BUFFER_VIEW vbs[2] = { m_treeVBView, {} };
        vbs[1].BufferLocation = m_instBufLOD0[fi]->GetGPUVirtualAddress();
        vbs[1].SizeInBytes = (UINT)(sizeof(TreeGPUData) * cntLOD0);
        vbs[1].StrideInBytes = sizeof(TreeGPUData);

        cmdList->IASetVertexBuffers(0, 2, vbs);
        cmdList->IASetIndexBuffer(&m_treeIBView);
        cmdList->DrawIndexedInstanced(m_treeIndexCount, (UINT)cntLOD0, 0, 0, 0);
    }

    // ==== LOD1: Billboard (далеко) ====
    if (cntLOD1 > 0)
    {
        cmdList->SetPipelineState(m_billPSO.Get());
        cmdList->SetGraphicsRootSignature(m_billRootSig.Get());
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_VERTEX_BUFFER_VIEW vbs[2] = { m_quadVBView, {} };
        vbs[1].BufferLocation = m_instBufLOD1[fi]->GetGPUVirtualAddress();
        vbs[1].SizeInBytes = (UINT)(sizeof(TreeGPUData) * cntLOD1);
        vbs[1].StrideInBytes = sizeof(TreeGPUData);

        cmdList->IASetVertexBuffers(0, 2, vbs);
        cmdList->IASetIndexBuffer(&m_quadIBView);
        cmdList->DrawIndexedInstanced(6, (UINT)cntLOD1, 0, 0, 0);
    }
}