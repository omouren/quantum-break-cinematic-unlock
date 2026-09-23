#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cassert>
#include <cstdio>
#include "../../src/native/asi_transaction.hpp"

DWORD WINAPI waiting_thread(void* event) {
    return WaitForSingleObject(static_cast<HANDLE>(event), 10000);
}
LONG NTAPI denied_enumeration(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE output) {
    *output = nullptr;
    return LONG(0xC0000022); // STATUS_ACCESS_DENIED
}
DWORD fixture_thread_id = 0;
DWORD fixture_access = THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
DWORD WINAPI finished_thread(void*) { return STILL_ACTIVE; } // Valid exit code, not a liveness test.
LONG NTAPI restricted_thread(HANDLE, HANDLE cursor, ACCESS_MASK, ULONG, ULONG, PHANDLE output) {
    *output = nullptr;
    if (cursor) return LONG(0x8000001A);
    // Intentionally omit GET_CONTEXT to reproduce a failure after suspension.
    *output = OpenThread(fixture_access, FALSE, fixture_thread_id);
    return *output ? 0 : LONG(0xC0000022);
}
int main() {
    auto page = static_cast<BYTE*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(page);
    QBPlan plan{}; plan.version = 1; plan.count = 4;
    for (unsigned i = 0; i < 4; ++i) {
        auto& p = plan.patches[i]; p.address = reinterpret_cast<std::uintptr_t>(page + i * 16);
        p.length = i == 1 ? 11 : 1;
        for (unsigned j = 0; j < p.length; ++j) {
            p.original[j] = BYTE(0x40 + i); p.replacement[j] = BYTE(0x90 + i);
        }
        std::memcpy(reinterpret_cast<void*>(p.address), p.original, p.length);
    }
    DWORD old = 0; assert(VirtualProtect(page, 8192, PAGE_EXECUTE_READ, &old));
    auto check = [&](bool changed) {
        for (const auto& p : plan.patches)
            assert(!std::memcmp(reinterpret_cast<void*>(p.address),
                                changed ? p.replacement : p.original, p.length));
        MEMORY_BASIC_INFORMATION mbi{};
        assert(VirtualQuery(page, &mbi, sizeof mbi));
        assert(mbi.Protect == PAGE_EXECUTE_READ);
    };
    for (int fail = 0; fail < 4; ++fail) {
        assert(qb_asi::commit(plan, fail) == ERROR_WRITE_FAULT); check(false);
    }
    auto bad = plan; bad.patches[2].original[0] ^= 1;
    assert(qb_asi::commit(bad) == ERROR_REVISION_MISMATCH); check(false);
    bad = plan; bad.patches[3].address = reinterpret_cast<std::uintptr_t>(page + 4096);
    assert(VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &old));
    assert(qb_asi::commit(bad) == ERROR_REVISION_MISMATCH); check(false);
    assert(VirtualProtect(page + 4096, 4096, PAGE_EXECUTE_READ, &old));
    auto next = reinterpret_cast<qb_asi::NextThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread"));
    assert(next);
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr); assert(event);
    HANDLE thread = CreateThread(nullptr, 0, waiting_thread, event, 0, nullptr); assert(thread);
    fixture_thread_id = GetThreadId(thread);
    qb_asi::InstallDiagnostic diagnostic{};
    assert(qb_asi::install(plan, denied_enumeration, &diagnostic) == ERROR_BUSY);
    assert(!std::strcmp(diagnostic.stage, "enumerate_threads"));
    assert(diagnostic.nt_status == LONG(0xC0000022) && diagnostic.frozen == 0);
    check(false);
    assert(qb_asi::install(plan, restricted_thread, &diagnostic) == ERROR_BUSY);
    assert(!std::strcmp(diagnostic.stage, "get_thread_context"));
    assert(diagnostic.win32_error == ERROR_ACCESS_DENIED);
    assert(diagnostic.thread_id == fixture_thread_id && diagnostic.frozen == 1);
    assert(SuspendThread(thread) == 0); // Failed installation resumed the thread.
    assert(ResumeThread(thread) == 1);
    check(false);
    fixture_access = THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
    assert(qb_asi::install(plan, restricted_thread, &diagnostic) == ERROR_BUSY);
    assert(!std::strcmp(diagnostic.stage, "suspend_thread"));
    assert(diagnostic.win32_error == ERROR_ACCESS_DENIED && diagnostic.terminated_seen == 0);
    check(false); // An inaccessible live thread must still block installation.
    HANDLE finished = CreateThread(nullptr, 0, finished_thread, nullptr, 0, nullptr); assert(finished);
    assert(WaitForSingleObject(finished, 2000) == WAIT_OBJECT_0);
    DWORD finished_code = 0; assert(GetExitCodeThread(finished, &finished_code));
    assert(finished_code == STILL_ACTIVE);
    // Reproduce the original refusal, with a real retained terminated thread.
    assert(SuspendThread(finished) == DWORD(-1)); assert(GetLastError() == ERROR_ACCESS_DENIED);
    assert(qb_asi::install(plan, next, &diagnostic) == ERROR_SUCCESS); check(true);
    assert(diagnostic.terminated_seen > 0);
    assert(qb_asi::install(plan, next) == ERROR_REVISION_MISMATCH); check(true);
    QBPlan reverse = plan;
    for (auto& p : reverse.patches) {
        BYTE temp[16]; std::memcpy(temp, p.original, 16);
        std::memcpy(p.original, p.replacement, 16); std::memcpy(p.replacement, temp, 16);
    }
    assert(qb_asi::install(reverse, next) == ERROR_SUCCESS); check(false);
    assert(SetEvent(event)); assert(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    DWORD result = 999; assert(GetExitCodeThread(thread, &result)); assert(result == WAIT_OBJECT_0);
    CloseHandle(finished); CloseHandle(thread); CloseHandle(event); VirtualFree(page, 0, MEM_RELEASE);
    std::puts("ASI transaction: commit, rollback at all four writes, mismatch, unreadable page, shared-page protections and thread resume OK");
    std::puts("Diagnostics: native enumeration denial and context access failure identified; no writes and thread resumed OK");
    std::puts("Retained terminated thread: access-denied suspension reproduced, signaled handle safely skipped; live denied thread still blocks OK");
}
