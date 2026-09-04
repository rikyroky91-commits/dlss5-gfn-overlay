#include "presenter.h"

#include <algorithm>
#include <vector>

#include "log.h"

namespace gfn {
namespace {

constexpr wchar_t kWindowClass[] = L"Dlss5GfnOverlayWindow";

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* presenter = reinterpret_cast<Presenter*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_CLOSE:
            if (presenter) presenter->RequestClose();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && presenter) presenter->RequestClose();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct MonitorSearch {
    int wanted = 0;
    int seen = 0;
    RECT bounds{};
    bool found = false;
};

BOOL CALLBACK MonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* search = reinterpret_cast<MonitorSearch*>(parameter);
    if (search->seen == search->wanted) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            search->bounds = info.rcMonitor;
            search->found = true;
        }
        return FALSE;
    }
    ++search->seen;
    return TRUE;
}

RECT ResolveBounds(const Presenter::Options& options) {
    if (options.monitor_index >= 0) {
        MonitorSearch search{options.monitor_index, 0, {}, false};
        EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&search));
        if (search.found) return search.bounds;
        GFN_WARN("monitor %d does not exist; falling back to the source window's monitor",
                 options.monitor_index);
    }

    HMONITOR monitor = options.anchor
                           ? MonitorFromWindow(options.anchor, MONITOR_DEFAULTTONEAREST)
                           : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) return info.rcMonitor;
    return RECT{0, 0, 1920, 1080};
}

}  // namespace

Presenter::~Presenter() { Destroy(); }

bool Presenter::Create(GraphicsContext* graphics, const Options& options, std::string* error) {
    graphics_ = graphics;
    allow_tearing_ = options.allow_tearing && graphics->tearing_supported();

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClass;
    // A duplicate registration is fine when the presenter is recreated.
    RegisterClassExW(&window_class);

    const RECT bounds = ResolveBounds(options);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;

    const DWORD style = options.fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    window_ = CreateWindowExW(0, kWindowClass, options.title.c_str(), style, bounds.left,
                              bounds.top, width, height, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!window_) {
        *error = "CreateWindowEx failed for the output window";
        return false;
    }
    SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);

    RECT client{};
    GetClientRect(window_, &client);
    width_ = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
    height_ = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (allow_tearing_) desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    HRESULT hr = graphics->factory()->CreateSwapChainForHwnd(
        graphics->device(), window_, &desc, nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        *error = "CreateSwapChainForHwnd failed: " + HresultText(hr);
        return false;
    }

    // Alt+Enter fullscreen transitions would fight the borderless window.
    graphics->factory()->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);

    return AcquireBackBuffer(error);
}

void Presenter::Destroy() {
    back_buffer_rtv_.Reset();
    swapchain_.Reset();
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool Presenter::AcquireBackBuffer(std::string* error) {
    back_buffer_rtv_.Reset();
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) {
        *error = "swapchain GetBuffer failed: " + HresultText(hr);
        return false;
    }
    hr = graphics_->device()->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                                     &back_buffer_rtv_);
    if (FAILED(hr)) {
        *error = "CreateRenderTargetView(back buffer) failed: " + HresultText(hr);
        return false;
    }
    return true;
}

bool Presenter::ResizeBuffers(std::string* error) {
    RECT client{};
    GetClientRect(window_, &client);
    const uint32_t width = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
    const uint32_t height = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
    if (width == width_ && height == height_) return true;

    auto guard = graphics_->lock();
    back_buffer_rtv_.Reset();
    graphics_->context()->ClearState();
    const UINT flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    const HRESULT hr = swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) {
        *error = "ResizeBuffers failed: " + HresultText(hr);
        return false;
    }
    width_ = width;
    height_ = height;
    return AcquireBackBuffer(error);
}

bool Presenter::PresentWith(
    const std::function<void(ID3D11RenderTargetView*, uint32_t, uint32_t)>& draw,
    std::string* error) {
    if (!ResizeBuffers(error)) return false;

    {
        auto guard = graphics_->lock();
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        graphics_->context()->ClearRenderTargetView(back_buffer_rtv_.Get(), black);
    }

    draw(back_buffer_rtv_.Get(), width_, height_);

    auto guard = graphics_->lock();
    const UINT flags = allow_tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT hr = swapchain_->Present(0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        *error = "the graphics device was removed: " + HresultText(hr);
        return false;
    }
    if (FAILED(hr)) {
        *error = "Present failed: " + HresultText(hr);
        return false;
    }
    return true;
}

bool Presenter::Present(ID3D11ShaderResourceView* source, uint32_t source_width,
                        uint32_t source_height, std::string* error) {
    return PresentWith(
        [&](ID3D11RenderTargetView* target, uint32_t width, uint32_t height) {
            const DestRect dest = DestRect::Fit(width, height, source_width, source_height);
            auto guard = graphics_->lock();
            graphics_->Blit(source, target, dest, SourceRect{}, /*swap_rb=*/false);
        },
        error);
}

bool Presenter::PumpMessages(const std::function<void(const MSG&)>& observer) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        if (observer) observer(message);
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !close_requested_;
}

}  // namespace gfn
