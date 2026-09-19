#ifndef RENDERER_DX11_H
#define RENDERER_DX11_H

#include "renderer.h"

class RendererD3D11 final : public Renderer {
public:
    explicit RendererD3D11();

    bool init(HWND hwnd, int w, int h, int adapterIndex) override;
    void beginFrame() override {}
    void draw() override;
    void drawRandomTris(int count) override;
    void present() override;
    bool resize(int w, int h) override;
    void setClearColor(float r, float g, float b, float a) override;
    void setFragCount(uint32_t v) override;
    Backend backend() const override { return Backend::D3D11; }
    const RendererInfo& info() const override { return m_info; }

    bool getImguiD3D11Handles(ID3D11Device** device, ID3D11DeviceContext** ctx) override {
        *device = m_device.Get();
        *ctx = m_ctx.Get();
        return true;
    }

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool createSwapChain(HWND hwnd, int w, int h);
    bool createRenderTargets(int w, int h);
    bool createPipeline();
    bool createBuffers();

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<IDXGISwapChain3> m_swapChain3;
    DXGI_ADAPTER_DESC m_adapterDesc = {};

    ComPtr<ID3D11RenderTargetView> m_rtv[3];
    ComPtr<ID3D11Texture2D> m_depthTex;
    ComPtr<ID3D11DepthStencilView> m_dsv;

    ComPtr<ID3D11Buffer> m_vb;
    ComPtr<ID3D11Buffer> m_ib;
    ComPtr<ID3D11Buffer> m_vbRandom;
    ComPtr<ID3D11Buffer> m_cbVS;
    ComPtr<ID3D11Buffer> m_cbPS;
    ComPtr<ID3D11Buffer> m_counter;
    ComPtr<ID3D11UnorderedAccessView> m_counterUAV;

    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11InputLayout> m_il;
    ComPtr<ID3D11BlendState> m_blend;
    ComPtr<ID3D11DepthStencilState> m_dsState;
    ComPtr<ID3D11RasterizerState> m_rs;

    D3D_FEATURE_LEVEL m_fl = D3D_FEATURE_LEVEL_11_0;
    RendererInfo m_info;
    size_t m_vbRandomBytes = 0;
    bool m_quadUploaded = false;
    UINT m_bufferCount = 3;
    bool m_flipModel = true;
};

#endif // RENDERER_DX11_H
