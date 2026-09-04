#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace gfn {

enum class LogLevel { Debug, Info, Warn, Error };

void SetLogLevel(LogLevel level);
void LogLine(LogLevel level, const std::string& message);

// Formats with printf semantics. Kept as a free function rather than a macro so
// the call sites stay greppable.
void LogFmt(LogLevel level, const char* format, ...);

#define GFN_DEBUG(...) ::gfn::LogFmt(::gfn::LogLevel::Debug, __VA_ARGS__)
#define GFN_INFO(...)  ::gfn::LogFmt(::gfn::LogLevel::Info, __VA_ARGS__)
#define GFN_WARN(...)  ::gfn::LogFmt(::gfn::LogLevel::Warn, __VA_ARGS__)
#define GFN_ERROR(...) ::gfn::LogFmt(::gfn::LogLevel::Error, __VA_ARGS__)

}  // namespace gfn
