#include "ParticleSystem.h"
#include <stdio.h>
#include <deque>

namespace {
    struct EmitBatch { UINT count; float lifeRemaining; };
    std::deque<EmitBatch> g_batches;

    void TickBatches(float dt)
    {
        for (auto& b : g_batches) b.lifeRemaining -= dt;
        while (!g_batches.empty() && g_batches.front().lifeRemaining <= 0.0f)
            g_batches.pop_front();
    }
    UINT TotalAlive()
    {
        UINT sum = 0;
        for (auto& b : g_batches) sum += b.count;
        return sum;
    }
    void AddBatch(UINT count, float life)
    {
        if (count > 0) g_batches.push_back({ count, life });
    }
}

static ComPtr<ID3D12Resource> CreateUploadBufferPS(ID3D12Device* dev, UINT64 size)
{
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   rd = CD3DX12_RESOURCE_DESC::Buffer(size);
    ComPtr<ID3D12Resource> buf;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    return buf;
}

static ComPtr<ID3D12Resource> CreateDefaultUavBufferPS(ID3D12Device* dev,
    UINT64 size, D3D12_RESOURCE_STATES initState)
{
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC   rd = CD3DX12_RESOURCE_DESC::Buffer(size,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> buf;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        initState, nullptr, IID_PPV_ARGS(&buf));
    return buf;
}

bool ParticleSystem::Init(ID3D12Device* device, DXGI_FORMAT rtFmt, DXGI_FORMAT dsFmt)
{
    m_device = device;
    if (!CompileShaders())                      return false;
    if (!CreateResources(device))               return false;
    if (!CreateDescriptorHeapAndViews(device))  return false;
    if (!CreateRootSignatures(device))          return false;
    if (!CreatePipelines(device, rtFmt, dsFmt)) return false;
    m_initialized = true;
    return true;
}

static void ReportShaderError(const char* stage, HRESULT hr, ID3DBlob* err)
{
    char buf[2048];
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == 0x80070002)
    {
        sprintf_s(buf, "[%s] HLSL FILE NOT FOUND (HRESULT=0x%08X)", stage, (unsigned)hr);
    }
    else if (err && err->GetBufferPointer())
    {
        sprintf_s(buf, "[%s] HLSL compile error (HRESULT=0x%08X):\n\n%s",
            stage, (unsigned)hr, (const char*)err->GetBufferPointer());
    }
    else
    {
        sprintf_s(buf, "[%s] D3DCompileFromFile failed (HRESULT=0x%08X), no err blob",
            stage, (unsigned)hr);
    }
    MessageBoxA(nullptr, buf, "ParticleSystem error", MB_OK | MB_ICONERROR);
}

bool ParticleSystem::CompileShaders()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    HRESULT hr;

    hr = D3DCompileFromFile(L"ParticleComputeShaders.hlsl", nullptr, nullptr,
        "EmitCS", "cs_5_0", flags, 0, &m_csEmit, &err);
    if (FAILED(hr)) { ReportShaderError("EmitCS", hr, err.Get()); return false; }
    err.Reset();

    hr = D3DCompileFromFile(L"ParticleComputeShaders.hlsl", nullptr, nullptr,
        "UpdateCS", "cs_5_0", flags, 0, &m_csUpdate, &err);
    if (FAILED(hr)) { ReportShaderError("UpdateCS", hr, err.Get()); return false; }
    err.Reset();

    hr = D3DCompileFromFile(L"ParticleRenderShaders.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", flags, 0, &m_vsRender, &err);
    if (FAILED(hr)) { ReportShaderError("VSMain", hr, err.Get()); return false; }
    err.Reset();

    hr = D3DCompileFromFile(L"ParticleRenderShaders.hlsl", nullptr, nullptr,
        "GSMain", "gs_5_0", flags, 0, &m_gsRender, &err);
    if (FAILED(hr)) { ReportShaderError("GSMain", hr, err.Get()); return false; }
    err.Reset();

    hr = D3DCompileFromFile(L"ParticleRenderShaders.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", flags, 0, &m_psRender, &err);
    if (FAILED(hr)) { ReportShaderError("PSMain", hr, err.Get()); return false; }
    return true;
}

bool ParticleSystem::CreateResources(ID3D12Device* device)
{
    const UINT64 partBufSize = (UINT64)sizeof(Particle) * MAX_PARTICLES;

    for (int i = 0; i < 2; ++i)
    {
        m_particleBuf[i] = CreateDefaultUavBufferPS(device, partBufSize,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_counterBuf[i] = CreateDefaultUavBufferPS(device, 256,
            D3D12_RESOURCE_STATE_COPY_DEST);
        if (!m_particleBuf[i] || !m_counterBuf[i]) return false;
    }

    m_counterZeroUpload = CreateUploadBufferPS(device, 16);
    if (!m_counterZeroUpload) return false;
    {
        void* p = nullptr;
        m_counterZeroUpload->Map(0, nullptr, &p);
        memset(p, 0, 16);
        m_counterZeroUpload->Unmap(0, nullptr);
    }

    for (int i = 0; i < FRAME_COUNT; ++i)
    {
        m_updateCB[i] = CreateUploadBufferPS(device, sizeof(ParticleUpdateCB));
        m_renderCB[i] = CreateUploadBufferPS(device, sizeof(ParticleRenderCB));
        if (!m_updateCB[i] || !m_renderCB[i]) return false;
        m_updateCB[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_updateCBMapped[i]));
        m_renderCB[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_renderCBMapped[i]));
    }
    return true;
}

