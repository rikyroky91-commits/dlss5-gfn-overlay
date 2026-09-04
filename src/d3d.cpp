#include "d3d.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

#include "log.h"

namespace gfn {
namespace {

const char kShaderSource[] = R"(
cbuffer Params : register(b0)
{
    float4 uv_scale_offset;
    uint   swap_rb;
    uint3  padding;
};

Texture2D<float4> source_texture : register(t0);
SamplerState source_sampler : register(s0);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertex_id : SV_VertexID)
{
    VSOut output;
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float4 PSMain(VSOut input) : SV_Target
{
    float2 uv = input.uv * uv_scale_offset.xy + uv_scale_offset.zw;
    float4 colour = source_texture.Sample(source_sampler, uv);
    if (swap_rb != 0)
    {
        colour.rgb = colour.bgr;
    }
    return float4(colour.rgb, 1.0);
}
)";

struct BlitConstants {
    float uv_scale_offset[4];
    uint32_t swap_rb;
    uint32_t padding[3];
};
static_assert(sizeof(BlitConstants) == 32, "constant buffer must stay 16-byte aligned");

}  // namespace

std::string HresultText(HRESULT hr) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

bool GraphicsContext::Initialize(std::string* error) {
    UINT factory_flags = 0;
#if defined(_DEBUG)
    factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        *error = "CreateDXGIFactory2 failed: " + HresultText(hr);
        return false;
    }
    factory_ = factory;

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory_.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
                                                    sizeof(allow)))) {
            tearing_supported_ = allow == TRUE;
        }
    }

    // Prefer the highest-performance adapter: on a laptop the neural runtime
    // must land on the discrete RTX part, not the integrated one.
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory_.As(&factory6))) {
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             IID_PPV_ARGS(&adapter_));
    }
    if (!adapter_) {
        factory_->EnumAdapters1(0, &adapter_);
    }
    if (adapter_) {
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter_->GetDesc1(&description))) {
            const std::wstring wide(description.Description);
            adapter_description_.assign(wide.begin(), wide.end());
        }
    }

    UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL achieved{};
    hr = D3D11CreateDevice(adapter_.Get(),
                           adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           nullptr, device_flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                           &device_, &achieved, &context_);
    if (FAILED(hr)) {
        *error = "D3D11CreateDevice failed: " + HresultText(hr);
        return false;
    }

    // One frame of queued work keeps present latency close to the capture rate.
    ComPtr<IDXGIDevice1> dxgi_device;
    if (SUCCEEDED(device_.As(&dxgi_device))) {
        dxgi_device->SetMaximumFrameLatency(1);
    }

    return CompileShaders(error);
}

bool GraphicsContext::CompileShaders(std::string* error) {
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>* blob) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "blit.hlsl",
                                      nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob->GetAddressOf(),
                                      &errors);
        if (FAILED(hr)) {
            *error = std::string("compiling ") + entry + " failed: " + HresultText(hr);
            if (errors) {
                *error += ": ";
                error->append(static_cast<const char*>(errors->GetBufferPointer()),
                              errors->GetBufferSize());
            }
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vertex_blob;
    ComPtr<ID3DBlob> pixel_blob;
    if (!compile("VSMain", "vs_5_0", &vertex_blob)) return false;
    if (!compile("PSMain", "ps_5_0", &pixel_blob)) return false;

    HRESULT hr = device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                             vertex_blob->GetBufferSize(), nullptr,
                                             &vertex_shader_);
    if (FAILED(hr)) {
        *error = "CreateVertexShader failed: " + HresultText(hr);
        return false;
    }
    hr = device_->CreatePixelShader(pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
                                    nullptr, &pixel_shader_);
    if (FAILED(hr)) {
        *error = "CreatePixelShader failed: " + HresultText(hr);
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler, &sampler_);
    if (FAILED(hr)) {
        *error = "CreateSamplerState failed: " + HresultText(hr);
        return false;
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(BlitConstants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&buffer, nullptr, &constants_);
    if (FAILED(hr)) {
        *error = "CreateBuffer(constants) failed: " + HresultText(hr);
        return false;
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&blend, &blend_);
    if (FAILED(hr)) {
        *error = "CreateBlendState failed: " + HresultText(hr);
        return false;
    }

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    hr = device_->CreateRasterizerState(&raster, &raster_);
    if (FAILED(hr)) {
        *error = "CreateRasterizerState failed: " + HresultText(hr);
        return false;
    }

    return true;
}

DestRect DestRect::Fit(uint32_t width, uint32_t height, uint32_t source_width,
                       uint32_t source_height) {
    if (source_width == 0 || source_height == 0 || width == 0 || height == 0) {
        return Fill(width, height);
    }
    const double scale = std::min(static_cast<double>(width) / source_width,
                                  static_cast<double>(height) / source_height);
    const float fitted_width = static_cast<float>(source_width * scale);
    const float fitted_height = static_cast<float>(source_height * scale);
    return DestRect{(static_cast<float>(width) - fitted_width) * 0.5f,
                    (static_cast<float>(height) - fitted_height) * 0.5f, fitted_width,
                    fitted_height};
}

