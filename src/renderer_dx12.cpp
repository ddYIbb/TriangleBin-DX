#include "renderer_dx12.h"

#include <cstring>

namespace {

const char* kVtxShader = R"hlsl(
cbuffer VColorBuffer : register(b0) {
    float4 vcolor[7];
};

struct VSIn {
    float4 pos : POSITION;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

VSOut VSMain(VSIn input, uint vertexId : SV_VertexID) {
    VSOut output;
    output.pos = input.pos;
    output.color = vcolor[vertexId % 7];
    return output;
}
)hlsl";

const char* kFragShader = R"hlsl(
RWByteAddressBuffer counter : register(u1);

cbuffer FrameParams : register(b1) {
    float4 uniformcolor;
    uint frags_to_shade;
    uint3 pad;
};

struct PSIn {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

float4 PSMain(PSIn input) : SV_Target {
    uint cur = counter.Load(0);
    if (cur > frags_to_shade) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    uint prev;
    counter.InterlockedAdd(0, 1, prev);
    if (prev > frags_to_shade) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return input.color;
}
)hlsl";

struct FrameCB {
    float color[4];
    uint32_t frags_to_shade;
    uint32_t pad[3];
};

} // namespace

RendererD3D12::~RendererD3D12() {
    // The swapchain is presented with Present(0,0) (no vsync mailbox); frame
    // pacing to the display refresh rate is done on the CPU. This means there
    // are never flip-model vsync presents pending here, so releasing the
    // swapchain/device during shutdown does not trigger an NVIDIA TDR.
    flush();
    for (int i = 0; i < 3; ++i) {
        if (m_fenceEvents[i]) {
            CloseHandle(m_fenceEvents[i]);
            m_fenceEvents[i] = nullptr;
        }
    }
}

bool RendererD3D12::init(HWND hwnd, int w, int h, int adapterIndex) {
    ComPtr<IDXGIFactory1> factory1;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
        Log(LOG_ERROR) << "CreateDXGIFactory1 failed";
        return false;
    }
    if (FAILED(factory1.As(&factory))) return false;

    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    const UINT numLevels = (UINT)(sizeof(kLevels) / sizeof(kLevels[0]));

    ComPtr<IDXGIAdapter1> adapter;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    int usedIndex = -1;

    if (adapterIndex >= 0) {
        // Use the adapter explicitly chosen in the "Select GPU" window.
        ComPtr<IDXGIAdapter1> a;
        if (FAILED(factory->EnumAdapters1((UINT)adapterIndex, &a))) {
            Log(LOG_WARN) << "Requested D3D12 adapter index " << adapterIndex << " not found";
            return false;
        }
        DXGI_ADAPTER_DESC1 desc1 = {};
        a->GetDesc1(&desc1);
        if (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            Log(LOG_WARN) << "Requested adapter is a software adapter";
            return false;
        }
        ComPtr<ID3D12Device> probe;
        if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(&probe)))) {
            Log(LOG_WARN) << "Requested adapter does not support Direct3D 12";
            return false;
        }
        D3D12_FEATURE_DATA_FEATURE_LEVELS flData = {};
        flData.NumFeatureLevels = numLevels;
        flData.pFeatureLevelsRequested = kLevels;
        if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                                                 &flData, sizeof(flData)))) {
            fl = flData.MaxSupportedFeatureLevel;
        }
        adapter = a;
        usedIndex = adapterIndex;
    } else {
        // Pick the best hardware adapter: highest supported feature level first,
        // then largest dedicated video memory. Software (WARP) adapters are always
        // skipped -- no software rendering path is supported.
        ComPtr<IDXGIAdapter1> chosenAdapter;
        D3D_FEATURE_LEVEL bestFL = (D3D_FEATURE_LEVEL)0;
        SIZE_T bestVRAM = 0;
        int bestIndex = -1;

        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT hr = factory->EnumAdapters1(i, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;

            DXGI_ADAPTER_DESC1 desc1 = {};
            candidate->GetDesc1(&desc1);
            const bool software = (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

            ComPtr<ID3D12Device> probe;
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&probe)))) {
                continue;
            }
            D3D12_FEATURE_DATA_FEATURE_LEVELS flData = {};
            flData.NumFeatureLevels = numLevels;
            flData.pFeatureLevelsRequested = kLevels;
            D3D_FEATURE_LEVEL maxFL = D3D_FEATURE_LEVEL_11_0;
            if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                                                     &flData, sizeof(flData)))) {
                maxFL = flData.MaxSupportedFeatureLevel;
            }

            if (software) continue;

            if (maxFL > bestFL ||
                (maxFL == bestFL && desc1.DedicatedVideoMemory > bestVRAM)) {
                chosenAdapter = candidate;
                bestFL = maxFL;
                bestVRAM = desc1.DedicatedVideoMemory;
                bestIndex = (int)i;
            }
        }

        if (!chosenAdapter) {
            Log(LOG_WARN) << "No Direct3D 12 hardware adapter with feature level 11_0+ found";
            return false;
        }
        adapter = chosenAdapter;
        fl = bestFL;
        usedIndex = bestIndex;
    }
    m_fl = fl;
    m_info.adapterIndex = usedIndex;

    DXGI_ADAPTER_DESC ad = {};
    adapter->GetDesc(&ad);
    {
        size_t len = 0;
        while (len < 128 && ad.Description[len] != L'\0') ++len;
        std::wstring wname(ad.Description, len);
        int req = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                                      nullptr, 0, nullptr, nullptr);
        m_info.adapterName.resize(req);
        WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                            &m_info.adapterName[0], req, nullptr, nullptr);
    }
    LARGE_INTEGER umdVersion = {};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umdVersion))) {
        formatDriverVersion((ULONGLONG)umdVersion.QuadPart, m_info.driverVersion);
    } else {
        m_info.driverVersion = "unknown";
    }

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&m_device)))) {
        Log(LOG_ERROR) << "D3D12CreateDevice failed";
        return false;
    }
    m_fl = fl;
    Log(LOG_INFO) << "Selected Direct3D 12 adapter \"" << m_info.adapterName
                  << "\", feature level " << featureLevelName(m_fl);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))) return false;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = (UINT)w;
    sd.Height = (UINT)h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &sd,
                                                  nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        Log(LOG_ERROR) << "CreateSwapChainForHwnd (D3D12) failed: 0x" << std::hex << (unsigned)hr;
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(sc1.As(&m_swapChain))) return false;
    // Keep at most one frame queued ahead of the display (lower latency).
    m_swapChain->SetMaximumFrameLatency(1);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)))) return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)))) return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 16;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_uavHeap)))) return false;

    m_rtvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 3; ++i) {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])))) return false;
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvCpu);
        rtvCpu.ptr += m_rtvInc;
    }

    if (!createDepth(w, h)) return false;

    for (int i = 0; i < 3; ++i) {
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&m_alloc[i])))) return false;
    }
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           m_alloc[0].Get(), nullptr,
                                           IID_PPV_ARGS(&m_list)))) return false;
    m_list->Close();
    for (int i = 0; i < 3; ++i) {
        if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&m_fences[i])))) return false;
        m_fenceValues[i] = 0;
    }
    for (int i = 0; i < 3; ++i) {
        m_fenceEvents[i] = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvents[i]) return false;
    }

    if (!createRootSignatureAndPSO()) return false;
    if (!createUploadResources()) return false;
    if (!createCounter()) return false;

    m_info.width = w;
    m_info.height = h;
    m_info.apiName = "Direct3D 12";
    return true;
}

