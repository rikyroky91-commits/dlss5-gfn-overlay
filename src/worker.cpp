#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "worker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "log.h"

namespace gfn {
namespace {

// Protocol constants, taken from the worker's own framing.
constexpr uint32_t kVideoMagic = 0x34563544;  // setup request
constexpr uint32_t kSetupMagic = 0x34505553;  // setup response
constexpr uint32_t kFrameMagic = 0x314D5246;  // frame request
constexpr uint32_t kOutMagic   = 0x3154554F;  // frame response

#pragma pack(push, 1)
struct SetupRequest {
    uint32_t magic;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t warmup_frames;
    uint32_t frame_count;
    uint32_t perf_quality;
    uint32_t dlss_model_preset;
    uint32_t profile;
    uint32_t preset;
    uint32_t style;
    uint32_t auto_mask;
    uint32_t ui_correction;
    float    intensity;
    float    local_tone;
    float    local_structure;
    float    skin_structure;
};
static_assert(sizeof(SetupRequest) == 72, "setup request must match the '<14I4f' layout");

struct SetupResponse {
    uint32_t magic;
    uint32_t ok;
    uint32_t result;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t minimum_width;
    uint32_t minimum_height;
    uint32_t maximum_width;
    uint32_t maximum_height;
    uint32_t applied_dlss_model_preset;
};
static_assert(sizeof(SetupResponse) == 48, "setup response must match the '<12I' layout");

struct FrameRequest {
    uint32_t magic;
    uint32_t index;
    uint32_t reset;
    uint32_t reserved;
    int64_t  pts;
};
static_assert(sizeof(FrameRequest) == 24, "frame request must match the '<4Iq' layout");

struct FrameResponse {
    uint32_t magic;
    uint32_t index;
    uint32_t ok;
    uint32_t byte_count;
    uint32_t ngx_result;
    int64_t  pts;
};
static_assert(sizeof(FrameResponse) == 28, "frame response must match the '<5Iq' layout");
#pragma pack(pop)

std::string LastErrorText() {
    const DWORD code = GetLastError();
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string text = length && buffer ? std::string(buffer, length) : std::string("unknown error");
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return "0x" + [code] {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%08lX", static_cast<unsigned long>(code));
        return std::string(hex);
    }() + " " + text;
}

}  // namespace

struct NeuralWorker::LogBuffer {
    std::mutex mutex;
    std::deque<std::string> lines;
    std::string partial;

    void Append(const char* data, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < bytes; ++i) {
            const char c = data[i];
            if (c == '\n') {
                while (!partial.empty() && partial.back() == '\r') partial.pop_back();
                lines.push_back(partial);
                partial.clear();
                if (lines.size() > 200) lines.pop_front();
            } else {
                if (partial.size() < 4096) partial.push_back(c);
            }
        }
    }

    std::vector<std::string> Drain() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out(lines.begin(), lines.end());
        return out;
    }
};