void GraphicsContext::Blit(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
                           const DestRect& dest, const SourceRect& rect, bool swap_rb) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        BlitConstants values{};
        values.uv_scale_offset[0] = rect.u1 - rect.u0;
        values.uv_scale_offset[1] = rect.v1 - rect.v0;
        values.uv_scale_offset[2] = rect.u0;
        values.uv_scale_offset[3] = rect.v0;
        values.swap_rb = swap_rb ? 1u : 0u;
        std::memcpy(mapped.pData, &values, sizeof(values));
        context_->Unmap(constants_.Get(), 0);
    }

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = dest.x;
    viewport.TopLeftY = dest.y;
    viewport.Width = dest.width;
    viewport.Height = dest.height;
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* targets[] = {target};
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(raster_.Get());
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetBlendState(blend_.Get(), blend_factor, 0xFFFFFFFF);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    ID3D11Buffer* buffers[] = {constants_.Get()};
    context_->PSSetConstantBuffers(0, 1, buffers);
    ID3D11ShaderResourceView* views[] = {source};
    context_->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    // Leave no view bound, so the next CopyResource on the same texture is legal.
    ID3D11ShaderResourceView* none[] = {nullptr};
    context_->PSSetShaderResources(0, 1, none);
    ID3D11RenderTargetView* no_target[] = {nullptr};
    context_->OMSetRenderTargets(1, no_target, nullptr);
}

bool ReadbackTarget::Resize(GraphicsContext* graphics, uint32_t width, uint32_t height,
                            std::string* error) {
    if (width == width_ && height == height_ && texture_) return true;

    texture_.Reset();
    rtv_.Reset();
    staging_.Reset();
    fence_.Reset();
    width_ = 0;
    height_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = graphics->device()->CreateTexture2D(&desc, nullptr, &texture_);
    if (FAILED(hr)) {
        *error = "CreateTexture2D(neural input) failed: " + HresultText(hr);
        return false;
    }
    hr = graphics->device()->CreateRenderTargetView(texture_.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
        *error = "CreateRenderTargetView failed: " + HresultText(hr);
        return false;
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = graphics->device()->CreateTexture2D(&desc, nullptr, &staging_);
    if (FAILED(hr)) {
        *error = "CreateTexture2D(staging) failed: " + HresultText(hr);
        return false;
    }

    D3D11_QUERY_DESC query{};
    query.Query = D3D11_QUERY_EVENT;
    hr = graphics->device()->CreateQuery(&query, &fence_);
    if (FAILED(hr)) {
        *error = "CreateQuery failed: " + HresultText(hr);
        return false;
    }

    width_ = width;
    height_ = height;
    return true;
}

bool ReadbackTarget::Download(GraphicsContext* graphics, std::vector<uint8_t>* out,
                              std::string* error) {
    if (!staging_) {
        *error = "readback target is not sized";
        return false;
    }

    {
        auto guard = graphics->lock();
        graphics->context()->CopyResource(staging_.Get(), texture_.Get());
        graphics->context()->End(fence_.Get());
        graphics->context()->Flush();
    }

    // Poll outside the lock: blocking on Map while holding it would stall the
    // present thread for the whole neural round trip.
    for (int spin = 0;; ++spin) {
        BOOL done = FALSE;
        HRESULT hr;
        {
            auto guard = graphics->lock();
            hr = graphics->context()->GetData(fence_.Get(), &done, sizeof(done),
                                              D3D11_ASYNC_GETDATA_DONOTFLUSH);
        }
        if (FAILED(hr)) {
            *error = "GetData(fence) failed: " + HresultText(hr);
            return false;
        }
        if (hr == S_OK && done) break;
        if (spin < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        if (spin > 40000) {
            *error = "the GPU did not finish the capture copy in time";
            return false;
        }
    }

    auto guard = graphics->lock();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = graphics->context()->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        *error = "Map(staging) failed: " + HresultText(hr);
        return false;
    }
    out->resize(static_cast<size_t>(width_) * height_ * 4);
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    const size_t row_bytes = static_cast<size_t>(width_) * 4;
    if (mapped.RowPitch == row_bytes) {
        std::memcpy(out->data(), source, row_bytes * height_);
    } else {
        for (uint32_t y = 0; y < height_; ++y) {
            std::memcpy(out->data() + static_cast<size_t>(y) * row_bytes,
                        source + static_cast<size_t>(y) * mapped.RowPitch, row_bytes);
        }
    }
    graphics->context()->Unmap(staging_.Get(), 0);
    return true;
}

bool UploadTexture::Resize(GraphicsContext* graphics, uint32_t width, uint32_t height,
                           std::string* error) {
    if (width == width_ && height == height_ && texture_) return true;

    texture_.Reset();
    srv_.Reset();
    width_ = 0;
    height_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = graphics->device()->CreateTexture2D(&desc, nullptr, &texture_);
    if (FAILED(hr)) {
        *error = "CreateTexture2D(upload) failed: " + HresultText(hr);
        return false;
    }
    hr = graphics->device()->CreateShaderResourceView(texture_.Get(), nullptr, &srv_);
    if (FAILED(hr)) {
        *error = "CreateShaderResourceView(upload) failed: " + HresultText(hr);
        return false;
    }

    width_ = width;
    height_ = height;
    return true;
}

bool UploadTexture::Upload(GraphicsContext* graphics, const uint8_t* rgba, std::string* error) {
    auto guard = graphics->lock();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr =
        graphics->context()->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        *error = "Map(upload) failed: " + HresultText(hr);
        return false;
    }
    const size_t row_bytes = static_cast<size_t>(width_) * 4;
    auto* destination = static_cast<uint8_t*>(mapped.pData);
    if (mapped.RowPitch == row_bytes) {
        std::memcpy(destination, rgba, row_bytes * height_);
    } else {
        for (uint32_t y = 0; y < height_; ++y) {
            std::memcpy(destination + static_cast<size_t>(y) * mapped.RowPitch,
                        rgba + static_cast<size_t>(y) * row_bytes, row_bytes);
        }
    }
    graphics->context()->Unmap(texture_.Get(), 0);
    return true;
}

}  // namespace gfn
