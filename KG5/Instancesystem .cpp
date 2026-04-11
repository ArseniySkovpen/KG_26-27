#include "InstanceSystem.h"
#include <stdexcept>
#include <cstdlib>
#include <cmath>

// ----------------------------------------------------------------
// Куб: 24 вершины (по 4 на грань, чтобы нормали были правильными),
// 36 индексов.  Каждая вершина — float3 позиция + float3 нормаль.
// ----------------------------------------------------------------
struct CubeVert { float x, y, z, nx, ny, nz; };

static const CubeVert CUBE_VERTS[24] =
{
    // +Z (0,0,1)
    {-1,-1, 1,  0,0,1}, { 1,-1, 1,  0,0,1}, { 1, 1, 1,  0,0,1}, {-1, 1, 1,  0,0,1},
    // -Z (0,0,-1)
    { 1,-1,-1,  0,0,-1}, {-1,-1,-1,  0,0,-1}, {-1, 1,-1,  0,0,-1}, { 1, 1,-1,  0,0,-1},
    // +X (1,0,0)
    { 1,-1, 1,  1,0,0}, { 1,-1,-1,  1,0,0}, { 1, 1,-1,  1,0,0}, { 1, 1, 1,  1,0,0},
    // -X (-1,0,0)
    {-1,-1,-1, -1,0,0}, {-1,-1, 1, -1,0,0}, {-1, 1, 1, -1,0,0}, {-1, 1,-1, -1,0,0},
    // +Y (0,1,0)
    {-1, 1, 1,  0,1,0}, { 1, 1, 1,  0,1,0}, { 1, 1,-1,  0,1,0}, {-1, 1,-1,  0,1,0},
    // -Y (0,-1,0)
    {-1,-1,-1,  0,-1,0}, { 1,-1,-1,  0,-1,0}, { 1,-1, 1,  0,-1,0}, {-1,-1, 1,  0,-1,0},
};

static const UINT CUBE_INDICES[36] =
{
     0, 1, 2,  0, 2, 3,   // +Z
     4, 5, 6,  4, 6, 7,   // -Z
     8, 9,10,  8,10,11,   // +X
    12,13,14, 12,14,15,   // -X
    16,17,18, 16,18,19,   // +Y
    20,21,22, 20,22,23,   // -Y
};

// ----------------------------------------------------------------
// Вспомогательная функция: создаём буфер на upload heap
// ----------------------------------------------------------------
static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, UINT64 size)
{
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   rd = CD3DX12_RESOURCE_DESC::Buffer(size);
    ComPtr<ID3D12Resource> buf;
    device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&buf));
    return buf;
}

// ================================================================
// Init
// ================================================================
bool InstanceSystem::Init(ID3D12Device* device,
    DXGI_FORMAT backBufFormat,
    DXGI_FORMAT depthFormat)
{
    GenerateInstances();

    if (!CreateGeometry(device))     return false;
    if (!CreatePipeline(device, backBufFormat, depthFormat)) return false;
    if (!CreateDynamicBuffers(device)) return false;

    return true;
}