bool RendererD3D12::createDepth(int w, int h) {
    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = (UINT)w;
    dd.Height = (UINT)h;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.SampleDesc = {1, 0};
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 1.f;

    HRESULT hr = m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &cv, IID_PPV_ARGS(&m_depth));
    if (FAILED(hr)) return false;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    m_device->CreateDepthStencilView(m_depth.Get(), &dsv,
                                     m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool RendererD3D12::createRootSignatureAndPSO() {
    std::vector<uint8_t> vsBlob, psBlob;
    if (!compileHLSL(kVtxShader, "VSMain", "vs_5_0", vsBlob)) return false;
    if (!compileHLSL(kFragShader, "PSMain", "ps_5_0", psBlob)) return false;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 1;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob;
    ComPtr<ID3DBlob> sigErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &sigErr);
    if (FAILED(hr)) {
        if (sigErr) Log(LOG_ERROR) << "Root signature error: " << (const char*)sigErr->GetBufferPointer();
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                             sigBlob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_rootSig)))) return false;

    D3D12_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = { vsBlob.data(), vsBlob.size() };
    pso.PS = { psBlob.data(), psBlob.size() };
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = 0xFFFFFFFF;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.InputLayout = { layoutDesc, 1 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc = {1, 0};

    HRESULT psoHr = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso));
    if (FAILED(psoHr)) {
        Log(LOG_ERROR) << "CreateGraphicsPipelineState (D3D12) failed: 0x"
                       << std::hex << (unsigned)psoHr;
        return false;
    }
    return true;
}

bool RendererD3D12::createUploadBuffer(ID3D12Device* dev, size_t size,
                                       void** mapped, ID3D12Resource** out) {
    size = (size + 255) & ~(size_t)255;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr)) return false;
    if (mapped) {
        hr = (*out)->Map(0, nullptr, mapped);
        if (FAILED(hr)) {
            (*out)->Release();
            *out = nullptr;
            return false;
        }
    }
    return true;
}

