#pragma once
// =============================================================================
// core/log.h — Logging System
// =============================================================================
// Outputs to Visual Studio Output window via OutputDebugStringA.
// NOT thread-safe — call only from the main thread until a job system exists.
// Supports Info/Warn/Error levels with printf-style formatting.
// =============================================================================

#include <cstdint>

namespace core
{

enum class LogLevel : uint8_t
{
    Info,
    Warn,
    Error
};

namespace Log
{
    void Init();
    void Shutdown();

    void Info(const char* msg);
    void Warn(const char* msg);
    void Error(const char* msg);

    // Printf-style variants (up to 512 chars)
    void Infof(const char* fmt, ...);
    void Warnf(const char* fmt, ...);
    void Errorf(const char* fmt, ...);

    // Number of Error-level messages logged since process start. Lets the smoke
    // harness fail a run whose frame loop logged errors but still exited cleanly.
    uint32_t ErrorCount();

} // namespace Log

} // namespace core