// ----------------------------------------------------------------
// Генерируем инстансы: кубы разбросаны по всему объёму Sponza.
//
// Sponza layout (приблизительно):
//   X: -380 .. +380  (длина атриума)
//   Y:    5 .. +420  (от пола до потолка, несколько ярусов)
//   Z:  -90 .. +90   (ширина атриума)
//
// Распределение по Y разбито на три «этажа», чтобы кубы
// встречались на полу, на балконах и под потолком.
// ----------------------------------------------------------------
void InstanceSystem::GenerateInstances()
{
    srand(42);

    m_instances.reserve(INSTANCE_COUNT);
    m_aabbs.reserve(INSTANCE_COUNT);

    // Границы сцены
    const float X_MIN = -370.f, X_MAX = 370.f;
    const float Z_MIN = -85.f, Z_MAX = 85.f;

    // Три высотных диапазона и их веса (сколько кубов на каждый ярус)
    struct Layer { float yMin, yMax; int count; };
    const Layer LAYERS[] = {
        {   5.f, 100.f, 1000 },  // пол и нижний ярус
        { 120.f, 260.f,  600 },  // балконный ярус
        { 280.f, 410.f,  400 },  // верхний ярус / под потолком
    };

    for (const auto& layer : LAYERS)
    {
        for (int i = 0; i < layer.count; ++i)
        {
            float rx = (float)rand() / RAND_MAX;
            float ry = (float)rand() / RAND_MAX;
            float rz = (float)rand() / RAND_MAX;

            InstanceRecord inst{};
            inst.Position.x = X_MIN + rx * (X_MAX - X_MIN);
            inst.Position.y = layer.yMin + ry * (layer.yMax - layer.yMin);
            inst.Position.z = Z_MIN + rz * (Z_MAX - Z_MIN);

            // Мелкие кубы на верхних ярусах выглядят органичнее
            float sizeRange = (layer.yMin < 110.f) ? 12.f : 7.f;
            float sizeBase = (layer.yMin < 110.f) ? 8.f : 4.f;
            inst.Scale = sizeBase + (float)rand() / RAND_MAX * sizeRange;

            // Цвет меняется по ярусам:
            // низ  — тёплые красно-оранжевые
            // балкон — зеленовато-голубые
            // верх   — холодные синие
            if (layer.yMin < 110.f) {
                inst.Color = { 0.6f + (float)rand() / RAND_MAX * 0.4f,
                               0.2f + (float)rand() / RAND_MAX * 0.3f,
                               0.05f + (float)rand() / RAND_MAX * 0.15f };
            }
            else if (layer.yMin < 270.f) {
                inst.Color = { 0.1f + (float)rand() / RAND_MAX * 0.3f,
                               0.5f + (float)rand() / RAND_MAX * 0.4f,
                               0.2f + (float)rand() / RAND_MAX * 0.4f };
            }
            else {
                inst.Color = { 0.1f + (float)rand() / RAND_MAX * 0.2f,
                               0.2f + (float)rand() / RAND_MAX * 0.3f,
                               0.6f + (float)rand() / RAND_MAX * 0.4f };
            }
            inst.Pad = 0.f;

            float s = inst.Scale;
            float cx = inst.Position.x;
            float cy = inst.Position.y;  // Y уже случайный внутри яруса
            float cz = inst.Position.z;

            inst.BoundingBox.Min = { cx - s, cy - s, cz - s };
            inst.BoundingBox.Max = { cx + s, cy + s, cz + s };

            m_aabbs.push_back(inst.BoundingBox);
            m_instances.push_back(inst);
        }
    }

    // Строим октодерево по всему объёму сцены
    AABB scene;
    scene.Min = { -400.f,  -10.f, -120.f };
    scene.Max = { 400.f,  430.f,  120.f };
    m_octree.Build(m_aabbs, scene);
}

// ----------------------------------------------------------------
// Загружаем куб в upload-буферы (без отдельного cmdList)
// ----------------------------------------------------------------
bool InstanceSystem::CreateGeometry(ID3D12Device* device)
{
    const UINT vbSize = sizeof(CUBE_VERTS);
    const UINT ibSize = sizeof(CUBE_INDICES);

    m_cubeVB = CreateUploadBuffer(device, vbSize);
    m_cubeIB = CreateUploadBuffer(device, ibSize);
    if (!m_cubeVB || !m_cubeIB) return false;

    // Записываем вершины
    void* ptr = nullptr;
    m_cubeVB->Map(0, nullptr, &ptr);
    memcpy(ptr, CUBE_VERTS, vbSize);
    m_cubeVB->Unmap(0, nullptr);

    m_cubeIB->Map(0, nullptr, &ptr);
    memcpy(ptr, CUBE_INDICES, ibSize);
    m_cubeIB->Unmap(0, nullptr);

    m_cubeVBView.BufferLocation = m_cubeVB->GetGPUVirtualAddress();
    m_cubeVBView.SizeInBytes = vbSize;
    m_cubeVBView.StrideInBytes = sizeof(CubeVert);

    m_cubeIBView.BufferLocation = m_cubeIB->GetGPUVirtualAddress();
    m_cubeIBView.SizeInBytes = ibSize;
    m_cubeIBView.Format = DXGI_FORMAT_R32_UINT;

    m_cubeIndexCount = _countof(CUBE_INDICES);
    return true;
}

