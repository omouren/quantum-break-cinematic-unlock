#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include "asi_transaction.hpp"
#pragma comment(lib, "bcrypt.lib")
extern "C" DWORD WINAPI QBPrepare(void*);
extern "C" const QBPlan* QBGetPlan();

namespace {
HMODULE module = nullptr;
std::atomic_flag started = ATOMIC_FLAG_INIT;
constexpr char build_sha[] = "2E3BAF80750F8462469FEE270B6054496BDA73DE541BB19E72AD1EA26DE4715B";
static_assert(sizeof(build_sha) == 65);

void log(const char* message, DWORD code = 0) {
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(module, path, 32768);
    if (!n || n >= 32768 - 5) return;
    auto slash = std::wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, 32768 - (slash + 1 - path), L"QuantumBreakCinematicUnlock.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512]{}; SYSTEMTIME time{}; GetLocalTime(&time);
    char suffix[32]{};
    if (code) sprintf_s(suffix, " | code=%lu", code);
    int count = sprintf_s(line, "[QuantumBreakCinematicUnlock] %04u-%02u-%02u %02u:%02u:%02u | %s%s\r\n",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,message,suffix);
    DWORD written = 0;
    if (count > 0) WriteFile(file, line, DWORD(count), &written, nullptr);
    CloseHandle(file);
}

bool supported_exe() {
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n == 32768) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE result[32]{}; BYTE buffer[65536]; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0
        && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer, sizeof buffer, &read, nullptr)) { ok = false; break; }
            if (!read) break;
            if (BCryptHashData(hash, buffer, read, 0) < 0) { ok = false; break; }
        }
        if (ok) ok = BCryptFinishHash(hash, result, sizeof result, 0) >= 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    char hex[65]{};
    for (unsigned i = 0; i != 32; ++i) sprintf_s(hex + i * 2, 3, "%02X", result[i]);
    return ok && std::strcmp(hex, build_sha) == 0;
}

// Steam's executable may not be unpacked yet when the ASI loader calls us.
// ReadProcessMemory safely rejects pages which are not readable at this point.
bool engine_ready() {
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    struct Signature { unsigned rva; unsigned count; BYTE bytes[11]; };
    const Signature signatures[]{
        {0x2DACB1,9,{0x80,0xB9,0x74,0x07,0,0,0,0x74,0x11}},
        {0x2DBCBF,9,{0x80,0xB9,0x74,0x07,0,0,0,0x74,0x11}},
        {0x2DBE40,11,{0x48,0x8B,0xC4,0x57,0x48,0x81,0xEC,0x80,0x01,0,0}},
        {0x147930,5,{0x83,0xFA,0x02,0x75,0x5B}},
        {0x147810,10,{0x40,0x53,0x48,0x83,0xEC,0x30,0x83,0x79,0x38,0x02}}
    };
    for (const auto& s : signatures) {
        BYTE bytes[16]{}; SIZE_T n = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + s.rva),
            bytes, s.count, &n) || n != s.count || std::memcmp(bytes, s.bytes, n)) return false;
    }
    std::uintptr_t callback = 0; SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + 0xAB9158),
        &callback, sizeof callback, &n) && n == sizeof callback && callback == base + 0x147930;
}

DWORD WINAPI worker(void*) {
    if (!supported_exe()) { log("ERROR: executable verification failed (unreadable or unsupported SHA-256)"); return 0; }
    auto next = reinterpret_cast<qb_asi::NextThread>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread"));
    if (!next) { log("ERROR: thread enumeration unavailable", GetLastError()); return 0; }
    const auto deadline = GetTickCount64() + 120000;
    while (!engine_ready()) {
        if (GetTickCount64() >= deadline) { log("ERROR: engine signatures not ready (120s)", ERROR_TIMEOUT); return 0; }
        Sleep(100); // Startup polling only. Never a cinematic timer or a Sleep hook.
    }
    DWORD error = QBPrepare(nullptr);
    if (error) { log("ERROR: native preparation failed", error); return 0; }
    qb_asi::InstallDiagnostic diagnostic{};
    unsigned attempts = 0;
    for (unsigned attempt = 0; attempt != 100; ++attempt) {
        attempts = attempt + 1;
        error = qb_asi::install(*QBGetPlan(), next, &diagnostic);
        if (error != ERROR_BUSY) break;
        Sleep(50);
    }
    // install() has resumed all suspended threads. Only report a final failure;
    // successful retries should not add diagnostic noise to a normal launch.
    if (error) {
        char details[384]{};
        sprintf_s(details,
            "ERROR: activation failed; no patch committed; stage=%s win32=%lu ntstatus=0x%08lX thread=%lu attempts=%u",
            diagnostic.stage, diagnostic.win32_error,
            static_cast<unsigned long>(diagnostic.nt_status), diagnostic.thread_id, attempts);
        log(details, error);
    }
    else log("ASI plugin is active.");
    return error;
}
}

// Ultimate ASI Loader calls this export after LoadLibrary. No heavy work in
// DllMain. The module is pinned before creating the worker: hooks never outlive it.
extern "C" __declspec(dllexport) void InitializeASI() {
    if (started.test_and_set()) return;
    log("ASI plugin loaded.");
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&InitializeASI), &pinned)) {
        log("ERROR: module pinning failed", GetLastError()); return;
    }
    HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else log("ERROR: worker creation failed", GetLastError());
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module = instance;
    // /MT may need CRT thread notifications; do not disable them.
    return TRUE;
}
