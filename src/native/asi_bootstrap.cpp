#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "asi_transaction.hpp"
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")
extern "C" DWORD WINAPI QBPrepare(void*);
extern "C" const QBPlan* QBGetPlan();
extern "C" __declspec(dllexport) void InitializeASI();

namespace {
HMODULE module = nullptr;
std::atomic_flag started = ATOMIC_FLAG_INIT;
std::atomic<bool> initialization_completed{false};
CRITICAL_SECTION log_lock{};
bool log_ok = false;
bool told_fallback = false;
DWORD unavailable_primary = 0;
DWORD unavailable_fallback = 0;
constexpr char build_sha[] = "2E3BAF80750F8462469FEE270B6054496BDA73DE541BB19E72AD1EA26DE4715B";
static_assert(sizeof(build_sha) == 65);
constexpr wchar_t log_name[] = L"QuantumBreakCinematicUnlock.log";

struct LogLock {
    LogLock() { EnterCriticalSection(&log_lock); }
    LogLock(const LogLock&) = delete;
    LogLock& operator=(const LogLock&) = delete;
    ~LogLock() { LeaveCriticalSection(&log_lock); }
};
struct FileHandle {
    HANDLE file;
    explicit FileHandle(HANDLE opened) : file(opened) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
struct OpenedLog {
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD primary_error = 0;
    DWORD fallback_error = 0;
    bool fallback = false;
};
struct ExceptionCapture {
    DWORD code = 0;
    void* address = nullptr;
};

void write_raw(HANDLE file, const char* text, int count) {
    if (!text || count <= 0) return;
    DWORD written = 0;
    WriteFile(file, text, DWORD(count), &written, nullptr);
}

void write_prefix(HANDLE file) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char prefix[80];
    int count = sprintf_s(prefix, "[QuantumBreakCinematicUnlock] %04u-%02u-%02u %02u:%02u:%02u | ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    write_raw(file, prefix, count);
}

void write_record(HANDLE file, const char* message, DWORD code) {
    write_prefix(file);
    if (message) write_raw(file, message, int(std::strlen(message)));
    if (code) {
        char suffix[32];
        int count = sprintf_s(suffix, " | code=%lu", code);
        write_raw(file, suffix, count);
    }
    write_raw(file, "\r\n", 2);
}

// Chunk on UTF-16 code units so a long path never needs a second full-size buffer.
void write_utf8(HANDLE file, const wchar_t* text) {
    if (!text) return;
    while (*text) {
        int count = 0;
        while (text[count] && count < 128) ++count;
        if (count > 0 && text[count] && text[count - 1] >= 0xD800 && text[count - 1] <= 0xDBFF) --count;
        if (count <= 0) return;
        char utf8[512];
        int bytes = WideCharToMultiByte(CP_UTF8, 0, text, count, utf8, int(sizeof utf8), nullptr, nullptr);
        if (bytes > 0) write_raw(file, utf8, bytes);
        text += count;
    }
}

void write_module_path(HANDLE file, HMODULE which) {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(which, path, 32768);
    if (!n || n >= 32768) {
        char missing[48];
        int count = sprintf_s(missing, "<unavailable code=%lu>", GetLastError());
        write_raw(file, missing, count);
        return;
    }
    write_utf8(file, path);
}

OpenedLog open_log() {
    OpenedLog opened;
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(module, path, 32768);
    if (!n || n >= 32768) {
        opened.primary_error = GetLastError();
        if (!opened.primary_error) opened.primary_error = ERROR_INSUFFICIENT_BUFFER;
    } else {
        auto slash = std::wcsrchr(path, L'\\');
        if (!slash) opened.primary_error = ERROR_BAD_PATHNAME;
        else if (wcscpy_s(slash + 1, 32768 - (slash + 1 - path), log_name) != 0)
            opened.primary_error = ERROR_FILENAME_EXCED_RANGE;
        else {
            opened.file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (opened.file != INVALID_HANDLE_VALUE) return opened;
            opened.primary_error = GetLastError();
            if (!opened.primary_error) opened.primary_error = ERROR_GEN_FAILURE;
        }
    }
    opened.fallback = true;
    DWORD temp = GetTempPathW(32768, path);
    if (!temp || temp >= 32768) {
        opened.fallback_error = GetLastError();
        if (!opened.fallback_error) opened.fallback_error = ERROR_PATH_NOT_FOUND;
        return opened;
    }
    if (wcscpy_s(path + temp, 32768 - temp, log_name) != 0) {
        opened.fallback_error = ERROR_FILENAME_EXCED_RANGE;
        return opened;
    }
    opened.file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened.file == INVALID_HANDLE_VALUE) {
        opened.fallback_error = GetLastError();
        if (!opened.fallback_error) opened.fallback_error = ERROR_GEN_FAILURE;
    }
    return opened;
}

// Caller holds log_lock. Writes the fallback notice once, before the caller's line.
HANDLE begin_log() {
    OpenedLog opened = open_log();
    if (opened.file == INVALID_HANDLE_VALUE) {
        unavailable_primary = opened.primary_error;
        unavailable_fallback = opened.fallback_error;
        return INVALID_HANDLE_VALUE;
    }
    log_ok = true;
    if (opened.fallback && !told_fallback) {
        write_record(opened.file, "ERROR: log file beside the plugin could not be opened", opened.primary_error);
        told_fallback = true;
    }
    return opened.file;
}

// noinline: LogLock must not be inlined into a function that uses __try (C2712).
__declspec(noinline) void log(const char* message, DWORD code = 0) {
    LogLock lock;
    FileHandle opened{begin_log()};
    if (opened.file == INVALID_HANDLE_VALUE) return;
    write_record(opened.file, message, code);
}

__declspec(noinline) void log_loaded() {
    LogLock lock;
    FileHandle opened{begin_log()};
    if (opened.file == INVALID_HANDLE_VALUE) return;
    write_prefix(opened.file);
    write_raw(opened.file, "ASI plugin loaded. | asi=", int(sizeof("ASI plugin loaded. | asi=") - 1));
    write_module_path(opened.file, module);
    write_raw(opened.file, " | exe=", int(sizeof(" | exe=") - 1));
    write_module_path(opened.file, nullptr);
    write_raw(opened.file, "\r\n", 2);
}

void report_unavailable_log() {
    char text[384];
    sprintf_s(text,
        "Quantum Break Cinematic Unlock could not create its log file.\n"
        "Beside the plugin: error %lu\n"
        "Temp folder: error %lu",
        unavailable_primary, unavailable_fallback);
    MessageBoxA(nullptr, text, "Quantum Break Cinematic Unlock", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
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

// noinline: install() keeps FrozenThreads in this frame, not in worker()'s __try.
__declspec(noinline) DWORD worker_impl() {
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

// Stash only. This runs before unwinding, possibly while other game threads are still suspended.
LONG capture_exception(EXCEPTION_POINTERS* info, ExceptionCapture* out) {
    if (info && info->ExceptionRecord && out) {
        out->code = info->ExceptionRecord->ExceptionCode;
        out->address = info->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool image_offset(void* address, unsigned long long* offset) {
    if (!module || !address || !offset) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(module, &region, sizeof region) || region.State != MEM_COMMIT) return false;
    if (region.RegionSize < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS)) return false;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < LONG(sizeof(IMAGE_DOS_HEADER))) return false;
    auto headers = SIZE_T(dos->e_lfanew);
    if (headers > region.RegionSize - sizeof(IMAGE_NT_HEADERS)) return false;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(module) + headers);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto size = nt->OptionalHeader.SizeOfImage;
    auto start = reinterpret_cast<std::uintptr_t>(module);
    auto addr = reinterpret_cast<std::uintptr_t>(address);
    if (!size || addr < start || addr - start >= size) return false;
    *offset = addr - start;
    return true;
}

void log_exception(const char* stage, const ExceptionCapture& captured) {
    char message[256];
    unsigned long long offset = 0;
    int count;
    if (image_offset(captured.address, &offset)) {
        count = sprintf_s(message,
            "ERROR: exception during %s; code=0x%08lX address=QuantumBreakCinematicUnlock.asi+0x%llX",
            stage, captured.code, offset);
    } else {
        count = sprintf_s(message,
            "ERROR: exception during %s; code=0x%08lX address=0x%llX",
            stage, captured.code, reinterpret_cast<unsigned long long>(captured.address));
    }
    if (count > 0) log(message);
}

DWORD WINAPI worker(void*) {
    ExceptionCapture captured{};
    __try {
        return worker_impl();
    }
    __except (capture_exception(GetExceptionInformation(), &captured)) {
        log_exception("worker", captured);
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

__declspec(noinline) void initialize_impl() {
    log("ASI initialize entered.");
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&InitializeASI), &pinned)) {
        log("ERROR: module pinning failed", GetLastError());
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!thread) {
        log("ERROR: worker creation failed", GetLastError());
        return;
    }
    initialization_completed.store(true, std::memory_order_release);
    CloseHandle(thread);
}
}

// Ultimate ASI Loader calls this export after LoadLibrary. DllMain only records
// the load. The module is pinned before creating the worker: hooks never outlive it.
extern "C" __declspec(dllexport) void InitializeASI() {
    if (started.test_and_set()) return;
    if (!log_ok) report_unavailable_log();
    ExceptionCapture captured{};
    __try {
        initialize_impl();
    }
    __except (capture_exception(GetExceptionInformation(), &captured)) {
        log_exception("InitializeASI", captured);
    }
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance;
        InitializeCriticalSection(&log_lock);
        log_loaded();
    } else if (reason == DLL_PROCESS_DETACH && !reserved
               && !initialization_completed.load(std::memory_order_acquire)) {
        log("ERROR: plugin unloaded before initialization completed");
    }
    // /MT may need CRT thread notifications; do not disable them.
    return TRUE;
}