// ----------------------------------------------------------------
// Root signature + PSO
// ----------------------------------------------------------------
bool InstanceSystem::CreatePipeline(ID3D12Device* device,
    DXGI_FORMAT rtFmt,
    DXGI_FORMAT dsFmt)
{
    // ---------- Компилируем шейдеры ----------
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    HRESULT hr;

    hr = D3DCompileFromFile(L"InstanceShader.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", flags, 0, &m_vsBlob, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        return false;
    }

    hr = D3DCompileFromFile(L"InstanceShader.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", flags, 0, &m_psBlob, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        return false;
    }

    // ---------- Root signature: один Root CBV (b0) ----------
    CD3DX12_ROOT_PARAMETER params[1];
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc(1, params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> rsBlob, rsErr;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &rsBlob, &rsErr);
    hr = device->CreateRootSignature(0,
        rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr)) return false;

    // ---------- Input layout ----------
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        // Stream 0: per-vertex
        { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
        { "NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
        // Stream 1: per-instance (мировая матрица — 4 × float4 + float4 цвет)
        { "INST_WORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INST_WORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INST_WORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INST_WORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INST_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // ---------- PSO ----------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.VS = { m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize() };
    psoDesc.PS = { m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize() };
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtFmt;
    psoDesc.DSVFormat = dsFmt;
    psoDesc.SampleDesc = { 1, 0 };
    psoDesc.SampleMask = UINT_MAX;

    // Растеризатор — сплошной
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    // Blend — непрозрачный
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // Depth: тест включён, запись включена (инстансы перекрывают друг друга корректно)
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
    if (FAILED(hr)) { OutputDebugStringA("InstanceSystem: PSO failed\n"); return false; }

    // Wireframe PSO
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_psowire));
    if (FAILED(hr)) { OutputDebugStringA("InstanceSystem: Wire PSO failed\n"); return false; }

    return true;
}

// ----------------------------------------------------------------
// Динамические буферы (один на кадр, постоянно замаплены)
// ----------------------------------------------------------------
bool InstanceSystem::CreateDynamicBuffers(ID3D12Device* device)
{
    const UINT64 instBufSize = (UINT64)sizeof(InstanceGPUData) * MAX_INSTANCES;
    const UINT64 cbSize = sizeof(InstanceFrameCB);    // уже выровнен до 256

    for (int f = 0; f < FRAME_COUNT; ++f)
    {
        m_instBuf[f] = CreateUploadBuffer(device, instBufSize);
        if (!m_instBuf[f]) return false;
        m_instBuf[f]->Map(0, nullptr, reinterpret_cast<void**>(&m_instMapped[f]));

        m_cb[f] = CreateUploadBuffer(device, cbSize);
        if (!m_cb[f]) return false;
        m_cb[f]->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped[f]));
    }
    return true;
}

