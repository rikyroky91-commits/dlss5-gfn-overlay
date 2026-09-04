#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "app.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "capture.h"
#include "d3d.h"
#include "hotkey.h"
#include "log.h"
#include "presenter.h"
#include "stats.h"
#include "worker.h"

namespace gfn {
namespace {

constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyQuit = 2;
constexpr int kHotkeyStats = 3;

using Clock = std::chrono::steady_clock;

double MillisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The enhanced frame handed from the neural thread to the present thread. One
// slot, overwritten in place: an older enhanced frame is never worth showing.
struct FrameSlot {
    std::mutex mutex;
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t version = 0;
    // When the frame was pulled off the capture surface, so the present thread
    // can report true end-to-end latency instead of adding up stage medians.
    std::chrono::steady_clock::time_point captured_at{};
};

// Waits for the target window to appear, so the overlay can be launched before
// the game client.
HWND WaitForWindow(const std::wstring& title, std::atomic<bool>* stop) {
    bool announced = false;
    while (!stop->load(std::memory_order_acquire)) {
        HWND window = FindWindowByTitleSubstring(title);
        if (window) return window;
        if (!announced) {
            GFN_INFO("waiting for a window whose title contains the configured text");
            announced = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return nullptr;
}

// Source rectangle inside the captured window after the configured crop.
SourceRect CropRect(const Config& config, uint32_t width, uint32_t height) {
    SourceRect rect;
    if (width == 0 || height == 0) return rect;
    rect.u0 = static_cast<float>(config.crop_left) / static_cast<float>(width);
    rect.v0 = static_cast<float>(config.crop_top) / static_cast<float>(height);
    rect.u1 = 1.0f - static_cast<float>(config.crop_right) / static_cast<float>(width);
    rect.v1 = 1.0f - static_cast<float>(config.crop_bottom) / static_cast<float>(height);
    return rect;
}

bool CroppedSize(const Config& config, uint32_t width, uint32_t height, uint32_t* out_width,
                 uint32_t* out_height) {
    const int64_t w = static_cast<int64_t>(width) - config.crop_left - config.crop_right;
    const int64_t h = static_cast<int64_t>(height) - config.crop_top - config.crop_bottom;
    if (w < 64 || h < 64) return false;
    // The neural runtime works in even dimensions; trimming one pixel is
    // cheaper than letting the worker reject the size.
    *out_width = static_cast<uint32_t>(w & ~1ll);
    *out_height = static_cast<uint32_t>(h & ~1ll);
    return true;
}

}  // namespace

int Run(const Config& config, const RunOptions& options) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::string error;
    GraphicsContext graphics;
    if (!graphics.Initialize(&error)) {
        GFN_ERROR("%s", error.c_str());
        return 1;
    }
    GFN_INFO("graphics adapter: %s", graphics.adapter_description().c_str());

    std::atomic<bool> stop{false};
    HWND source_window = WaitForWindow(config.window_title, &stop);
    if (!source_window) return 1;

    GFN_INFO("capturing window 0x%p", static_cast<void*>(source_window));

    WindowCapture capture;
    if (!capture.Start(&graphics, source_window, config.capture_cursor, &error)) {
        GFN_ERROR("%s", error.c_str());
        return 1;
    }

    // The first frame tells us the real content size; the window rect includes
    // shadows and borders that are not part of the stream.
    uint32_t content_width = 0;
    uint32_t content_height = 0;
    for (int attempt = 0; attempt < 200 && content_width == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        capture.ContentSize(&content_width, &content_height);
    }
    if (content_width == 0) {
        GFN_ERROR("no frame arrived from the capture session within five seconds");
        return 1;
    }
    GFN_INFO("source content is %ux%u", content_width, content_height);

    Presenter::Options presenter_options;
    presenter_options.fullscreen = config.fullscreen;
    presenter_options.monitor_index = config.output_monitor;
    presenter_options.anchor = source_window;
    presenter_options.allow_tearing = config.allow_tearing;
    Presenter presenter;
    if (!presenter.Create(&graphics, presenter_options, &error)) {
        GFN_ERROR("%s", error.c_str());
        return 1;
    }

    struct HotkeyBinding { int id; const wchar_t* name; const std::wstring* text; };
    const HotkeyBinding bindings[] = {
        {kHotkeyToggle, L"toggle", &config.hotkey_toggle},
        {kHotkeyQuit, L"quit", &config.hotkey_quit},
        {kHotkeyStats, L"stats", &config.hotkey_stats},
    };
    for (const HotkeyBinding& binding : bindings) {
        Hotkey parsed{};
        if (!ParseHotkey(*binding.text, &parsed)) {
            GFN_ERROR("could not parse the %ls hotkey from the config", binding.name);
            return 1;
        }
        std::string hotkey_error;
        if (!RegisterGlobalHotkey(binding.id, parsed, &hotkey_error)) {
            GFN_WARN("the %ls hotkey is unavailable: %s", binding.name, hotkey_error.c_str());
        }
    }

    std::atomic<bool> enhanced{config.start_enhanced};
    std::atomic<bool> worker_failed{false};
    FrameSlot slot;
    PipelineStats stats;
    std::mutex stats_mutex;

    std::thread neural_thread([&] {
        NeuralWorker worker;
        ReadbackTarget readback;
        uint32_t worker_input_width = 0;
        uint32_t worker_input_height = 0;
        std::vector<uint8_t> input_pixels;
        std::vector<uint8_t> output_pixels;
        const std::vector<uint16_t> no_motion;
        bool needs_reset = true;
        int consecutive_failures = 0;

        while (!stop.load(std::memory_order_acquire)) {
            if (!enhanced.load(std::memory_order_acquire)) {
                if (worker.running()) {
                    GFN_INFO("passthrough: stopping the neural worker");
                    worker.Stop();
                    needs_reset = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (capture.closed()) {
                GFN_WARN("the captured window went away");
                stop.store(true, std::memory_order_release);
                break;
            }

            uint32_t width = 0;
            uint32_t height = 0;
            capture.ContentSize(&width, &height);
            uint32_t input_width = 0;
            uint32_t input_height = 0;
            if (!CroppedSize(config, width, height, &input_width, &input_height)) {
                GFN_ERROR("the crop leaves nothing to process at %ux%u", width, height);
                enhanced.store(false, std::memory_order_release);
                worker_failed.store(true, std::memory_order_release);
                continue;
            }

            if (!worker.running() || input_width != worker_input_width ||
                input_height != worker_input_height) {
                std::string start_error;
                if (!readback.Resize(&graphics, input_width, input_height, &start_error) ||
                    !worker.Start(config, input_width, input_height, &start_error)) {
                    GFN_ERROR("%s", start_error.c_str());
                    // Back off rather than spinning on a runtime that will keep
                    // refusing; three tries is enough to rule out a transient
                    // driver hiccup.
                    if (++consecutive_failures >= 3) {
                        GFN_ERROR("giving up on the neural worker; staying in passthrough");
                        enhanced.store(false, std::memory_order_release);
                        worker_failed.store(true, std::memory_order_release);
                        consecutive_failures = 0;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                worker_input_width = input_width;
                worker_input_height = input_height;
                needs_reset = true;
                consecutive_failures = 0;
                {
                    std::lock_guard<std::mutex> lock(stats_mutex);
                    ++stats.worker_restarts;
                }
            }

            const Clock::time_point frame_start = Clock::now();
            const SourceRect rect = CropRect(config, width, height);
            if (!capture.BlitLatest(readback.rtv(), DestRect::Fill(input_width, input_height),
                                    rect, /*consume=*/true)) {
                // No new frame yet. Yield instead of re-processing the same one:
                // the worker's time is better spent on the next arrival.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            std::string frame_error;
            if (!readback.Download(&graphics, &input_pixels, &frame_error)) {
                GFN_ERROR("%s", frame_error.c_str());
                continue;
            }
            const double capture_ms = MillisSince(frame_start);

            const Clock::time_point worker_start = Clock::now();
            const int64_t pts = std::chrono::duration_cast<std::chrono::microseconds>(
                                    Clock::now().time_since_epoch())
                                    .count();
            if (!worker.ProcessFrame(input_pixels.data(), no_motion, needs_reset, pts,
                                     &output_pixels, &frame_error)) {
                GFN_ERROR("%s", frame_error.c_str());
                needs_reset = true;
                continue;  // the worker stopped itself; the loop restarts it
            }
            needs_reset = false;
            const double worker_ms = MillisSince(worker_start);

            {
                std::lock_guard<std::mutex> lock(slot.mutex);
                slot.pixels.swap(output_pixels);
                slot.width = worker.setup().output_width;
                slot.height = worker.setup().output_height;
                slot.captured_at = frame_start;
                ++slot.version;
            }
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                stats.capture_ms.Add(capture_ms);
                stats.worker_ms.Add(worker_ms);
            }
        }

        worker.Stop();
    });

    UploadTexture upload;
    uint64_t shown_version = 0;
    std::vector<uint8_t> present_pixels;
    Clock::time_point last_stats = Clock::now();
    int enhanced_frames_presented = 0;
    int exit_code = 0;

    while (!stop.load(std::memory_order_acquire)) {
        bool quit_requested = false;
        const bool alive = presenter.PumpMessages([&](const MSG& message) {
            if (message.message != WM_HOTKEY) return;
            switch (static_cast<int>(message.wParam)) {
                case kHotkeyToggle: {
                    const bool next = !enhanced.load(std::memory_order_acquire);
                    if (next && worker_failed.load(std::memory_order_acquire)) {
                        worker_failed.store(false, std::memory_order_release);
                    }
                    enhanced.store(next, std::memory_order_release);
                    GFN_INFO("%s", next ? "neural rendering on" : "passthrough");
                    break;
                }
                case kHotkeyQuit:
                    quit_requested = true;
                    break;
                case kHotkeyStats: {
                    std::lock_guard<std::mutex> lock(stats_mutex);
                    GFN_INFO("%s", stats.Summary().c_str());
                    break;
                }
                default:
                    break;
            }
        });
        if (!alive || quit_requested) break;

        if (capture.closed()) {
            GFN_WARN("the captured window closed");
            break;
        }

        const Clock::time_point present_start = Clock::now();
        bool presented = false;

        if (enhanced.load(std::memory_order_acquire)) {
            uint32_t width = 0;
            uint32_t height = 0;
            bool have_frame = false;
            Clock::time_point captured_at{};
            uint64_t skipped = 0;
            {
                std::lock_guard<std::mutex> lock(slot.mutex);
                if (slot.version != shown_version && !slot.pixels.empty()) {
                    present_pixels = slot.pixels;
                    width = slot.width;
                    height = slot.height;
                    captured_at = slot.captured_at;
                    skipped = slot.version - shown_version - 1;
                    shown_version = slot.version;
                    have_frame = true;
                }
            }
            if (have_frame) {
                if (!upload.Resize(&graphics, width, height, &error) ||
                    !upload.Upload(&graphics, present_pixels.data(), &error)) {
                    GFN_ERROR("%s", error.c_str());
                    break;
                }
                if (!presenter.Present(upload.srv(), width, height, &error)) {
                    GFN_ERROR("%s", error.c_str());
                    exit_code = 1;
                    break;
                }
                presented = true;
                ++enhanced_frames_presented;
                std::lock_guard<std::mutex> lock(stats_mutex);
                stats.frames_dropped += skipped;
                stats.total_ms.Add(MillisSince(captured_at));
            }
        } else {
            uint32_t width = 0;
            uint32_t height = 0;
            capture.ContentSize(&width, &height);
            uint32_t cropped_width = 0;
            uint32_t cropped_height = 0;
            if (CroppedSize(config, width, height, &cropped_width, &cropped_height)) {
                const SourceRect rect = CropRect(config, width, height);
                const bool ok = presenter.PresentWith(
                    [&](ID3D11RenderTargetView* target, uint32_t target_width,
                        uint32_t target_height) {
                        const DestRect dest = DestRect::Fit(target_width, target_height,
                                                            cropped_width, cropped_height);
                        capture.BlitLatest(target, dest, rect, /*consume=*/false);
                    },
                    &error);
                if (!ok) {
                    GFN_ERROR("%s", error.c_str());
                    exit_code = 1;
                    break;
                }
                presented = true;
            }
        }

        if (presented) {
            std::lock_guard<std::mutex> lock(stats_mutex);
            stats.present_ms.Add(MillisSince(present_start));
        } else {
            // Nothing new to draw. Sleeping here rather than spinning keeps a
            // core free for the worker.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (options.stats_interval_seconds > 0 &&
            MillisSince(last_stats) >= options.stats_interval_seconds * 1000.0) {
            last_stats = Clock::now();
            std::lock_guard<std::mutex> lock(stats_mutex);
            GFN_INFO("%s", stats.Summary().c_str());
        }

        if (options.bench_frames > 0 && enhanced_frames_presented >= options.bench_frames) {
            GFN_INFO("benchmark complete");
            break;
        }
    }

    stop.store(true, std::memory_order_release);
    if (neural_thread.joinable()) neural_thread.join();

    for (const HotkeyBinding& binding : bindings) UnregisterGlobalHotkey(binding.id);
    presenter.Destroy();
    capture.Stop();

    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        GFN_INFO("%s", stats.Summary().c_str());
    }
    return exit_code;
}

}  // namespace gfn
