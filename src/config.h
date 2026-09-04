#pragma once

#include <string>

namespace gfn {

// Neural Rendering controls. Ranges mirror the ones the DLSSNR worker validates;
// out-of-range values are rejected at load time rather than by the worker.
struct NeuralSettings {
    int   preset = 0;             // 0 = default, 1..3 = experimental model hints
    int   style = 0;              // 0 = default, 1 = natural, 2 = cinematic
    bool  automatic_mask = false;
    float intensity = 1.0f;              // 0.00 .. 2.00
    float local_tone = 1.0f;             // 0.00 .. 2.00
    float local_structure = 1.0f;        // 0.00 .. 2.00
    float skin_structure = -1.0f;        // -1.00 .. 2.00 (-1 keeps the native default)
    int   dlss_model_preset = 0;         // 0 default, 10=J, 11=K, 12=L, 13=M
};

struct Config {
    // Source
    std::wstring window_title = L"GeForce NOW";  // substring match, case-insensitive
    int crop_left = 0;
    int crop_top = 0;
    int crop_right = 0;
    int crop_bottom = 0;
    bool capture_cursor = false;

    // Neural Rendering
    // Fraction of the cropped source the neural pass actually runs on. The
    // runtime's cost scales with pixel count, so this is the main lever when a
    // GPU cannot hold the frame budget: 0.667 with upscale_factor 1.5 sends
    // 720p through the network and gets 1080p back.
    float neural_input_scale = 1.0f;  // 0.25 .. 1.0
    float upscale_factor = 1.0f;   // 1.0 DLAA, 1.5, 1.724, 2.0, 3.0
    NeuralSettings neural;
    int warmup_frames = 0;
    int frame_count_hint = 0;      // 0 = unbounded/streaming

    // Output
    bool fullscreen = true;
    int  output_monitor = -1;      // -1 = monitor holding the source window
    bool allow_tearing = true;
    bool start_enhanced = true;
    bool exclude_from_capture = true;

    // Hotkeys, parsed from strings like "ctrl+alt+F1".
    std::wstring hotkey_toggle = L"alt+F1";
    std::wstring hotkey_quit = L"alt+F4";
    std::wstring hotkey_stats = L"alt+F2";

    // Runtime
    std::wstring worker_path = L"runtime/host/nvngx.dll";
    std::wstring worker_working_dir = L"runtime/host";
    int worker_timeout_ms = 10000;

    std::string log_level = "info";
};

// Loads an INI file. Missing file leaves defaults untouched and returns true.
// Returns false and fills `error` when a present file contains an invalid value.
bool LoadConfig(const std::wstring& path, Config* config, std::string* error);

// Validates ranges independently of the file, so defaults changed in code are
// checked too.
bool ValidateConfig(const Config& config, std::string* error);

// Maps an upscale factor to the worker's perf_quality enum. Returns false for a
// factor the DLSS runtime does not define.
bool PerfQualityForFactor(float factor, int* perf_quality);

}  // namespace gfn
