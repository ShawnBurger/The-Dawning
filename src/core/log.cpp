// =============================================================================
// core/log.cpp — Logging System Implementation
// =============================================================================

#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace core { namespace Log {

static LARGE_INTEGER s_startTime;
static LARGE_INTEGER s_frequency;
static bool s_initialized = false;
static FILE* s_file = nullptr;

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
    if (level == LogLevel::Error) prefix = "[ERR ]";

    char buffer[640];
    snprintf(buffer, sizeof(buffer), "[%8.3f] %s %s\n", elapsed, prefix, msg);
    buffer[sizeof(buffer) - 1] = '\0';

    OutputDebugStringA(buffer);

    // Also write to stderr for console builds
    fputs(buffer, stderr);

    if (s_file)
    {
        fputs(buffer, s_file);
        fflush(s_file);
    }
}

void Info(const char* msg)  { Output(LogLevel::Info, msg); }
void Warn(const char* msg)  { Output(LogLevel::Warn, msg); }
void Error(const char* msg) { Output(LogLevel::Error, msg); }

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
