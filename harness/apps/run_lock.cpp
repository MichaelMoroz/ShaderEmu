#include "run_lock.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// A shared or exclusive LockFileEx on one byte of %LOCALAPPDATA%\ShaderEmu\harness.lock; each
// holder also keeps holders\PID.txt open delete-on-close, saying what it is, for the refusal.
static std::wstring lock_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) n = GetTempPathW(MAX_PATH, buf);
    std::wstring d(buf, n);
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    d += L"ShaderEmu";
    CreateDirectoryW(d.c_str(), nullptr);
    CreateDirectoryW((d + L"\\holders").c_str(), nullptr);
    return d;
}

static std::string holders(const std::wstring& dir) {
    std::string out;
    int count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\holders\\*.txt").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        HANDLE h = CreateFileW((dir + L"\\holders\\" + fd.cFileName).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        char text[200] = {};
        DWORD got = 0;
        ReadFile(h, text, sizeof text - 1, &got, nullptr);
        CloseHandle(h);
        if (got == 0) continue;
        if (++count <= 4) out += std::string(out.empty() ? "" : "; ") + text;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (count > 4) out += "; and " + std::to_string(count - 4) + " more";
    return out;
}

bool run_lock_take(bool gpu, const char* program) {
    std::wstring dir = lock_dir();
    HANDLE lock = CreateFileW((dir + L"\\harness.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
    const char* mode = gpu ? "GPU" : "CPU";
    if (lock == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[lock] %s: cannot open the harness lock (error %lu); running without it\n", program, GetLastError());
        return true;
    }
    OVERLAPPED ov = {};
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (gpu ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (!LockFileEx(lock, flags, 0, 1, 0, &ov)) {
        std::string who = holders(dir);
        if (who.empty()) who = gpu ? "CPU or GPU harness (not yet named)" : "GPU harness (not yet named)";
        fprintf(stderr, "[lock] %s refused (%s mode): held by %s. Any number of CPU harnesses or one GPU harness alone (exit %d)\n",
                program, mode, who.c_str(), kRunLockRefused);
        CloseHandle(lock);
        return false;
    }
    // (the handles are never closed: the process's end releases both)
    DWORD pid = GetCurrentProcessId();
    HANDLE me = CreateFileW((dir + L"\\holders\\" + std::to_wstring(pid) + L".txt").c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (me != INVALID_HANDLE_VALUE) {
        char text[160];
        int n = snprintf(text, sizeof text, "%s harness %s pid %lu", mode, program, pid);
        DWORD put = 0;
        WriteFile(me, text, (DWORD)n, &put, nullptr);
    }
    // RVC_LOCK_HOLD=S: hold the lock S seconds and exit, touching nothing else (to test the rule)
    if (const char* hold = getenv("RVC_LOCK_HOLD")) {
        fprintf(stderr, "[lock] %s holds the %s lock for %s s (RVC_LOCK_HOLD)\n", program, mode, hold);
        Sleep((DWORD)(atof(hold) * 1000));
        exit(0);
    }
    return true;
}
