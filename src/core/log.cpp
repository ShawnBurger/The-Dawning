// =============================================================================
// core/log.cpp — Logging System Implementation
// =============================================================================

#include "log.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <mutex>

namespace core { namespace Log {

static LARGE_INTEGER s_startTime;
static LARGE_INTEGER s_frequency;
static bool s_initialized = false;
static FILE* s_file = nullptr;
// THREAD-SAFETY: a plain ++ on the error count from two threads is a data race (UB
// by the C++ standard); it is atomic. The output mutex below makes a whole log LINE
// atomic across all three sinks (debugger + stderr + file) so concurrent callers do
// not interleave within a line. NOTE ON TESTABILITY: the surrounding output I/O
// (OutputDebugStringA is a syscall, the CRT locks FILE* for fputs) already serialises
// concurrent Error() calls in practice, so a unit test CANNOT observe the counter
// race even fully unsynchronised - there is no watched failure for this on MSVC (no
// ThreadSanitizer). The hardening removes the UB by construction (verified by
// inspection), and test_log.cpp only asserts that concurrent logging is crash-free
// and the count is exact - it does NOT claim to discriminate the atomic.
static std::atomic<uint32_t> s_errorCount{ 0 };

// Function-local static avoids any static-init-order dependence.
static std::mutex& OutputMutex()
{
    static std::mutex m;
    return m;
}

static void BuildLogPath(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
        return;

    strncpy_s(outPath, outPathSize, "TheDawning.log", _TRUNCATE);

    char modulePath[MAX_PATH] = {};
    DWORD length = GetModuleFileNameA(nullptr, modulePath, static_cast<DWORD>(sizeof(modulePath)));
    if (length == 0 || length >= sizeof(modulePath))
        return;

    char* slash = strrchr(modulePath, '\\');
    char* forwardSlash = strrchr(modulePath, '/');
    if (forwardSlash && (!slash || forwardSlash > slash))
        slash = forwardSlash;
    if (!slash)
        return;

    *(slash + 1) = '\0';
    strncpy_s(outPath, outPathSize, modulePath, _TRUNCATE);
    strncat_s(outPath, outPathSize, "TheDawning.log", _TRUNCATE);
}

void Init()
{
    QueryPerformanceFrequency(&s_frequency);
    QueryPerformanceCounter(&s_startTime);

    char logPath[MAX_PATH] = {};
    BuildLogPath(logPath, sizeof(logPath));
    fopen_s(&s_file, logPath, "w");

    s_initialized = true;
    Info("Log system initialized");
    Infof("Log file: %s", logPath);
}

void Shutdown()
{
    Info("Log system shutdown");
    if (s_file)
    {
        fclose(s_file);
        s_file = nullptr;
    }
    s_initialized = false;
}

static void Output(LogLevel level, const char* msg)
{
    // Compute elapsed time since init
    double elapsed = 0.0;
    if (s_initialized)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        elapsed = static_cast<double>(now.QuadPart - s_startTime.QuadPart)
                / static_cast<double>(s_frequency.QuadPart);
    }

    const char* prefix = "[INFO]";
    if (level == LogLevel::Warn)  prefix = "[WARN]";
    if (level == LogLevel::Error) { prefix = "[ERR ]"; s_errorCount.fetch_add(1, std::memory_order_relaxed); }

    char buffer[640]; // per-call, on the stack: formatting is already thread-safe
    snprintf(buffer, sizeof(buffer), "[%8.3f] %s %s\n", elapsed, prefix, msg);
    buffer[sizeof(buffer) - 1] = '\0';

    // One line, emitted atomically across all sinks so concurrent callers do not
    // interleave within a line.
    std::lock_guard<std::mutex> lk(OutputMutex());
    OutputDebugStringA(buffer);
    fputs(buffer, stderr); // also to stderr for console builds
    if (s_file)
    {
        fputs(buffer, s_file);
        fflush(s_file);
    }
}

void Info(const char* msg)  { Output(LogLevel::Info, msg); }
void Warn(const char* msg)  { Output(LogLevel::Warn, msg); }
void Error(const char* msg) { Output(LogLevel::Error, msg); }

uint32_t ErrorCount() { return s_errorCount.load(std::memory_order_relaxed); }

void Infof(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    Output(LogLevel::Info, buf);
}

void Warnf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    Output(LogLevel::Warn, buf);
}

void Errorf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    Output(LogLevel::Error, buf);
}

}} // namespace core::Log
