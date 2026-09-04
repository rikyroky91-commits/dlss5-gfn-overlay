#include "log.h"

#include <cstdarg>
#include <ctime>
#include <vector>

namespace gfn {
namespace {

std::mutex g_mutex;
LogLevel g_level = LogLevel::Info;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info ";
        case LogLevel::Warn:  return "warn ";
        case LogLevel::Error: return "error";
    }
    return "?????";
}

}  // namespace

void SetLogLevel(LogLevel level) { g_level = level; }

void LogLine(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) return;

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::FILE* stream = level == LogLevel::Error ? stderr : stdout;
    std::fprintf(stream, "[%s %s] %s\n", stamp, LevelName(level), message.c_str());
    std::fflush(stream);
}

void LogFmt(LogLevel level, const char* format, ...) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) return;

    va_list args;
    va_start(args, format);
    va_list probe;
    va_copy(probe, args);
    const int needed = std::vsnprintf(nullptr, 0, format, probe);
    va_end(probe);
    if (needed < 0) {
        va_end(args);
        return;
    }
    std::vector<char> buffer(static_cast<size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);
    LogLine(level, std::string(buffer.data(), static_cast<size_t>(needed)));
}

}  // namespace gfn