namespace {

struct DrainContext {
    HANDLE pipe;
    NeuralWorker::LogBuffer* buffer;
};

DWORD WINAPI DrainStderr(LPVOID parameter) {
    // Owns the context and closes nothing: the worker owns the pipe handle and
    // closing it is what makes this thread exit.
    auto* context = static_cast<DrainContext*>(parameter);
    char chunk[4096];
    DWORD read = 0;
    while (ReadFile(context->pipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        context->buffer->Append(chunk, read);
    }
    delete context;
    return 0;
}

}  // namespace

uint32_t NearestEven(double value) {
    const long long rounded = static_cast<long long>(std::floor(value / 2.0 + 0.5)) * 2;
    return static_cast<uint32_t>(std::max<long long>(2, rounded));
}

bool ResolveOutputSize(uint32_t width, uint32_t height, float factor,
                       uint32_t* out_width, uint32_t* out_height) {
    const uint32_t w = NearestEven(static_cast<double>(width) * factor);
    const uint32_t h = NearestEven(static_cast<double>(height) * factor);
    if (std::max(w, h) > 7680 || std::min(w, h) > 4320) return false;
    *out_width = w;
    *out_height = h;
    return true;
}

NeuralWorker::~NeuralWorker() { Stop(); }

bool NeuralWorker::Start(const Config& config, uint32_t input_width, uint32_t input_height,
                         std::string* error) {
    Stop();

    int perf_quality = 0;
    if (!PerfQualityForFactor(config.upscale_factor, &perf_quality)) {
        *error = "unsupported upscale_factor";
        return false;
    }
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    if (!ResolveOutputSize(input_width, input_height, config.upscale_factor, &output_width,
                           &output_height)) {
        *error = "the requested output exceeds the 7680x4320 boundary; lower upscale_factor";
        return false;
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;
    auto close_if = [](HANDLE& handle) {
        if (handle) {
            CloseHandle(handle);
            handle = nullptr;
        }
    };
    auto fail = [&](const std::string& message) {
        close_if(stdin_read);  close_if(stdin_write);
        close_if(stdout_read); close_if(stdout_write);
        close_if(stderr_read); close_if(stderr_write);
        *error = message;
        return false;
    };

    if (!CreatePipe(&stdin_read, &stdin_write, &inheritable, 0))
        return fail("CreatePipe(stdin) failed: " + LastErrorText());
    if (!CreatePipe(&stdout_read, &stdout_write, &inheritable, 0))
        return fail("CreatePipe(stdout) failed: " + LastErrorText());
    if (!CreatePipe(&stderr_read, &stderr_write, &inheritable, 0))
        return fail("CreatePipe(stderr) failed: " + LastErrorText());

    // Only the child's ends may be inherited.
    if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        return fail("SetHandleInformation failed: " + LastErrorText());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;

    // The worker checks its own image name, so the executable must keep the
    // .dll name it ships with and be launched directly.
    std::wstring command = L"\"" + config.worker_path + L"\" --video";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    PROCESS_INFORMATION process_info{};
    const BOOL created = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        config.worker_working_dir.empty() ? nullptr : config.worker_working_dir.c_str(),
        &startup, &process_info);
    if (!created) {
        return fail("could not start the DLSS worker at '" +
                    std::string(command.begin(), command.end()) + "': " + LastErrorText());
    }

    CloseHandle(process_info.hThread);
    close_if(stdin_read);
    close_if(stdout_write);
    close_if(stderr_write);

    process_ = process_info.hProcess;
    stdin_write_ = stdin_write;
    stdout_read_ = stdout_read;
    stderr_read_ = stderr_read;
    log_ = new LogBuffer();

    auto* context = new DrainContext{stderr_read_, log_};
    stderr_thread_ = CreateThread(nullptr, 0, DrainStderr, context, 0, nullptr);
    if (!stderr_thread_) {
        delete context;
        GFN_WARN("could not start the worker stderr reader; diagnostics will be missing");
    }

    SetupRequest request{};
    request.magic = kVideoMagic;
    request.input_width = input_width;
    request.input_height = input_height;
    request.output_width = output_width;
    request.output_height = output_height;
    request.warmup_frames = static_cast<uint32_t>(config.warmup_frames);
    request.frame_count = static_cast<uint32_t>(config.frame_count_hint);
    request.perf_quality = static_cast<uint32_t>(perf_quality);
    request.dlss_model_preset = static_cast<uint32_t>(config.neural.dlss_model_preset);
    request.profile = 0;
    request.preset = static_cast<uint32_t>(config.neural.preset);
    request.style = static_cast<uint32_t>(config.neural.style);
    request.auto_mask = config.neural.automatic_mask ? 1u : 0u;
    request.ui_correction = 0;
    request.intensity = config.neural.intensity;
    request.local_tone = config.neural.local_tone;
    request.local_structure = config.neural.local_structure;
    request.skin_structure = config.neural.skin_structure;

    std::string io_error;
    if (!WriteAll(&request, sizeof(request), &io_error)) {
        *error = FailureDetail("the worker closed its input during setup: " + io_error);
        Stop();
        return false;
    }

    SetupResponse response{};
    if (!ReadAll(&response, sizeof(response), &io_error)) {
        *error = FailureDetail("the worker produced no setup response: " + io_error);
        Stop();
        return false;
    }
    if (response.magic != kSetupMagic) {
        *error = FailureDetail("the worker does not speak the version-4 setup protocol");
        Stop();
        return false;
    }
    if (!response.ok) {
        char ngx[32];
        std::snprintf(ngx, sizeof(ngx), "0x%08X", response.result);
        *error = FailureDetail(std::string("DLSS setup failed (NGX ") + ngx +
                               "); lower upscale_factor or update the NVIDIA driver");
        Stop();
        return false;
    }
    if (response.output_width != output_width || response.output_height != output_height) {
        *error = FailureDetail("the worker negotiated output dimensions we did not request");
        Stop();
        return false;
    }
    if (response.applied_dlss_model_preset !=
        static_cast<uint32_t>(config.neural.dlss_model_preset)) {
        *error = FailureDetail("the worker applied a different DLSS model preset than requested");
        Stop();
        return false;
    }

    setup_.render_width = response.render_width;
    setup_.render_height = response.render_height;
    setup_.output_width = response.output_width;
    setup_.output_height = response.output_height;
    setup_.minimum_width = response.minimum_width;
    setup_.minimum_height = response.minimum_height;
    setup_.maximum_width = response.maximum_width;
    setup_.maximum_height = response.maximum_height;
    setup_.applied_dlss_model_preset = response.applied_dlss_model_preset;

    input_width_ = input_width;
    input_height_ = input_height;
    frame_index_ = 0;
    motion_bytes_ = static_cast<size_t>(input_width) * input_height * 2 * sizeof(uint16_t);
    zero_motion_.assign(motion_bytes_ / sizeof(uint16_t), 0);

    GFN_INFO("worker ready: %ux%u in, %ux%u out, render %ux%u", input_width, input_height,
             setup_.output_width, setup_.output_height, setup_.render_width,
             setup_.render_height);
    return true;
}

void NeuralWorker::Stop() {
    if (stdin_write_) {
        CloseHandle(static_cast<HANDLE>(stdin_write_));
        stdin_write_ = nullptr;
    }
    if (process_) {
        // Closing stdin is the worker's shutdown signal; kill it only if it
        // ignores that.
        if (WaitForSingleObject(static_cast<HANDLE>(process_), 3000) != WAIT_OBJECT_0) {
            TerminateProcess(static_cast<HANDLE>(process_), 1);
            WaitForSingleObject(static_cast<HANDLE>(process_), 2000);
        }
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (stdout_read_) {
        CloseHandle(static_cast<HANDLE>(stdout_read_));
        stdout_read_ = nullptr;
    }
    if (stderr_read_) {
        CloseHandle(static_cast<HANDLE>(stderr_read_));
        stderr_read_ = nullptr;
    }
    if (stderr_thread_) {
        WaitForSingleObject(static_cast<HANDLE>(stderr_thread_), 2000);
        CloseHandle(static_cast<HANDLE>(stderr_thread_));
        stderr_thread_ = nullptr;
    }
    delete log_;
    log_ = nullptr;
    input_width_ = 0;
    input_height_ = 0;
    frame_index_ = 0;
}

bool NeuralWorker::WriteAll(const void* data, size_t bytes, std::string* error) {
    const auto* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(stdin_write_), cursor, chunk, &written, nullptr) ||
            written == 0) {
            *error = LastErrorText();
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool NeuralWorker::ReadAll(void* data, size_t bytes, std::string* error) {
    auto* cursor = static_cast<uint8_t*>(data);
    size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(static_cast<HANDLE>(stdout_read_), cursor, chunk, &read, nullptr) ||
            read == 0) {
            *error = LastErrorText();
            return false;
        }
        cursor += read;
        remaining -= read;
    }
    return true;
}

