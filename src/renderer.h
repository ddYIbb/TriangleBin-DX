#ifndef RENDERER_H
#define RENDERER_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <glm/common.hpp>
#include <glm/matrix.hpp>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "logger.h"

enum class Backend {
    Auto = 0,
    D3D12 = 1,
    D3D11 = 2
};

// Some stripped or older Windows SDKs do not define this constant even though
// the runtime supports it; keep the source portable.
#ifndef D3D_FEATURE_LEVEL_12_2
#define D3D_FEATURE_LEVEL_12_2 ((D3D_FEATURE_LEVEL)0xc200)
#endif

struct RendererInfo {
    std::string adapterName;
    std::string driverVersion;
    std::string apiName;
    int width = 0;
    int height = 0;
    int adapterIndex = -1;   // DXGI adapter index actually in use
};

// A hardware adapter available for rendering, as listed in the "Select GPU"
// window.
struct AdapterInfo {
    int index = -1;                                   // DXGI adapter index
    std::string name;
    std::string driverVersion;
    D3D_FEATURE_LEVEL maxFeatureLevel = (D3D_FEATURE_LEVEL)0;
    bool supportsD3D12 = false;
};

// Handles handed to the ImGui D3D12 backend by the app.
struct ImguiD3D12Handles {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    int numFrames = 3;
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual bool init(HWND hwnd, int w, int h, int adapterIndex) = 0;
    virtual void beginFrame() = 0;
    virtual void draw() = 0;
    virtual void drawRandomTris(int count) = 0;
    virtual void present() = 0;
    virtual void bindImguiSrvHeap() {}
    virtual bool resize(int w, int h) = 0;
    virtual void setClearColor(float r, float g, float b, float a) = 0;
    virtual void setFragCount(uint32_t v) = 0;
    virtual Backend backend() const = 0;
    virtual const RendererInfo& info() const = 0;

    virtual bool getImguiD3D11Handles(ID3D11Device** device, ID3D11DeviceContext** ctx) {
        (void)device; (void)ctx;
        return false;
    }
    virtual bool getImguiD3D12Handles(ImguiD3D12Handles& out) {
        (void)out;
        return false;
    }

    int width() const { return info().width; }
    int height() const { return info().height; }
    int triangleCount() const { return triangle_count; }
    uint32_t fragCount() const { return frag_count; }

protected:
    Renderer();

    int triangle_count = 2;
    uint32_t frag_count = 0;
    float clearColor[4] = {0.f, 0.f, 0.f, 1.f};

    struct vtxData {
        glm::vec4 pos;
        glm::vec4 color;
    };

    std::vector<vtxData> vbData;
    std::vector<uint16_t> ibDb {0, 1, 2, 1, 2, 3};
    std::vector<uint16_t> ibData;
    std::vector<vtxData> vbDataRandom;

    std::random_device rd;
    std::mt19937 gen;
    std::vector<std::uniform_real_distribution<float>> distributions;
    std::uniform_real_distribution<float> nudge_distribution;
    std::uniform_real_distribution<float> distribution;

    void ensureQuadIndexData();
    void ensureRandomTris(int count);
};

// Tries D3D12 first, falls back to D3D11 unless forced. Returns nullptr when
// no supported device exists (caller reports the error). adapterIndex >= 0
// forces a specific DXGI adapter; -1 keeps the automatic best-GPU selection.
std::unique_ptr<Renderer> createRenderer(Backend forced, HWND hwnd, int w, int h,
                                         int adapterIndex = -1);

// Enumerates the available hardware (non-software) DXGI adapters, in DXGI
// index order, with their driver version and best supported feature level.
std::vector<AdapterInfo> enumerateAdapters();

std::string featureLevelName(D3D_FEATURE_LEVEL fl);
std::string dxVersionName(D3D_FEATURE_LEVEL fl);
// Finds the real hardware GPU (skipping software adapters) and reports its
// model name, highest supported DirectX version and driver version. Returns
// false when no hardware GPU exists (e.g. virtual machines with only WARP).
bool probeHardwareGpu(std::string& name, std::string& dxName,
                      std::string& driverVersion);
void formatDriverVersion(ULONGLONG v, std::string& out);
bool compileHLSL(const char* src, const char* entry, const char* target, std::vector<uint8_t>& blob);

#endif // RENDERER_H
