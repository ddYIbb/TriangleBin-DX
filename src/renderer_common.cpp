#include "renderer.h"

#include "renderer_dx11.h"
#include "renderer_dx12.h"

#include <cstdio>
#include <cstring>

Renderer::Renderer() : gen(rd()) {
    int num = 1;
    float stride = 2.f / (float)num;
    for (int i = 0; i < num; ++i) {
        float start = -1.f + stride * (float)i;
        distributions.emplace_back(start + stride / 2.f,
                                   start + stride / 2.f + 1.f);
    }
    nudge_distribution = std::uniform_real_distribution<float>(0., stride);
    distribution = std::uniform_real_distribution<float>(-1, 1);

    vbData = {
        { glm::vec4(-1,  1, 0, 1) },
        { glm::vec4( 1,  1, 0, 1) },
        { glm::vec4(-1, -1, 0, 1) },
        { glm::vec4( 1, -1, 0, 1) }
    };
    ibData = { 0, 1, 2, 1, 2, 3 };
}

void Renderer::ensureQuadIndexData() {
    size_t vtxcount = (size_t)triangle_count * 3;
    if (vtxcount != ibData.size()) {
        ibData.clear();
        for (size_t i = 0; i < vtxcount; ++i) {
            ibData.emplace_back(ibDb[i % ibDb.size()]);
        }
    }
}

void Renderer::ensureRandomTris(int count) {
    int vtxcount = count * 3;
    if ((int)vbDataRandom.size() != vtxcount) {
        vbDataRandom.clear();
        int vtxcount2add = vtxcount - (int)vbDataRandom.size();
        for (int i = 0; i < vtxcount2add; ++i) {
            int tricount = vtxcount2add / 3;
            int triidx = i / 3;
            float z = (float)(tricount - 1 - triidx) / (float)tricount * 2.f - 1.f;
            vtxData v {
                .pos = glm::vec4(distribution(gen), distribution(gen), z, 1.f)
            };
            vbDataRandom.push_back(v);
        }
        // GL NDC z in [-1, 1] -> D3D NDC z in [0, 1]
        for (auto& vert : vbDataRandom) {
            vert.pos.z = vert.pos.z * 0.5f + 0.5f;
        }
    }
}

std::unique_ptr<Renderer> createRenderer(Backend forced, HWND hwnd, int w, int h,
                                         int adapterIndex) {
    std::unique_ptr<Renderer> renderer;

    if (forced != Backend::D3D11) {
        auto r = std::make_unique<RendererD3D12>();
        if (r->init(hwnd, w, h, adapterIndex)) {
            renderer = std::move(r);
            Log(LOG_INFO) << "Initialized Direct3D 12 renderer";
        } else {
            Log(LOG_WARN) << "Direct3D 12 hardware initialization failed, falling back to Direct3D 11";
        }
    }

    if (!renderer && forced != Backend::D3D12) {
        auto r = std::make_unique<RendererD3D11>();
        if (r->init(hwnd, w, h, adapterIndex)) {
            renderer = std::move(r);
            Log(LOG_INFO) << "Initialized Direct3D 11 renderer";
        } else {
            Log(LOG_WARN) << "Direct3D 11 hardware initialization failed";
        }
    }

    if (!renderer) {
        std::string hwName, hwDx, hwDriver;
        std::string detail;
        if (probeHardwareGpu(hwName, hwDx, hwDriver)) {
            detail = " Detected GPU \"" + hwName + "\" supports only " + hwDx + ".";
        }
        Log(LOG_FATAL) << "No supported DirectX device found (requires DirectX 11 or newer "
                          "hardware with SM5.0)." << detail;
    }
    return renderer;
}

std::vector<AdapterInfo> enumerateAdapters() {
    using Microsoft::WRL::ComPtr;
    std::vector<AdapterInfo> out;

    ComPtr<IDXGIFactory1> factory1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) return out;
    ComPtr<IDXGIFactory4> factory4;
    factory1.As(&factory4);

    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };
    const UINT numLevels = (UINT)(sizeof(kLevels) / sizeof(kLevels[0]));

    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter1;
        HRESULT hr = factory4
            ? factory4->EnumAdapters1(i, &adapter1)
            : factory1->EnumAdapters(i, (IDXGIAdapter**)adapter1.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        DXGI_ADAPTER_DESC1 desc1 = {};
        bool haveDesc = SUCCEEDED(adapter1->GetDesc1(&desc1));
        if (!haveDesc) {
            ComPtr<IDXGIAdapter> base;
            adapter1.As(&base);
            DXGI_ADAPTER_DESC d0 = {};
            if (SUCCEEDED(base->GetDesc(&d0))) {
                for (int k = 0; k < 128; ++k) desc1.Description[k] = d0.Description[k];
                desc1.DedicatedVideoMemory = d0.DedicatedVideoMemory;
                haveDesc = true;
            }
        }
        if (haveDesc && (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (!haveDesc) continue;

        AdapterInfo info;
        info.index = (int)i;

        size_t len = 0;
        while (len < 128 && desc1.Description[len] != L'\0') ++len;
        std::wstring wname(desc1.Description, len);
        int req = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                                      nullptr, 0, nullptr, nullptr);
        info.name.resize(req);
        WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                            &info.name[0], req, nullptr, nullptr);

        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter1->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            formatDriverVersion((ULONGLONG)umd.QuadPart, info.driverVersion);
        } else {
            info.driverVersion = "unknown";
        }

        ComPtr<ID3D12Device> dev12;
        if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&dev12)))) {
            D3D12_FEATURE_DATA_FEATURE_LEVELS fld = {};
            fld.NumFeatureLevels = numLevels;
            fld.pFeatureLevelsRequested = kLevels;
            D3D_FEATURE_LEVEL mfl = D3D_FEATURE_LEVEL_11_0;
            if (SUCCEEDED(dev12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                                                     &fld, sizeof(fld)))) {
                mfl = fld.MaxSupportedFeatureLevel;
            }
            info.supportsD3D12 = true;
            info.maxFeatureLevel = mfl;
        } else {
            ComPtr<ID3D11Device> dev11;
            D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
            if (SUCCEEDED(D3D11CreateDevice(adapter1.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels, numLevels,
                                            D3D11_SDK_VERSION, &dev11, &fl, nullptr))) {
                info.maxFeatureLevel = fl;
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::string featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default: return "?";
    }
}