std::string NeuralWorker::FailureDetail(const std::string& summary) {
    std::string detail = summary;
    if (log_) {
        const std::vector<std::string> lines = log_->Drain();
        const size_t start = lines.size() > 40 ? lines.size() - 40 : 0;
        for (size_t i = start; i < lines.size(); ++i) {
            detail += "\n  worker: " + lines[i];
        }
    }
    return detail;
}

std::vector<std::string> NeuralWorker::DrainLog() {
    return log_ ? log_->Drain() : std::vector<std::string>{};
}

bool NeuralWorker::ProcessFrame(const uint8_t* rgba, const std::vector<uint16_t>& motion,
                                bool reset, int64_t pts, std::vector<uint8_t>* output,
                                std::string* error) {
    if (!process_) {
        *error = "the worker is not running";
        return false;
    }

    const std::vector<uint16_t>& motion_field = motion.empty() ? zero_motion_ : motion;
    if (motion_field.size() * sizeof(uint16_t) != motion_bytes_) {
        *error = "motion field size does not match the negotiated input size";
        return false;
    }

    FrameRequest request{};
    request.magic = kFrameMagic;
    request.index = frame_index_;
    request.reset = reset ? 1u : 0u;
    request.reserved = 0;
    request.pts = pts;

    const size_t rgba_bytes = static_cast<size_t>(input_width_) * input_height_ * 4;
    std::string io_error;
    if (!WriteAll(&request, sizeof(request), &io_error) ||
        !WriteAll(rgba, rgba_bytes, &io_error) ||
        !WriteAll(motion_field.data(), motion_bytes_, &io_error)) {
        *error = FailureDetail("the worker died while receiving frame " +
                               std::to_string(frame_index_) + ": " + io_error);
        Stop();
        return false;
    }

    FrameResponse response{};
    if (!ReadAll(&response, sizeof(response), &io_error)) {
        *error = FailureDetail("the worker died before answering frame " +
                               std::to_string(frame_index_) +
                               "; feature 18 raising 0xC0000005 is the usual cause: " + io_error);
        Stop();
        return false;
    }

    const size_t expected =
        static_cast<size_t>(setup_.output_width) * setup_.output_height * 4;
    if (response.magic != kOutMagic || !response.ok || response.index != frame_index_ ||
        response.byte_count != expected) {
        *error = FailureDetail("invalid worker response for frame " +
                               std::to_string(frame_index_));
        Stop();
        return false;
    }
    if (response.ngx_result != 1) {
        char ngx[32];
        std::snprintf(ngx, sizeof(ngx), "0x%08X", response.ngx_result);
        *error = FailureDetail(std::string("feature-18 evaluation failed: ") + ngx);
        Stop();
        return false;
    }

    output->resize(expected);
    if (!ReadAll(output->data(), expected, &io_error)) {
        *error = FailureDetail("the worker died while returning frame " +
                               std::to_string(frame_index_) + ": " + io_error);
        Stop();
        return false;
    }

    ++frame_index_;
    return true;
}

}  // namespace gfn
