// Minimal logging for the injected payload.
//
// We are inside someone else's process with no console and often no debugger,
// so a file in the temp dir is the only reliable channel. Logging is for init
// and error paths only - anything on the per-frame path must use
// OVERLAY_LOG_ONCE so a persistent failure cannot turn into an I/O storm on
// the game's render thread.
#pragma once

#include <cstdarg>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <share.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace overlay {

inline void LogImpl(const char* fmt, ...) {
    static FILE* file = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
#ifdef _WIN32
        char dir[MAX_PATH];
        if (GetTempPathA(MAX_PATH, dir)) {
            char path[MAX_PATH];
            sprintf_s(path, "%sGameOverlay.Avalonia.%lu.log", dir, GetCurrentProcessId());
            // _fsopen with _SH_DENYWR rather than fopen_s: fopen_s opens with
            // exclusive sharing, which stops anyone tailing the log while the
            // game is running - exactly when you need to read it.
            file = _fsopen(path, "w", _SH_DENYWR);
        }
#else
        char path[256];
        std::snprintf(path, sizeof(path), "/tmp/GameOverlay.Avalonia.%d.log",
                      static_cast<int>(getpid()));
        file = std::fopen(path, "w");
#endif
    }

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char line[1120];
#ifdef _WIN32
    sprintf_s(line, "[overlay %5lu] %s\n", GetCurrentThreadId(), msg);
    OutputDebugStringA(line);
#else
    std::snprintf(line, sizeof(line), "[overlay %5ld] %s\n",
                  static_cast<long>(syscall(SYS_gettid)), msg);
#endif
    if (file) {
        fputs(line, file);
        fflush(file);   // the game may be killed at any moment; never buffer
    }
}

} // namespace overlay

#define OVERLAY_LOG(...) ::overlay::LogImpl(__VA_ARGS__)

// For anything reachable from the Present hook.
#define OVERLAY_LOG_ONCE(...)                     \
    do {                                          \
        static bool logged_ = false;              \
        if (!logged_) {                           \
            logged_ = true;                       \
            ::overlay::LogImpl(__VA_ARGS__);      \
        }                                         \
    } while (0)
