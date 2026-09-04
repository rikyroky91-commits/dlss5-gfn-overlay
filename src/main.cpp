#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winrt/base.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "capture.h"
#include "config.h"
#include "log.h"

namespace {

void PrintUsage() {
    std::printf(
        "dlss5-gfn-overlay - real-time DLSS 5 Neural Rendering on a captured window\n"
        "\n"
        "Usage:\n"
        "  dlss5-gfn-overlay.exe [options]\n"
        "\n"
        "Options:\n"
        "  --config <path>   INI file to read (default: config.ini next to the exe)\n"
        "  --window <text>   Override window_title from the config\n"
        "  --bench <n>       Process n enhanced frames, print the timings, exit\n"
        "  --stats <sec>     Print a timing line every sec seconds (0 disables)\n"
        "  --log-level <lvl> debug, info, warn or error\n"
        "  --list-windows    Print capturable top-level windows and exit\n"
        "  --help            This text\n");
}

std::wstring Widen(const char* value) {
    if (!value) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring wide(static_cast<size_t>(needed) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, wide.data(), needed);
    return wide;
}

BOOL CALLBACK ListProc(HWND window, LPARAM) {
    if (!IsWindowVisible(window)) return TRUE;
    if (GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    title.resize(static_cast<size_t>(copied));
    RECT rect{};
    GetClientRect(window, &rect);
    std::wprintf(L"  %5ldx%-5ld  %s\n", rect.right - rect.left, rect.bottom - rect.top,
                 title.c_str());
    return TRUE;
}

std::wstring DefaultConfigPath() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) path.resize(slash + 1);
    return path + L"config.ini";
}

}  // namespace

int main(int argc, char** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    std::wstring config_path = DefaultConfigPath();
    std::wstring window_override;
    gfn::RunOptions options;
    std::string log_level_override;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        } else if (argument == "--config") {
            config_path = Widen(next("--config"));
        } else if (argument == "--window") {
            window_override = Widen(next("--window"));
        } else if (argument == "--bench") {
            options.bench_frames = std::atoi(next("--bench"));
        } else if (argument == "--stats") {
            options.stats_interval_seconds = std::atoi(next("--stats"));
        } else if (argument == "--log-level") {
            log_level_override = next("--log-level");
        } else if (argument == "--list-windows") {
            std::wprintf(L"Capturable top-level windows:\n");
            EnumWindows(ListProc, 0);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            PrintUsage();
            return 2;
        }
    }

    gfn::Config config;
    std::string error;
    if (!gfn::LoadConfig(config_path, &config, &error)) {
        std::fprintf(stderr, "config error: %s\n", error.c_str());
        return 2;
    }
    if (!window_override.empty()) config.window_title = window_override;
    if (!log_level_override.empty()) config.log_level = log_level_override;

    if (config.log_level == "debug") {
        gfn::SetLogLevel(gfn::LogLevel::Debug);
    } else if (config.log_level == "warn") {
        gfn::SetLogLevel(gfn::LogLevel::Warn);
    } else if (config.log_level == "error") {
        gfn::SetLogLevel(gfn::LogLevel::Error);
    } else {
        gfn::SetLogLevel(gfn::LogLevel::Info);
    }

    return gfn::Run(config, options);
}