// ================================================================
// Draw — вызывается между DrawScene() и EndFrame()
// ================================================================
void InstanceSystem::Draw(
    ID3D12GraphicsCommandList* cmdList,
    UINT                       frameIndex,
    XMMATRIX                   view,
    XMMATRIX                   proj,
    XMFLOAT3                   eye,
    XMFLOAT3                   lightDir,
    XMFLOAT3                   lightColor,
    XMFLOAT3                   ambient,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
    D3D12_CPU_DESCRIPTOR_HANDLE gbufferDSV,
    UINT width, UINT height,
    bool wireframe)
{
    if (!m_pso || !m_rootSig) return;

    // ---------- 1. Frustum culling ----------
    XMMATRIX vp = XMMatrixMultiply(view, proj);
    std::vector<int> visible;

    if (!m_useFrustum)
    {
        // Без отсечения: все инстансы
        visible.resize(INSTANCE_COUNT);
        for (int i = 0; i < INSTANCE_COUNT; ++i) visible[i] = i;
    }
    else if (m_useOctree)
    {
        // Frustum culling через октодерево
        Frustum f = ExtractFrustum(vp);
        m_octree.Query(f, visible);
    }
    else
    {
        // Прямой перебор AABB без октодерева
        Frustum f = ExtractFrustum(vp);
        visible.reserve(INSTANCE_COUNT);
        for (int i = 0; i < INSTANCE_COUNT; ++i)
            if (TestAABBFrustum(f, m_aabbs[i]))
                visible.push_back(i);
    }
    m_visibleCount = (int)visible.size();

    if (m_visibleCount == 0) return;

    // ---------- 2. Заполняем instance buffer текущего кадра ----------
    const UINT fi = frameIndex % FRAME_COUNT;

    for (int k = 0; k < m_visibleCount; ++k)
    {
        const InstanceRecord& rec = m_instances[visible[k]];
        float s = rec.Scale;
        float cx = rec.Position.x;
        float cy = rec.Position.y;        // центр уже задан случайно внутри яруса
        float cz = rec.Position.z;

        // Масштаб + перенос (без вращения)
        XMMATRIX world = XMMatrixScaling(s, s, s) * XMMatrixTranslation(cx, cy, cz);

        XMFLOAT4X4 w;
        XMStoreFloat4x4(&w, world);   // row-major — строки идут в World0..3

        InstanceGPUData& g = m_instMapped[fi][k];
        g.World0 = { w._11, w._12, w._13, w._14 };
        g.World1 = { w._21, w._22, w._23, w._24 };
        g.World2 = { w._31, w._32, w._33, w._34 };
        g.World3 = { w._41, w._42, w._43, w._44 };
        g.Color = rec.Color;
        g.Pad = 0.f;
    }

    // ---------- 3. Обновляем constant buffer ----------
    InstanceFrameCB& cb = *m_cbMapped[fi];
    XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
    cb.EyePos = eye;
    cb.LightDir = lightDir;
    cb.LightColor = lightColor;
    cb.Ambient = ambient;

    // ---------- 4. Настраиваем Pipeline ----------
    cmdList->SetPipelineState(wireframe ? m_psowire.Get() : m_pso.Get());
    cmdList->SetGraphicsRootSignature(m_rootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, m_cb[fi]->GetGPUVirtualAddress());

    // ---------- 5. Render Target = back buffer + GBuffer depth ----------
    cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, &gbufferDSV);

    D3D12_VIEWPORT vp2{ 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
    D3D12_RECT     sc{ 0, 0, (LONG)width, (LONG)height };
    cmdList->RSSetViewports(1, &vp2);
    cmdList->RSSetScissorRects(1, &sc);

    // ---------- 6. Вершинный / индексный буферы ----------
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW vbs[2] = { m_cubeVBView, {} };
    vbs[1].BufferLocation = m_instBuf[fi]->GetGPUVirtualAddress();
    vbs[1].SizeInBytes = (UINT)(sizeof(InstanceGPUData) * m_visibleCount);
    vbs[1].StrideInBytes = sizeof(InstanceGPUData);

    cmdList->IASetVertexBuffers(0, 2, vbs);
    cmdList->IASetIndexBuffer(&m_cubeIBView);

    // ---------- 7. Instanced draw ----------
    cmdList->DrawIndexedInstanced(m_cubeIndexCount, (UINT)m_visibleCount, 0, 0, 0);
}