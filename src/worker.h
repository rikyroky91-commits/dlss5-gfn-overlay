#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace gfn {

// Result of the worker's setup handshake. The worker negotiates the sizes it
// will actually accept, which can differ from what we asked for.
struct WorkerSetup {
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t minimum_width = 0;
    uint32_t minimum_height = 0;
    uint32_t maximum_width = 0;
    uint32_t maximum_height = 0;
    uint32_t applied_dlss_model_preset = 0;
};

// Drives the persistent DLSS Neural Rendering worker over its stdin/stdout
// protocol: one setup handshake, then one request/response per frame.
//
// The worker is a separate process on purpose. A crash inside the neural
// runtime (feature 18 raises 0xC0000005 on some driver/GPU combinations) takes
// down the worker, not the overlay, and Restart() brings it back.
class NeuralWorker {
public:
    NeuralWorker() = default;
    ~NeuralWorker();

    NeuralWorker(const NeuralWorker&) = delete;
    NeuralWorker& operator=(const NeuralWorker&) = delete;

    // Starts the worker and performs the setup handshake for a fixed input
    // size. Call again after Stop() to change resolution.
    bool Start(const Config& config, uint32_t input_width, uint32_t input_height,
               std::string* error);
    void Stop();
    bool running() const { return process_ != nullptr; }

    const WorkerSetup& setup() const { return setup_; }
    uint32_t input_width() const { return input_width_; }
    uint32_t input_height() const { return input_height_; }

    // Sends one RGBA8 frame and blocks until the enhanced frame comes back.
    // `rgba` must hold input_width * input_height * 4 bytes. `motion` may be
    // empty, in which case a zero motion field is sent (what the worker's
    // still-image path uses).
    //
    // On failure the worker is left stopped and `error` carries the diagnosis,
    // including the last lines the worker wrote to stderr.
    bool ProcessFrame(const uint8_t* rgba, const std::vector<uint16_t>& motion, bool reset,
                      int64_t pts, std::vector<uint8_t>* output, std::string* error);

    // Last lines the worker wrote to stderr, newest last.
    std::vector<std::string> DrainLog();

private:
    bool WriteAll(const void* data, size_t bytes, std::string* error);
    bool ReadAll(void* data, size_t bytes, std::string* error);
    std::string FailureDetail(const std::string& summary);

    void* process_ = nullptr;       // HANDLE
    void* stdin_write_ = nullptr;   // HANDLE
    void* stdout_read_ = nullptr;   // HANDLE
    void* stderr_read_ = nullptr;   // HANDLE
    void* stderr_thread_ = nullptr; // HANDLE

    WorkerSetup setup_{};
    uint32_t input_width_ = 0;
    uint32_t input_height_ = 0;
    uint32_t frame_index_ = 0;
    size_t motion_bytes_ = 0;
    std::vector<uint16_t> zero_motion_;

    struct LogBuffer;
    LogBuffer* log_ = nullptr;
};

// Rounds a dimension the way the worker's sizing rules do: nearest even pixel,
// at least 2.
uint32_t NearestEven(double value);

// Computes the worker output size for an input size and factor. Returns false
// when the result would exceed the 7680x4320 boundary the runtime enforces.
bool ResolveOutputSize(uint32_t width, uint32_t height, float factor,
                       uint32_t* out_width, uint32_t* out_height);

}  // namespace gfn