bool ParticleSystem::CreateDescriptorHeapAndViews(ID3D12Device* device)
{
    // 6 slots, all PRE-CREATED. Layout:
    //   slot 0: UAV(buf0, cnt0)
    //   slot 1: UAV(buf1, cnt1)
    //   slot 2: UAV(buf1, cnt1)   - same as slot 1, used when src=1
    //   slot 3: UAV(buf0, cnt0)   - same as slot 0, used when src=0->dst=1, dst path
    //   slot 4: SRV(buf0)
    //   slot 5: SRV(buf1)
    //
    // Compute table at frame F: starts at slot (m_srcIdx==0 ? 0 : 2)
    //   so two consecutive UAVs are: [UAV(src), UAV(dst)]
    // Render SRV: slot (4 + m_dstIdx)

    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = 6;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_srvUavHeap))))
        return false;
    m_descSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto makeUavWithCounter = [&](UINT bufIdx, UINT slot)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = MAX_PARTICLES;
            uav.Buffer.StructureByteStride = sizeof(Particle);
            uav.Buffer.CounterOffsetInBytes = 0;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            device->CreateUnorderedAccessView(
                m_particleBuf[bufIdx].Get(),
                m_counterBuf[bufIdx].Get(),
                &uav, CpuH(slot));
        };

    auto makeSrv = [&](UINT bufIdx, UINT slot)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = MAX_PARTICLES;
            srv.Buffer.StructureByteStride = sizeof(Particle);
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            device->CreateShaderResourceView(
                m_particleBuf[bufIdx].Get(), &srv, CpuH(slot));
        };

    // Config A (src=0, dst=1): table starts at slot 0 -> [UAV(buf0), UAV(buf1)]
    makeUavWithCounter(0, 0);
    makeUavWithCounter(1, 1);
    // Config B (src=1, dst=0): table starts at slot 2 -> [UAV(buf1), UAV(buf0)]
    makeUavWithCounter(1, 2);
    makeUavWithCounter(0, 3);

    makeSrv(0, 4);
    makeSrv(1, 5);

    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE ParticleSystem::GpuH(UINT slot) const
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), (INT)slot, m_descSize);
}
D3D12_CPU_DESCRIPTOR_HANDLE ParticleSystem::CpuH(UINT slot) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), (INT)slot, m_descSize);
}

bool ParticleSystem::CreateRootSignatures(ID3D12Device* device)
{
    {
        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &uavRange);

        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&desc,
            D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) return false;
        if (FAILED(device->CreateRootSignature(0,
            blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&m_computeRS)))) return false;
    }
    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&desc,
            D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) return false;
        if (FAILED(device->CreateRootSignature(0,
            blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&m_renderRS)))) return false;
    }
    return true;
}

bool ParticleSystem::CreatePipelines(ID3D12Device* device,
    DXGI_FORMAT rtFmt, DXGI_FORMAT dsFmt)
{
    auto makeCS = [&](ID3DBlob* blob, ComPtr<ID3D12PipelineState>& out) -> bool
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = m_computeRS.Get();
            d.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
            return SUCCEEDED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)));
        };

    if (!makeCS(m_csEmit.Get(), m_psoEmit))    return false;
    if (!makeCS(m_csUpdate.Get(), m_psoUpdate))  return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_renderRS.Get();
    pso.VS = { m_vsRender->GetBufferPointer(), m_vsRender->GetBufferSize() };
    pso.GS = { m_gsRender->GetBufferPointer(), m_gsRender->GetBufferSize() };
    pso.PS = { m_psRender->GetBufferPointer(), m_psRender->GetBufferSize() };
    pso.InputLayout = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
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

    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoRender))))
        return false;
    return true;
}

