#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gfn {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Normalised source rectangle inside the captured texture, used to crop the
// stream out of the window's own chrome and letterboxing.
struct SourceRect {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

// Destination rectangle in target pixels.
struct DestRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    static DestRect Fill(uint32_t width, uint32_t height) {
        return DestRect{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
    }

    // Largest centred rectangle inside width x height that keeps the source
    // aspect ratio.
    static DestRect Fit(uint32_t width, uint32_t height, uint32_t source_width,
                        uint32_t source_height);
};

// One D3D11 device shared by capture, the neural round trip and presentation.
// The immediate context is not thread safe, so every caller goes through
// `lock()`; the class keeps that discipline in one place.
class GraphicsContext {
public:
    bool Initialize(std::string* error);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    IDXGIFactory2* factory() const { return factory_.Get(); }
    IDXGIAdapter1* adapter() const { return adapter_.Get(); }
    bool tearing_supported() const { return tearing_supported_; }
    std::string adapter_description() const { return adapter_description_; }

    std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(mutex_); }

    // Draws `source` into the `dest` rectangle of `target`, sampling the `rect`
    // region of the source and optionally swapping the red and blue channels.
    // Pixels of `target` outside `dest` are left untouched, which is what makes
    // the letterbox bars stay the cleared colour. The caller must already hold
    // `lock()`.
    void Blit(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
              const DestRect& dest, const SourceRect& rect, bool swap_rb);

private:
    bool CompileShaders(std::string* error);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    std::string adapter_description_;
    bool tearing_supported_ = false;
    std::mutex mutex_;
};

// A render target the neural input is rasterised into, plus the staging copy the
// CPU reads back. Sized once per resolution change.
class ReadbackTarget {
public:
    bool Resize(GraphicsContext* graphics, uint32_t width, uint32_t height, std::string* error);

    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Copies the render target into the staging texture and returns once the
    // GPU has finished, without holding the context lock while waiting.
    // `out` is resized to width * height * 4 bytes of RGBA8.
    bool Download(GraphicsContext* graphics, std::vector<uint8_t>* out, std::string* error);

private:
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> staging_;
    ComPtr<ID3D11Query> fence_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

// A CPU-writable texture the enhanced frame is uploaded into before drawing.
class UploadTexture {
public:
    bool Resize(GraphicsContext* graphics, uint32_t width, uint32_t height, std::string* error);
    bool Upload(GraphicsContext* graphics, const uint8_t* rgba, std::string* error);

    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

std::string HresultText(HRESULT hr);

}  // namespace gfn
