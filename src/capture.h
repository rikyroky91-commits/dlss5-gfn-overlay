#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "d3d.h"

namespace gfn {

// Captures a single window with Windows Graphics Capture and keeps only the
// most recent frame. Dropping older frames is deliberate: a queue would trade
// latency for smoothness, and latency is the whole point on a cloud stream.
class WindowCapture {
public:
    ~WindowCapture();

    bool Start(GraphicsContext* graphics, HWND window, bool capture_cursor, std::string* error);
    void Stop();

    // Draws the newest captured frame into `dest` on `target`, sampling `rect`
    // and converting BGRA to RGBA. Returns false when no new frame has arrived
    // since the last call. `consume` false peeks without clearing the new-frame
    // flag, which the passthrough path needs so it can redraw the same frame.
    bool BlitLatest(ID3D11RenderTargetView* target, const DestRect& dest,
                    const SourceRect& rect, bool consume);

    // Size of the captured surface, or 0x0 before the first frame.
    void ContentSize(uint32_t* width, uint32_t* height) const;

    // True once the capture session has been closed by the system, which
    // happens when the captured window disappears.
    bool closed() const { return closed_.load(std::memory_order_acquire); }

    uint64_t frames_captured() const { return frames_captured_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    GraphicsContext* graphics_ = nullptr;

    mutable std::mutex mutex_;
    ComPtr<ID3D11Texture2D> latest_;
    ComPtr<ID3D11ShaderResourceView> latest_srv_;
    uint32_t content_width_ = 0;
    uint32_t content_height_ = 0;
    bool has_new_frame_ = false;
    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> frames_captured_{0};

    void OnFrame(ID3D11Texture2D* surface, uint32_t width, uint32_t height);
};

// Finds a top-level, visible window whose title contains `needle`
// (case-insensitive). Returns nullptr when nothing matches.
HWND FindWindowByTitleSubstring(const std::wstring& needle);

// True when Windows Graphics Capture is available on this system.
bool IsCaptureSupported();

}  // namespace gfn
