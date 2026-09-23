#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include "candidate_module_api.hpp"

namespace qb_asi {
// Resolve before freezing: no loader/CRT locks, heap allocations or logging
// are allowed while other threads are suspended. NtGetNextThread enumerates
// kernel thread objects without a Toolhelp snapshot allocation in that window.
using NextThread = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
struct InstallDiagnostic {
    const char* stage = "not_started";
    DWORD win32_error = 0;
    LONG nt_status = 0;
    DWORD thread_id = 0;
    unsigned frozen = 0;
    unsigned pass = 0;
    std::uintptr_t instruction = 0;
    unsigned patch = 0;
    unsigned terminated_seen = 0;
};
class FrozenThreads {
    HANDLE handles_[1024]{};
    unsigned count_ = 0;
public:
    FrozenThreads() = default;
    FrozenThreads(const FrozenThreads&) = delete;
    ~FrozenThreads() {
        for (unsigned i = count_; i; --i) {
            if (ResumeThread(handles_[i - 1]) == DWORD(-1)
                && WaitForSingleObject(handles_[i - 1], 0) != WAIT_OBJECT_0) {
                // Do not leave a running game with an accidentally frozen thread.
                TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
            }
            CloseHandle(handles_[i - 1]);
        }
    }
    DWORD freeze(NextThread next, const QBPlan& plan, InstallDiagnostic& diagnostic) {
        diagnostic.stage = "resolve_enumerator";
        if (!next) { diagnostic.win32_error = ERROR_PROC_NOT_FOUND; return ERROR_PROC_NOT_FOUND; }
        HANDLE cursor = nullptr;
        const DWORD self = GetCurrentThreadId();
        // Repeat until a pass finds no new threads. Previously suspended
        // threads cannot create more; refuse a continuously changing process.
        for (unsigned pass = 0; pass != 8; ++pass) {
            diagnostic.pass = pass + 1;
            const auto before = count_;
            cursor = nullptr;
            for (;;) {
                HANDLE h = nullptr;
                diagnostic.stage = "enumerate_threads";
                LONG status = next(GetCurrentProcess(), cursor,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                    0, 0, &h);
                // cursor has its own enumeration handle, separate from the
                // retained suspend handles, so it can always be closed here.
                if (cursor) CloseHandle(cursor);
                cursor = h;
                if (status == LONG(0x8000001A)) break; // STATUS_NO_MORE_ENTRIES
                if (status < 0) { diagnostic.nt_status = status; if (h) CloseHandle(h); return ERROR_BUSY; }
                diagnostic.stage = "query_thread_id";
                const DWORD id = GetThreadId(h);
                if (!id) { diagnostic.win32_error = GetLastError(); CloseHandle(h); return ERROR_BUSY; }
                diagnostic.thread_id = id;
                if (id == self) continue;
                // Native enumeration can include terminated thread objects
                // whose handles are still retained. A signaled thread handle
                // proves termination, even if its exit code happens to be 259.
                diagnostic.stage = "check_thread_termination";
                const auto state = WaitForSingleObject(h, 0);
                if (state == WAIT_OBJECT_0) { ++diagnostic.terminated_seen; continue; }
                if (state != WAIT_TIMEOUT) {
                    diagnostic.win32_error = GetLastError(); CloseHandle(h); return ERROR_BUSY;
                }
                bool known = false;
                for (unsigned i = 0; i < count_; ++i)
                    if (GetThreadId(handles_[i]) == id) known = true;
                if (known) continue;
                if (count_ == 1024) { diagnostic.stage = "thread_capacity"; CloseHandle(h); return ERROR_TOO_MANY_TCBS; }
                HANDLE held = nullptr;
                diagnostic.stage = "duplicate_thread_handle";
                if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(),
                                     &held, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    diagnostic.win32_error = GetLastError();
                    CloseHandle(h); return ERROR_BUSY;
                }
                diagnostic.stage = "suspend_thread";
                if (SuspendThread(held) == DWORD(-1)) {
                    const auto suspend_error = GetLastError();
                    if (WaitForSingleObject(held, 0) == WAIT_OBJECT_0) {
                        ++diagnostic.terminated_seen; CloseHandle(held); continue;
                    }
                    diagnostic.win32_error = suspend_error;
                    CloseHandle(held); CloseHandle(h); return ERROR_BUSY;
                }
                handles_[count_++] = held;
                diagnostic.frozen = count_;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                diagnostic.stage = "get_thread_context";
                if (!GetThreadContext(held, &context)) {
                    const auto context_error = GetLastError();
                    if (WaitForSingleObject(held, 0) == WAIT_OBJECT_0) {
                        --count_; diagnostic.frozen = count_;
                        ++diagnostic.terminated_seen; CloseHandle(held); continue;
                    }
                    diagnostic.win32_error = context_error; CloseHandle(h); return ERROR_BUSY;
                }
                for (unsigned i = 0; i < plan.count; ++i) {
                    const auto& p = plan.patches[i];
                    if (context.Rip >= p.address && context.Rip < p.address + p.length) {
                        diagnostic.stage = "instruction_in_patch";
                        diagnostic.instruction = context.Rip;
                        diagnostic.patch = i;
                        CloseHandle(h); return ERROR_BUSY;
                    }
                }
            }
            if (count_ == before) { diagnostic.stage = "threads_frozen"; return ERROR_SUCCESS; }
        }
        diagnostic.stage = "thread_population_changed";
        return ERROR_BUSY;
    }
};

