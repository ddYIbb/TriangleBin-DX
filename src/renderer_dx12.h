#ifndef RENDERER_DX12_H
#define RENDERER_DX12_H

#include "renderer.h"

class RendererD3D12 final : public Renderer {
public:
    RendererD3D12() = default;
    ~RendererD3D12() override;

    bool init(HWND hwnd, int w, int h, int adapterIndex) override;
    void beginFrame() override;
    void draw() override;
    void drawRandomTris(int count) override;
    void present() override;
    void bindImguiSrvHeap() override;
    bool resize(int w, int h) override;
    void setClearColor(float r, float g, float b, float a) override;
    void setFragCount(uint32_t v) override;
    Backend backend() const override { return Backend::D3D12; }
    const RendererInfo& info() const override { return m_info; }

    bool getImguiD3D12Handles(ImguiD3D12Handles& out) override;

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool createDepth(int w, int h);
    bool createRootSignatureAndPSO();
    bool createUploadResources();
    bool createCounter();
    void waitForFrame(UINT idx);
    void flush();
    static bool createUploadBuffer(ID3D12Device* dev, size_t size, void** mapped, ID3D12Resource** out);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandAllocator> m_alloc[3];
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap; // ImGui font SRVs
    ComPtr<ID3D12DescriptorHeap> m_uavHeap; // fragment counter UAV
    ComPtr<ID3D12Resource> m_backBuffers[3];
    ComPtr<ID3D12Resource> m_depth;
    ComPtr<ID3D12Fence> m_fences[3];
    UINT64 m_fenceValues[3] = {};
    HANDLE m_fenceEvents[3] = {};
    UINT m_rtvInc = 0;

    ComPtr<ID3D12Resource> m_vb;
    ComPtr<ID3D12Resource> m_ib;
    ComPtr<ID3D12Resource> m_cbVS;
    // One upload buffer per in-flight frame for the random triangles. They are
    // allocated once at the maximum size (200 tris) so that dragging the "Tris"
    // slider never recreates GPU resources, and each frame writes only into the
    // buffer belonging to that frame (no racing with in-flight GPU reads).
    ComPtr<ID3D12Resource> m_vbRandom[3];
    void* m_vbRandomMapped[3] = {};
    size_t m_vbRandomBytes = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_cbVSAddr = 0;

    ComPtr<ID3D12Resource> m_cbPS[3];
    ComPtr<ID3D12Resource> m_zeroSrc[3];
    void* m_cbPSMapped[3] = {};
    void* m_zeroMapped[3] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_cbPSAddr[3] = {};

    ComPtr<ID3D12Resource> m_counter;
    D3D12_GPU_DESCRIPTOR_HANDLE m_counterUavGpu = {};
    bool m_counterInitialized = false;

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    D3D_FEATURE_LEVEL m_fl = D3D_FEATURE_LEVEL_11_0;
    RendererInfo m_info;
    UINT m_frameIndex = 0;
};

#endif // RENDERER_DX12_H