bool RendererD3D12::createUploadResources() {
    // Quad vertices (GL z=0 remapped to D3D z=0.5).
    std::vector<vtxData> quad = vbData;
    for (auto& vert : quad) {
        vert.pos.z = vert.pos.z * 0.5f + 0.5f;
    }
    void* mapped = nullptr;
    if (!createUploadBuffer(m_device.Get(), quad.size() * sizeof(vtxData),
                            &mapped, &m_vb)) return false;
    memcpy(mapped, quad.data(), quad.size() * sizeof(vtxData));

    if (!createUploadBuffer(m_device.Get(), ibData.size() * sizeof(uint16_t),
                            &mapped, &m_ib)) return false;
    memcpy(mapped, ibData.data(), ibData.size() * sizeof(uint16_t));

    float vcolor[7][4] = {
        {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f},
        {1.f, 1.f, 0.f, 1.f}, {1.f, 0.f, 1.f, 1.f}, {0.f, 1.f, 1.f, 1.f},
        {1.f, 1.f, 1.f, 1.f}
    };
    if (!createUploadBuffer(m_device.Get(), sizeof(vcolor), &mapped, &m_cbVS)) return false;
    memcpy(mapped, vcolor, sizeof(vcolor));
    m_cbVSAddr = m_cbVS->GetGPUVirtualAddress();

    for (int i = 0; i < 3; ++i) {
        if (!createUploadBuffer(m_device.Get(), sizeof(FrameCB),
                                &m_cbPSMapped[i], &m_cbPS[i])) return false;
        memset(m_cbPSMapped[i], 0, sizeof(FrameCB));
        m_cbPSAddr[i] = m_cbPS[i]->GetGPUVirtualAddress();

        if (!createUploadBuffer(m_device.Get(), sizeof(uint32_t),
                                &m_zeroMapped[i], &m_zeroSrc[i])) return false;
        *((uint32_t*)m_zeroMapped[i]) = 0;
    }
    return true;
}

bool RendererD3D12::createCounter() {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&m_counter)))) return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = 1; // 4-byte raw buffer = 1 DWORD
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    m_device->CreateUnorderedAccessView(m_counter.Get(), nullptr, &uav,
                                        m_uavHeap->GetCPUDescriptorHandleForHeapStart());
    m_counterUavGpu = m_uavHeap->GetGPUDescriptorHandleForHeapStart();
    m_counterInitialized = false;
    return true;
}

void RendererD3D12::setClearColor(float r, float g, float b, float a) {
    clearColor[0] = r;
    clearColor[1] = g;
    clearColor[2] = b;
    clearColor[3] = a;
}

void RendererD3D12::setFragCount(uint32_t v) {
    frag_count = v;
}

void RendererD3D12::waitForFrame(UINT idx) {
    // init() may fail part-way through, leaving some fences never created. In
    // that case flush() (called from the destructor) must not dereference a
    // null fence.
    if (!m_fences[idx]) return;
    const UINT64 target = m_fenceValues[idx];
    // Bounded wait so a GPU stall can't hang the app forever (e.g. on close).
    int spins = 0;
    while (m_fences[idx]->GetCompletedValue() < target && spins < 40) {
        m_fences[idx]->SetEventOnCompletion(target, m_fenceEvents[idx]);
        WaitForSingleObject(m_fenceEvents[idx], 50);
        spins++;
    }
    if (m_fences[idx]->GetCompletedValue() < target) {
        Log(LOG_WARN) << "waitForFrame(" << idx
                      << ") timed out after ~2s; GPU may be stalled";
    }
}

void RendererD3D12::flush() {
    for (int i = 0; i < 3; ++i) {
        waitForFrame((UINT)i);
    }
}

