#include "capture.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <dwmapi.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <vector>

#include "log.h"

namespace winrt_capture = winrt::Windows::Graphics::Capture;
namespace winrt_directx = winrt::Windows::Graphics::DirectX;
namespace winrt_d3d11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace gfn {

struct WindowCapture::Impl {
    winrt_d3d11::IDirect3DDevice device{nullptr};
    winrt_capture::GraphicsCaptureItem item{nullptr};
    winrt_capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt_capture::GraphicsCaptureSession session{nullptr};
    winrt::event_token frame_token{};
    winrt::event_token closed_token{};
};

namespace {

std::wstring LowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

struct SearchState {
    std::wstring needle;
    HWND match = nullptr;
};

BOOL CALLBACK EnumProc(HWND window, LPARAM parameter) {
    auto* state = reinterpret_cast<SearchState*>(parameter);
    if (!IsWindowVisible(window)) return TRUE;
    if (GetWindow(window, GW_OWNER) != nullptr) return TRUE;

    // Cloaked windows (UWP suspended, virtual desktops) look visible but cannot
    // be captured usefully.
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked) {
        return TRUE;
    }

    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    title.resize(static_cast<size_t>(std::max(copied, 0)));

    if (LowerCopy(title).find(state->needle) != std::wstring::npos) {
        state->match = window;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

bool IsCaptureSupported() {
    try {
        return winrt_capture::GraphicsCaptureSession::IsSupported();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

HWND FindWindowByTitleSubstring(const std::wstring& needle) {
    SearchState state{LowerCopy(needle), nullptr};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&state));
    return state.match;
}

WindowCapture::~WindowCapture() { Stop(); }

bool WindowCapture::Start(GraphicsContext* graphics, HWND window, bool capture_cursor,
                          std::string* error) {
    Stop();
    graphics_ = graphics;
    closed_.store(false, std::memory_order_release);

    if (!IsCaptureSupported()) {
        *error = "Windows Graphics Capture is not available; Windows 10 1903 or later is required";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    try {
        ComPtr<IDXGIDevice> dxgi_device;
        HRESULT hr = graphics->device()->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (FAILED(hr)) {
            *error = "the D3D11 device does not expose IDXGIDevice: " + HresultText(hr);
            return false;
        }
        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put());
        if (FAILED(hr)) {
            *error = "CreateDirect3D11DeviceFromDXGIDevice failed: " + HresultText(hr);
            return false;
        }
        impl->device = inspectable.as<winrt_d3d11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<winrt_capture::GraphicsCaptureItem,
                                                     ::IGraphicsCaptureItemInterop>();
        winrt_capture::GraphicsCaptureItem item{nullptr};
        hr = interop->CreateForWindow(
            window, winrt::guid_of<winrt_capture::GraphicsCaptureItem>(),
            winrt::put_abi(item));
        if (FAILED(hr)) {
            *error = "this window cannot be captured (CreateForWindow: " + HresultText(hr) + ")";
            return false;
        }
        impl->item = item;

        const auto size = impl->item.Size();
        if (size.Width <= 0 || size.Height <= 0) {
            *error = "the target window reports an empty size";
            return false;
        }

        impl->pool = winrt_capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl->device, winrt_directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        impl->session = impl->pool.CreateCaptureSession(impl->item);

        try {
            impl->session.IsCursorCaptureEnabled(capture_cursor);
        } catch (const winrt::hresult_error&) {
            GFN_DEBUG("cursor capture toggle unavailable on this build of Windows");
        }
        try {
            // Windows 11 only. Without it the capture draws a yellow border
            // around the source window.
            impl->session.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) {
            GFN_DEBUG("capture border cannot be disabled on this build of Windows");
        }

        impl->frame_token = impl->pool.FrameArrived(
            [this](const winrt_capture::Direct3D11CaptureFramePool& pool,
                        const winrt::Windows::Foundation::IInspectable&) {
                auto frame = pool.TryGetNextFrame();
                if (!frame) return;
                auto access = frame.Surface().as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> surface;
                if (FAILED(access->GetInterface(IID_PPV_ARGS(&surface)))) return;

                const auto content = frame.ContentSize();
                if (content.Width > 0 && content.Height > 0) {
                    OnFrame(surface.Get(), 0, 0, static_cast<uint32_t>(content.Width),
                            static_cast<uint32_t>(content.Height));
                }
            });

        impl->closed_token = impl->item.Closed(
            [this](const winrt_capture::GraphicsCaptureItem&,
                   const winrt::Windows::Foundation::IInspectable&) {
                closed_.store(true, std::memory_order_release);
            });

        impl->session.StartCapture();
    } catch (const winrt::hresult_error& e) {
        *error = "starting the capture session failed: " + HresultText(e.code()) + " " +
                 winrt::to_string(e.message());
        return false;
    }

    impl_ = impl.release();
    return true;
}

void WindowCapture::Stop() {
    if (!impl_) return;
    try {
        if (impl_->pool && impl_->frame_token) impl_->pool.FrameArrived(impl_->frame_token);
        if (impl_->item && impl_->closed_token) impl_->item.Closed(impl_->closed_token);
        if (impl_->session) impl_->session.Close();
        if (impl_->pool) impl_->pool.Close();
    } catch (const winrt::hresult_error&) {
        // Closing a session whose window already vanished throws; nothing to do.
    }
    delete impl_;
    impl_ = nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    latest_.Reset();
    latest_srv_.Reset();
    content_width_ = 0;
    content_height_ = 0;
    has_new_frame_ = false;
}

void WindowCapture::OnFrame(ID3D11Texture2D* surface, uint32_t source_x, uint32_t source_y,
                            uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!latest_ || content_width_ != width || content_height_ != height) {
        latest_.Reset();
        latest_srv_.Reset();

        D3D11_TEXTURE2D_DESC desc{};
        surface->GetDesc(&desc);
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        if (FAILED(graphics_->device()->CreateTexture2D(&desc, nullptr, &latest_))) {
            GFN_WARN("could not allocate the capture surface at %ux%u", width, height);
            return;
        }
        if (FAILED(graphics_->device()->CreateShaderResourceView(latest_.Get(), nullptr,
                                                                 &latest_srv_))) {
            latest_.Reset();
            GFN_WARN("could not create the capture shader resource view");
            return;
        }
        content_width_ = width;
        content_height_ = height;
    }

    // The frame pool surface can be larger than the content; copy only the part
    // that carries pixels.
    D3D11_BOX box{};
    box.left = source_x;
    box.top = source_y;
    box.right = source_x + width;
    box.bottom = source_y + height;
    box.back = 1;

    auto guard = graphics_->lock();
    graphics_->context()->CopySubresourceRegion(latest_.Get(), 0, 0, 0, 0, surface, 0, &box);
    has_new_frame_ = true;
    frames_captured_.fetch_add(1, std::memory_order_relaxed);
}

void WindowCapture::ContentSize(uint32_t* width, uint32_t* height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    *width = content_width_;
    *height = content_height_;
}

bool WindowCapture::BlitLatest(ID3D11RenderTargetView* target, const DestRect& dest,
                               const SourceRect& rect, bool consume) {
    ComPtr<ID3D11ShaderResourceView> source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_srv_) return false;
        if (consume && !has_new_frame_) return false;
        source = latest_srv_;
        if (consume) has_new_frame_ = false;
    }

    auto guard = graphics_->lock();
    graphics_->Blit(source.Get(), target, dest, rect, /*swap_rb=*/true);
    return true;
}

}  // namespace gfn