std::string dxVersionName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_12_2:
    case D3D_FEATURE_LEVEL_12_1:
    case D3D_FEATURE_LEVEL_12_0: return "DirectX 12";
    case D3D_FEATURE_LEVEL_11_1:
    case D3D_FEATURE_LEVEL_11_0: return "DirectX 11";
    case D3D_FEATURE_LEVEL_10_1: return "DirectX 10.1 (SM4.1)";
    case D3D_FEATURE_LEVEL_10_0: return "DirectX 10 (SM4.0)";
    case D3D_FEATURE_LEVEL_9_3:  return "DirectX 9 (FL 9_3)";
    case D3D_FEATURE_LEVEL_9_2:
    case D3D_FEATURE_LEVEL_9_1:  return "DirectX 9";
    default: return "unknown";
    }
}

bool probeHardwareGpu(std::string& name, std::string& dxName,
                      std::string& driverVersion) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory1> factory1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) return false;
    ComPtr<IDXGIFactory4> factory4;
    factory1.As(&factory4);

    static const D3D_FEATURE_LEVEL kProbeLevels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };
    const UINT numLevels = (UINT)(sizeof(kProbeLevels) / sizeof(kProbeLevels[0]));

    std::string bestName;
    std::string bestDriver;
    D3D_FEATURE_LEVEL bestFL = (D3D_FEATURE_LEVEL)0;
    SIZE_T bestVRAM = 0;
    bool found = false;
    std::string fallbackName;  // first hardware adapter, even if D3D11 probe fails

    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter1;
        HRESULT hr = factory4
            ? factory4->EnumAdapters1(i, &adapter1)
            : factory1->EnumAdapters(i, (IDXGIAdapter**)adapter1.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        DXGI_ADAPTER_DESC1 desc1 = {};
        bool software = false;
        if (SUCCEEDED(adapter1->GetDesc1(&desc1))) {
            software = (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        } else {
            ComPtr<IDXGIAdapter> baseAdapter;
            adapter1.As(&baseAdapter);
            DXGI_ADAPTER_DESC desc0 = {};
            if (SUCCEEDED(baseAdapter->GetDesc(&desc0))) {
                desc1.DedicatedVideoMemory = desc0.DedicatedVideoMemory;
            }
        }
        if (software) continue;

        size_t len = 0;
        while (len < 128 && desc1.Description[len] != L'\0') ++len;
        std::wstring wname(desc1.Description, len);
        int req = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string adapterName;
        adapterName.resize(req);
        WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), (int)wname.size(),
                            &adapterName[0], req, nullptr, nullptr);

        if (fallbackName.empty()) fallbackName = adapterName;

        ComPtr<ID3D11Device> dev;
        D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
        hr = D3D11CreateDevice(adapter1.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               kProbeLevels, numLevels, D3D11_SDK_VERSION,
                               &dev, &fl, nullptr);
        if (FAILED(hr)) continue;

        const bool better = !found || fl > bestFL ||
                            (fl == bestFL && desc1.DedicatedVideoMemory > bestVRAM);
        if (!better) continue;

        found = true;
        bestFL = fl;
        bestVRAM = desc1.DedicatedVideoMemory;
        bestName = adapterName;

        LARGE_INTEGER umdVersion = {};
        if (SUCCEEDED(adapter1->CheckInterfaceSupport(__uuidof(IDXGIDevice),
                                                      &umdVersion))) {
            formatDriverVersion((ULONGLONG)umdVersion.QuadPart, bestDriver);
        } else {
            bestDriver = "unknown";
        }
    }

    if (found) {
        name = bestName;
        dxName = dxVersionName(bestFL);
        driverVersion = bestDriver;
    } else if (!fallbackName.empty()) {
        // A hardware adapter exists but could not be probed through D3D11
        // (e.g. a DX9-era GPU on Windows 7). Report it as DX9 or lower.
        name = fallbackName;
        dxName = "DirectX 9 or lower";
        driverVersion = "unknown";
    } else {
        return false;
    }
    return true;
}

void formatDriverVersion(ULONGLONG v, std::string& out) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)((v >> 48) & 0xFFFF),
             (unsigned)((v >> 32) & 0xFFFF),
             (unsigned)((v >> 16) & 0xFFFF),
             (unsigned)(v & 0xFFFF));
    out = buf;
}

bool compileHLSL(const char* src, const char* entry, const char* target, std::vector<uint8_t>& blob) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> err;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(src, strlen(src), "TriangleBin", nullptr, nullptr,
                            entry, target, flags, 0, &shader, &err);
    if (FAILED(hr)) {
        Log(LOG_ERROR) << "Could not compile shader! Target: " << target
                       << ", HRESULT: 0x" << std::hex << (unsigned)hr;
        if (err) {
            Log(LOG_ERROR) << "Error log: " << (const char*)err->GetBufferPointer();
        }
        Log(LOG_ERROR) << "Shader source: " << src;
        return false;
    }
    const uint8_t* begin = (const uint8_t*)shader->GetBufferPointer();
    blob.assign(begin, begin + shader->GetBufferSize());
    return true;
}
