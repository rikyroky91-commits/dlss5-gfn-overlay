#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "log.h"

namespace gfn {
namespace {

std::string Trim(const std::string& value) {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::wstring Widen(const std::string& value) {
    // The config file is ASCII in practice; a window title with non-ASCII
    // characters is the one case worth handling, so go through the UTF-8 path.
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        wide.data(), needed);
    return wide;
}

bool ParseBool(const std::string& value, bool* out) {
    const std::string lowered = Lower(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        *out = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        *out = false;
        return true;
    }
    return false;
}

bool ParseInt(const std::string& value, int* out) {
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) return false;
        *out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseFloat(const std::string& value, float* out) {
    try {
        size_t consumed = 0;
        const float parsed = std::stof(value, &consumed);
        if (consumed != value.size()) return false;
        if (!std::isfinite(parsed)) return false;
        *out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool InRange(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

}  // namespace

bool PerfQualityForFactor(float factor, int* perf_quality) {
    struct Entry { float factor; int perf_quality; };
    // Mirrors the DLSS mode table: DLAA, Quality, Balanced, Performance,
    // Ultra Performance.
    static const Entry kModes[] = {
        {1.0f, 5}, {1.5f, 2}, {1.724f, 1}, {2.0f, 0}, {3.0f, 3},
    };
    for (const Entry& entry : kModes) {
        if (std::fabs(factor - entry.factor) < 1e-4f) {
            *perf_quality = entry.perf_quality;
            return true;
        }
    }
    return false;
}

bool ValidateConfig(const Config& config, std::string* error) {
    int perf_quality = 0;
    if (!PerfQualityForFactor(config.upscale_factor, &perf_quality)) {
        *error = "upscale_factor must be one of 1.0, 1.5, 1.724, 2.0, 3.0";
        return false;
    }
    if (config.neural.preset < 0 || config.neural.preset > 3) {
        *error = "nr_preset must be between 0 and 3";
        return false;
    }
    if (config.neural.style < 0 || config.neural.style > 2) {
        *error = "nr_style must be 0 (default), 1 (natural) or 2 (cinematic)";
        return false;
    }
    if (!InRange(config.neural.intensity, 0.0f, 2.0f)) {
        *error = "nr_intensity must be between 0.0 and 2.0";
        return false;
    }
    if (!InRange(config.neural.local_tone, 0.0f, 2.0f)) {
        *error = "local_tone_strength must be between 0.0 and 2.0";
        return false;
    }
    if (!InRange(config.neural.local_structure, 0.0f, 2.0f)) {
        *error = "local_structure_strength must be between 0.0 and 2.0";
        return false;
    }
    if (!InRange(config.neural.skin_structure, -1.0f, 2.0f)) {
        *error = "skin_structure_strength must be between -1.0 and 2.0";
        return false;
    }
    switch (config.neural.dlss_model_preset) {
        case 0: case 10: case 11: case 12: case 13: break;
        default:
            *error = "dlss_model_preset must be 0 (default), 10 (J), 11 (K), 12 (L) or 13 (M)";
            return false;
    }
    if (config.crop_left < 0 || config.crop_top < 0 || config.crop_right < 0 ||
        config.crop_bottom < 0) {
        *error = "crop_* values must not be negative";
        return false;
    }
    if (config.warmup_frames < 0 || config.frame_count_hint < 0) {
        *error = "warmup_frames and frame_count_hint must not be negative";
        return false;
    }
    if (config.worker_timeout_ms < 100) {
        *error = "worker_timeout_ms must be at least 100";
        return false;
    }
    return true;
}

bool LoadConfig(const std::wstring& path, Config* config, std::string* error) {
    std::ifstream stream(path);
    if (!stream) {
        GFN_INFO("no config file at the requested path, using defaults");
        return ValidateConfig(*config, error);
    }

    std::string line;
    int line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
        if (trimmed[0] == '[') continue;  // section headers are decorative here

        const size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            *error = "line " + std::to_string(line_number) + ": expected key = value";
            return false;
        }
        const std::string key = Lower(Trim(trimmed.substr(0, separator)));
        const std::string value = Trim(trimmed.substr(separator + 1));

        bool ok = true;
        if (key == "window_title") {
            config->window_title = Widen(value);
            ok = !config->window_title.empty();
        } else if (key == "crop_left") {
            ok = ParseInt(value, &config->crop_left);
        } else if (key == "crop_top") {
            ok = ParseInt(value, &config->crop_top);
        } else if (key == "crop_right") {
            ok = ParseInt(value, &config->crop_right);
        } else if (key == "crop_bottom") {
            ok = ParseInt(value, &config->crop_bottom);
        } else if (key == "capture_cursor") {
            ok = ParseBool(value, &config->capture_cursor);
        } else if (key == "upscale_factor") {
            ok = ParseFloat(value, &config->upscale_factor);
        } else if (key == "nr_preset") {
            ok = ParseInt(value, &config->neural.preset);
        } else if (key == "nr_style") {
            ok = ParseInt(value, &config->neural.style);
        } else if (key == "automatic_mask") {
            ok = ParseBool(value, &config->neural.automatic_mask);
        } else if (key == "nr_intensity") {
            ok = ParseFloat(value, &config->neural.intensity);
        } else if (key == "local_tone_strength") {
            ok = ParseFloat(value, &config->neural.local_tone);
        } else if (key == "local_structure_strength") {
            ok = ParseFloat(value, &config->neural.local_structure);
        } else if (key == "skin_structure_strength") {
            ok = ParseFloat(value, &config->neural.skin_structure);
        } else if (key == "dlss_model_preset") {
            ok = ParseInt(value, &config->neural.dlss_model_preset);
        } else if (key == "warmup_frames") {
            ok = ParseInt(value, &config->warmup_frames);
        } else if (key == "frame_count_hint") {
            ok = ParseInt(value, &config->frame_count_hint);
        } else if (key == "fullscreen") {
            ok = ParseBool(value, &config->fullscreen);
        } else if (key == "output_monitor") {
            ok = ParseInt(value, &config->output_monitor);
        } else if (key == "allow_tearing") {
            ok = ParseBool(value, &config->allow_tearing);
        } else if (key == "start_enhanced") {
            ok = ParseBool(value, &config->start_enhanced);
        } else if (key == "hotkey_toggle") {
            config->hotkey_toggle = Widen(value);
        } else if (key == "hotkey_quit") {
            config->hotkey_quit = Widen(value);
        } else if (key == "hotkey_stats") {
            config->hotkey_stats = Widen(value);
        } else if (key == "worker_path") {
            config->worker_path = Widen(value);
        } else if (key == "worker_working_dir") {
            config->worker_working_dir = Widen(value);
        } else if (key == "worker_timeout_ms") {
            ok = ParseInt(value, &config->worker_timeout_ms);
        } else if (key == "log_level") {
            config->log_level = Lower(value);
        } else {
            GFN_WARN("ignoring unknown config key '%s' on line %d", key.c_str(), line_number);
        }

        if (!ok) {
            *error = "line " + std::to_string(line_number) + ": invalid value for '" + key + "'";
            return false;
        }
    }

    return ValidateConfig(*config, error);
}

}  // namespace gfn
