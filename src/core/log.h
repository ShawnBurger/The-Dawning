#pragma once
// =============================================================================
// core/log.h — Logging System
// =============================================================================
// Outputs to Visual Studio Output window via OutputDebugStringA, stderr, and a log
// file beside the executable. Supports Info/Warn/Error levels with printf formatting.
//
// THREAD-SAFE for the message calls (Info/Warn/Error[f] and ErrorCount): the error
// counter is atomic and each log line is emitted under a mutex, so worker threads
// (core::JobSystem) may log concurrently without tearing lines or losing error
// counts. Init()/Shutdown() are lifecycle calls and remain main-thread-only.
// (RULE 8 in CLAUDE.md predates the job system and should be updated to match.)
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