void ParticleSystem::InitGpuState(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->CopyBufferRegion(m_counterBuf[0].Get(), 0, m_counterZeroUpload.Get(), 0, 4);
    cmdList->CopyBufferRegion(m_counterBuf[1].Get(), 0, m_counterZeroUpload.Get(), 0, 4);

    D3D12_RESOURCE_BARRIER bars[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_counterBuf[0].Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(m_counterBuf[1].Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmdList->ResourceBarrier(_countof(bars), bars);
}

void ParticleSystem::UpdateAndDraw(
    ID3D12GraphicsCommandList* cmdList,
    UINT                       frameIndex,
    float dt, float totalTime,
    XMMATRIX view, XMMATRIX proj, XMFLOAT3 eye,
    XMFLOAT3 emitterPos, UINT emitCount,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    UINT width, UINT height)
{
    if (!m_psoRender || !m_psoUpdate) return;

    if (m_needsGpuInit)
    {
        InitGpuState(cmdList);
        m_needsGpuInit = false;
    }

    const UINT fi = frameIndex % FRAME_COUNT;
    if (emitCount > MAX_PARTICLES) emitCount = MAX_PARTICLES;

    // Tick CPU tracker with REAL dt (matches shader now).
    TickBatches(dt);

    UINT srcCount = TotalAlive();
    if (srcCount > MAX_PARTICLES) srcCount = MAX_PARTICLES;
    m_aliveTracked = srcCount;

    ParticleUpdateCB ucb{};
    ucb.DeltaTime = dt;
    ucb.TotalTime = totalTime;
    ucb.EmitCount = emitCount;
    ucb.SrcCount = srcCount;
    ucb.EmitterPos = { emitterPos.x, emitterPos.y, emitterPos.z, 0.0f };
    ucb.Gravity = { 0.0f, -100.0f, 0.0f };
    memcpy(m_updateCBMapped[fi], &ucb, sizeof(ucb));

    ParticleRenderCB rcb{};
    XMStoreFloat4x4(&rcb.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&rcb.Proj, XMMatrixTranspose(proj));
    rcb.EyePos = eye;
    memcpy(m_renderCBMapped[fi], &rcb, sizeof(rcb));

    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // ---- Reset counter[dst] = 0 ----
    {
        auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(m_counterBuf[m_dstIdx].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->ResourceBarrier(1, &b0);
        cmdList->CopyBufferRegion(m_counterBuf[m_dstIdx].Get(), 0,
            m_counterZeroUpload.Get(), 0, 4);
        auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(m_counterBuf[m_dstIdx].Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &b1);
    }

    cmdList->SetComputeRootSignature(m_computeRS.Get());
    cmdList->SetComputeRootConstantBufferView(0,
        m_updateCB[fi]->GetGPUVirtualAddress());

    // Pick descriptor table start: 0 if src=0, 2 if src=1
    UINT computeTableStart = (m_srcIdx == 0) ? 0u : 2u;
    cmdList->SetComputeRootDescriptorTable(1, GpuH(computeTableStart));

    // ---- UpdateCS ----
    UINT updateGroups = (srcCount + 63u) / 64u;
    if (updateGroups > 0)
    {
        cmdList->SetPipelineState(m_psoUpdate.Get());
        cmdList->Dispatch(updateGroups, 1, 1);

        D3D12_RESOURCE_BARRIER uavs[] = {
            CD3DX12_RESOURCE_BARRIER::UAV(m_particleBuf[m_dstIdx].Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_counterBuf[m_dstIdx].Get()),
        };
        cmdList->ResourceBarrier(_countof(uavs), uavs);
    }

    // ---- EmitCS ----
    if (emitCount > 0)
    {
        cmdList->SetPipelineState(m_psoEmit.Get());
        UINT emitGroups = (emitCount + 63u) / 64u;
        cmdList->Dispatch(emitGroups, 1, 1);

        D3D12_RESOURCE_BARRIER uavs[] = {
            CD3DX12_RESOURCE_BARRIER::UAV(m_particleBuf[m_dstIdx].Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_counterBuf[m_dstIdx].Get()),
        };
        cmdList->ResourceBarrier(_countof(uavs), uavs);

        AddBatch(emitCount, 5.0f);
    }

    UINT drawCount = srcCount + emitCount;
    if (drawCount > MAX_PARTICLES) drawCount = MAX_PARTICLES;
    m_aliveTracked = drawCount;

    // ---- Render ----
    if (drawCount > 0)
    {
        auto toRender = CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuf[m_dstIdx].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &toRender);

        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
        D3D12_RECT     sc{ 0, 0, (LONG)width, (LONG)height };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &sc);

        cmdList->SetPipelineState(m_psoRender.Get());
        cmdList->SetGraphicsRootSignature(m_renderRS.Get());
        cmdList->SetGraphicsRootConstantBufferView(0,
            m_renderCB[fi]->GetGPUVirtualAddress());
        // SRV slot: 4 if dst=0, 5 if dst=1
        UINT srvSlot = 4u + m_dstIdx;
        cmdList->SetGraphicsRootDescriptorTable(1, GpuH(srvSlot));
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

        cmdList->DrawInstanced(drawCount, 1, 0, 0);

        auto back = CD3DX12_RESOURCE_BARRIER::Transition(m_particleBuf[m_dstIdx].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &back);
    }

    UINT tmp = m_srcIdx; m_srcIdx = m_dstIdx; m_dstIdx = tmp;
}