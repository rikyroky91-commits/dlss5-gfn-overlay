#pragma once

#include <functional>
#include <string>

#include "d3d.h"

namespace gfn {

// Borderless output window plus its swapchain. It shows either the enhanced
// frame or, in passthrough, the captured frame straight from the GPU, so the
// hotkey comparison is like for like.
class Presenter {
public:
    ~Presenter();

    struct Options {
        bool fullscreen = true;
        int monitor_index = -1;      // -1 = the monitor holding `anchor`
        HWND anchor = nullptr;       // window whose monitor we follow
        bool allow_tearing = true;
        std::wstring title = L"DLSS 5 overlay";
    };

    bool Create(GraphicsContext* graphics, const Options& options, std::string* error);
    void Destroy();

    HWND window() const { return window_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Draws `source` letterboxed into the swapchain and presents. `source_width`
    // and `source_height` describe the aspect to preserve.
    bool Present(ID3D11ShaderResourceView* source, uint32_t source_width,
                 uint32_t source_height, std::string* error);

    // Presents whatever the callback draws into the back buffer. Used for the
    // passthrough path, where the capture module owns the source texture.
    bool PresentWith(const std::function<void(ID3D11RenderTargetView*, uint32_t, uint32_t)>& draw,
                     std::string* error);

    // Pumps the thread's message queue. `observer` sees every message before it
    // is dispatched, which is how WM_HOTKEY reaches the caller: hotkeys are
    // thread messages with a null window and never reach a window procedure.
    // Returns false once the window has been closed.
    bool PumpMessages(const std::function<void(const MSG&)>& observer);

    void RequestClose() { close_requested_ = true; }

private:
    bool ResizeBuffers(std::string* error);
    bool AcquireBackBuffer(std::string* error);

    GraphicsContext* graphics_ = nullptr;
    HWND window_ = nullptr;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool allow_tearing_ = false;
    bool close_requested_ = false;
};

}  // namespace gfn
