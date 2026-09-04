// Desktop Duplication capture backend.
//
// Windows Graphics Capture is the nicer API - it captures one window, follows
// it across monitors, and never sees anything else on screen. It also needs
// C++/WinRT, which only the MSVC toolchain has. This backend uses DXGI Desktop
// Duplication instead: it duplicates the whole monitor the target window sits
// on and crops to the window's client area, using headers that every Windows
// toolchain ships.
//
// Duplicating the monitor means the overlay would film itself, so the presenter
// marks its own window WDA_EXCLUDEFROMCAPTURE and disappears from the
// duplication. See Presenter::Create.

#include "capture.h"

#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <memory>
#include <thread>
#include <vector>

#include "log.h"

namespace gfn {

struct WindowCapture::Impl {
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<IDXGIOutput1> output;
    RECT output_bounds{};
    std::thread thread;
    std::atomic<bool> stop{false};
    HWND window = nullptr;
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

// Client area of `window` in desktop coordinates. The client area excludes the
// title bar and the invisible resize border, so it is what actually carries the
// stream.
bool ClientRectOnDesktop(HWND window, RECT* rect) {
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    POINT origin{0, 0};
    if (!ClientToScreen(window, &origin)) return false;
    rect->left = origin.x;
    rect->top = origin.y;
    rect->right = origin.x + (client.right - client.left);
    rect->bottom = origin.y + (client.bottom - client.top);
    return rect->right > rect->left && rect->bottom > rect->top;
}

// Finds the output that shows `window` and the adapter it belongs to.
bool FindOutputForWindow(IDXGIAdapter1* adapter, HWND window, ComPtr<IDXGIOutput1>* output,
                         RECT* bounds, std::string* error) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> candidate;
        const HRESULT hr = adapter->EnumOutputs(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) {
            *error = "EnumOutputs failed: " + HresultText(hr);
            return false;
        }
        DXGI_OUTPUT_DESC description{};
        if (FAILED(candidate->GetDesc(&description))) continue;
        if (description.Monitor != monitor) continue;

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(candidate.As(&output1))) {
            *error = "this display does not support Desktop Duplication";
            return false;
        }
        *output = output1;
        *bounds = description.DesktopCoordinates;
        if (description.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
            description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            *error = "the display is rotated, which this capture backend does not handle";
            return false;
        }
        return true;
    }
    *error =
        "the window is on a display this graphics adapter does not drive; on a hybrid-GPU "
        "laptop, force the application onto the discrete GPU";
    return false;
}

std::string DuplicationErrorText(HRESULT hr) {
    if (hr == E_ACCESSDENIED) {
        return "Desktop Duplication was denied. A full-screen exclusive game, a secure "
               "desktop (UAC prompt, lock screen), or another duplication client is holding "
               "the display.";
    }
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        return "Desktop Duplication is unsupported on this display or driver.";
    }
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
        return "Every Desktop Duplication slot on this display is already taken.";
    }
    return "DuplicateOutput failed: " + HresultText(hr);
}

}  // namespace

bool IsCaptureSupported() {
    // Desktop Duplication landed in Windows 8; anything that runs D3D11.1 has it.
    return true;
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

    if (capture_cursor) {
        // Desktop Duplication hands the pointer over as a separate shape rather
        // than compositing it, so honouring this would mean drawing the cursor
        // by hand. For a stream overlay that is not worth the code.
        GFN_WARN("capture_cursor is ignored by the Desktop Duplication backend");
    }

    auto impl = std::make_unique<Impl>();
    impl->window = window;

    if (!FindOutputForWindow(graphics->adapter(), window, &impl->output, &impl->output_bounds,
                             error)) {
        return false;
    }

    const HRESULT hr = impl->output->DuplicateOutput(graphics->device(), &impl->duplication);
    if (FAILED(hr)) {
        *error = DuplicationErrorText(hr);
        return false;
    }

    GFN_INFO("duplicating the display at %ld,%ld %ldx%ld", impl->output_bounds.left,
             impl->output_bounds.top, impl->output_bounds.right - impl->output_bounds.left,
             impl->output_bounds.bottom - impl->output_bounds.top);

    Impl* raw = impl.get();
    impl->thread = std::thread([this, raw] {
        while (!raw->stop.load(std::memory_order_acquire)) {
            if (!IsWindow(raw->window)) {
                closed_.store(true, std::memory_order_release);
                return;
            }

            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            const HRESULT acquire = raw->duplication->AcquireNextFrame(100, &info, &resource);
            if (acquire == DXGI_ERROR_WAIT_TIMEOUT) continue;  // nothing changed on screen
            if (acquire == DXGI_ERROR_ACCESS_LOST) {
                // A mode switch, a full-screen transition or a UAC prompt drops
                // the duplication. Rebuilding it is expected, not an error.
                GFN_INFO("the duplication was lost; rebuilding it");
                raw->duplication.Reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (FAILED(raw->output->DuplicateOutput(graphics_->device(),
                                                        &raw->duplication))) {
                    continue;
                }
                continue;
            }
            if (FAILED(acquire)) {
                GFN_WARN("AcquireNextFrame failed: %s", HresultText(acquire).c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // LastPresentTime stays zero when only the mouse pointer moved.
            // Those wake-ups carry no new desktop pixels.
            if (info.LastPresentTime.QuadPart != 0) {
                ComPtr<ID3D11Texture2D> desktop;
                if (SUCCEEDED(resource.As(&desktop))) {
                    RECT client{};
                    if (ClientRectOnDesktop(raw->window, &client)) {
                        // Desktop coordinates to output-local, clamped so a
                        // window hanging off the edge cannot read out of bounds.
                        const LONG left = std::max<LONG>(0, client.left - raw->output_bounds.left);
                        const LONG top = std::max<LONG>(0, client.top - raw->output_bounds.top);
                        const LONG right = std::min<LONG>(
                            raw->output_bounds.right - raw->output_bounds.left,
                            client.right - raw->output_bounds.left);
                        const LONG bottom = std::min<LONG>(
                            raw->output_bounds.bottom - raw->output_bounds.top,
                            client.bottom - raw->output_bounds.top);
                        if (right - left >= 64 && bottom - top >= 64) {
                            OnFrame(desktop.Get(), static_cast<uint32_t>(left),
                                    static_cast<uint32_t>(top),
                                    static_cast<uint32_t>(right - left),
                                    static_cast<uint32_t>(bottom - top));
                        }
                    }
                }
            }

            raw->duplication->ReleaseFrame();
        }
    });

    impl_ = impl.release();
    return true;
}

void WindowCapture::Stop() {
    if (impl_) {
        impl_->stop.store(true, std::memory_order_release);
        if (impl_->thread.joinable()) impl_->thread.join();
        delete impl_;
        impl_ = nullptr;
    }

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