void RendererD3D12::beginFrame() {
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    UINT bb = m_frameIndex;
    waitForFrame(bb);

    m_alloc[bb]->Reset();
    m_list->Reset(m_alloc[bb].Get(), m_pso.Get());

    D3D12_VIEWPORT vp = {0.f, 0.f, (float)m_info.width, (float)m_info.height, 0.f, 1.f};
    D3D12_RECT sr = {0, 0, m_info.width, m_info.height};
    m_list->RSSetViewports(1, &vp);
    m_list->RSSetScissorRects(1, &sr);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_RESOURCE_BARRIER bar = {};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = m_backBuffers[bb].Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_list->ResourceBarrier(1, &bar);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bb * m_rtvInc;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

    ID3D12DescriptorHeap* heaps[1] = { m_uavHeap.Get() };
    m_list->SetDescriptorHeaps(1, heaps);

    FrameCB* pcb = (FrameCB*)m_cbPSMapped[bb];
    pcb->color[0] = 0.5f;
    pcb->color[1] = 0.5f;
    pcb->color[2] = 0.f;
    pcb->color[3] = 1.f;
    pcb->frags_to_shade = frag_count;
    pcb->pad[0] = pcb->pad[1] = pcb->pad[2] = 0;

    m_list->SetGraphicsRootSignature(m_rootSig.Get());
    m_list->SetGraphicsRootConstantBufferView(0, m_cbVSAddr);
    m_list->SetGraphicsRootConstantBufferView(1, m_cbPSAddr[bb]);
    m_list->SetGraphicsRootDescriptorTable(2, m_counterUavGpu);

    // Reset the fragment counter before drawing.
    *((uint32_t*)m_zeroMapped[bb]) = 0;
    D3D12_RESOURCE_BARRIER cb1 = {};
    cb1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    cb1.Transition.pResource = m_counter.Get();
    cb1.Transition.StateBefore = m_counterInitialized
        ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        : D3D12_RESOURCE_STATE_COMMON;
    cb1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cb1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_list->ResourceBarrier(1, &cb1);
    m_list->CopyBufferRegion(m_counter.Get(), 0, m_zeroSrc[bb].Get(), 0, 4);
    D3D12_RESOURCE_BARRIER cb2 = cb1;
    cb2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    cb2.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_list->ResourceBarrier(1, &cb2);
    m_counterInitialized = true;
}

void RendererD3D12::draw() {
    ensureQuadIndexData();

    UINT bb = m_frameIndex;

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = m_vb->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)(vbData.size() * sizeof(vtxData));
    vbv.StrideInBytes = (UINT)sizeof(vtxData);
    m_list->IASetVertexBuffers(0, 1, &vbv);

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = m_ib->GetGPUVirtualAddress();
    ibv.SizeInBytes = (UINT)(ibData.size() * sizeof(uint16_t));
    ibv.Format = DXGI_FORMAT_R16_UINT;
    m_list->IASetIndexBuffer(&ibv);
    m_list->DrawIndexedInstanced((UINT)ibData.size(), 1, 0, 0, 0);
}

void RendererD3D12::drawRandomTris(int count) {
    ensureRandomTris(count);
    if (vbDataRandom.empty()) return;

    const size_t bytes = vbDataRandom.size() * sizeof(vtxData);
    const size_t maxBytes = 200 * 3 * sizeof(vtxData); // "Tris" slider max
    if (!m_vbRandom[0]) {
        for (int i = 0; i < 3; ++i) {
            if (!createUploadBuffer(m_device.Get(), maxBytes,
                                    &m_vbRandomMapped[i], &m_vbRandom[i])) return;
        }
        m_vbRandomBytes = maxBytes;
    }

    UINT bb = m_frameIndex;
    memcpy(m_vbRandomMapped[bb], vbDataRandom.data(), bytes);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = m_vbRandom[bb]->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)bytes;
    vbv.StrideInBytes = (UINT)sizeof(vtxData);
    m_list->IASetVertexBuffers(0, 1, &vbv);
    m_list->DrawInstanced((UINT)vbDataRandom.size(), 1, 0, 0);
}

void RendererD3D12::bindImguiSrvHeap() {
    ID3D12DescriptorHeap* heaps[1] = { m_srvHeap.Get() };
    m_list->SetDescriptorHeaps(1, heaps);
}

void RendererD3D12::present() {
    UINT bb = m_frameIndex;
    D3D12_RESOURCE_BARRIER bar = {};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = m_backBuffers[bb].Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_list->ResourceBarrier(1, &bar);

    m_list->Close();
    ID3D12CommandList* lists[1] = { m_list.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    // Present without blocking on a vertical blank. Frame pacing to the display
    // refresh rate is done on the CPU (see main.cpp). Using Present(0,0) means
    // the flip-model swapchain never holds a blocking vsync present, so tearing
    // the device down at exit cannot trigger an NVIDIA driver TDR.
    m_swapChain->Present(0, 0);
    m_fenceValues[bb] += 1;
    m_queue->Signal(m_fences[bb].Get(), m_fenceValues[bb]);
}

bool RendererD3D12::resize(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    flush();
    for (auto& buf : m_backBuffers) buf.Reset();
    m_depth.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(3, (UINT)w, (UINT)h,
                                            DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        Log(LOG_ERROR) << "ResizeBuffers (D3D12) failed: 0x" << std::hex << (unsigned)hr;
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 3; ++i) {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])))) return false;
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvCpu);
        rtvCpu.ptr += m_rtvInc;
    }
    if (!createDepth(w, h)) return false;

    m_info.width = w;
    m_info.height = h;
    return true;
}

bool RendererD3D12::getImguiD3D12Handles(ImguiD3D12Handles& out) {
    out.device = m_device.Get();
    out.queue = m_queue.Get();
    out.commandList = m_list.Get();
    out.srvHeap = m_srvHeap.Get();
    out.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    out.numFrames = 3;
    return true;
}