// Called only while all engine threads are frozen. Page protections are saved
// per entry, including entries sharing a page, and restored in reverse order.
inline DWORD commit(const QBPlan& plan, int fail_after = -1) {
    if (plan.version != 1 || plan.count != 4) return ERROR_INVALID_DATA;
    for (unsigned i = 0; i < plan.count; ++i) {
        const auto& p = plan.patches[i];
        if (!p.address || !p.length || p.length > 16) return ERROR_INVALID_DATA;
        BYTE current[16]{}; SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p.address),
                               current, p.length, &read) || read != p.length
            || std::memcmp(current, p.original, p.length)) return ERROR_REVISION_MISMATCH;
    }
    DWORD protections[4]{};
    unsigned writable = 0;
    DWORD error = ERROR_SUCCESS;
    for (; writable < plan.count; ++writable) {
        const auto& p = plan.patches[writable];
        if (!VirtualProtect(reinterpret_cast<void*>(p.address), p.length,
                            PAGE_EXECUTE_READWRITE, &protections[writable])) {
            error = GetLastError(); break;
        }
    }
    unsigned written = 0;
    if (!error) {
        for (; written < plan.count; ++written) {
            const auto& p = plan.patches[written];
            if (int(written) == fail_after) { error = ERROR_WRITE_FAULT; break; }
            std::memcpy(reinterpret_cast<void*>(p.address), p.replacement, p.length);
        }
        for (unsigned i = 0; !error && i < plan.count; ++i) {
            const auto& p = plan.patches[i];
            if (std::memcmp(reinterpret_cast<void*>(p.address), p.replacement, p.length))
                error = ERROR_WRITE_FAULT;
        }
        if (!FlushInstructionCache(GetCurrentProcess(), nullptr, 0)) error = GetLastError();
    }
    if (error) {
        while (written) {
            const auto& p = plan.patches[--written];
            std::memcpy(reinterpret_cast<void*>(p.address), p.original, p.length);
        }
        if (!FlushInstructionCache(GetCurrentProcess(), nullptr, 0))
            TerminateProcess(GetCurrentProcess(), ERROR_WRITE_FAULT);
    }
    while (writable) {
        const auto i = --writable; DWORD ignored = 0;
        const auto& p = plan.patches[i];
        if (!VirtualProtect(reinterpret_cast<void*>(p.address), p.length,
                            protections[i], &ignored)) {
            // Uncertain protection state must never be reported as success.
            TerminateProcess(GetCurrentProcess(), ERROR_WRITE_FAULT);
        }
    }
    return error;
}
inline DWORD install(const QBPlan& plan, NextThread next, InstallDiagnostic* output = nullptr) {
    InstallDiagnostic local{};
    auto& diagnostic = output ? *output : local;
    diagnostic = {};
    if (plan.version != 1 || plan.count != 4) {
        diagnostic.stage = "validate_plan";
        diagnostic.win32_error = ERROR_INVALID_DATA;
        return ERROR_INVALID_DATA;
    }
    FrozenThreads frozen;
    const auto error = frozen.freeze(next, plan, diagnostic);
    if (error) return error;
    diagnostic.stage = "commit_patches";
    const auto result = commit(plan);
    diagnostic.win32_error = result;
    if (!result) diagnostic.stage = "installed";
    return result;
}
}
