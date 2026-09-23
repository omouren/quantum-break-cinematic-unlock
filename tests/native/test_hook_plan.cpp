#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "../../src/native/candidate_hook_plan.hpp"
#include <cassert>
#include <iostream>
using Fn = unsigned long long(*)();
Fn original = nullptr;
int calls = 0;
unsigned long long replacement() { ++calls; return original() + 1; }
int main() {
    using namespace qb_candidate;
    auto memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(memory);
    auto addr = reinterpret_cast<std::uintptr_t>(memory);
    // A synthetic function with EXACTLY the original copied prologue.
    std::memcpy(memory, stop_prefix.data(), stop_prefix.size());
    const std::uint8_t tail[]{0x48,0xB8,0x34,0x12,0,0,0,0,0,0,
        0x48,0x81,0xC4,0x80,0x01,0,0,0x5F,0xC3};
    std::memcpy(memory + 11, tail, sizeof tail);
    auto p = make_hook_plan(addr - stop_rva, addr + 256,
                           reinterpret_cast<std::uintptr_t>(&replacement));
    std::memcpy(memory + 256, p.executable.data(), p.executable.size());
    original = reinterpret_cast<Fn>(memory + 256 + 32);
    RUNTIME_FUNCTION entry{p.function_begin,p.function_end,p.unwind_info};
    assert(RtlAddFunctionTable(&entry, 1, addr + 256));
    DWORD old = 0;
    assert(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old));
    assert(FlushInstructionCache(GetCurrentProcess(), memory, 4096));
    auto target = reinterpret_cast<Fn>(memory);
    assert(target() == 0x1234);
    assert(original() == 0x1234);
    assert(reinterpret_cast<Fn>(memory + 256)() == 0x1235 && calls == 1);

    // Verify Windows consumes the trampoline's unwind metadata correctly.
    alignas(16) std::uint64_t stack[64]{};
    constexpr std::uint64_t saved_rdi = 0x456789;
    constexpr std::uint64_t return_address = 0x12345678;
    stack[48] = saved_rdi; // 0x180 bytes of local allocation
    stack[49] = return_address;
    for (DWORD64 offset : {32ull, 35ull, 36ull, 43ull, 53ull}) {
        CONTEXT context{};
        context.Rip = addr + 256 + offset;
        const int stack_index = offset >= 43 ? 0 : (offset >= 36 ? 48 : 49);
        context.Rsp = reinterpret_cast<DWORD64>(&stack[stack_index]);
        context.Rdi = saved_rdi;
        context.R11 = addr + 11;
        PVOID handler = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, addr + 256, context.Rip,
                        &entry, &context, &handler, &establisher, nullptr);
        assert(context.Rdi == saved_rdi && context.Rip == return_address);
        assert(context.Rsp == reinterpret_cast<DWORD64>(&stack[50]));
    }

    // Install and restore ONLY in this test process's synthetic function.
    assert(VirtualProtect(memory, 4096, PAGE_READWRITE, &old));
    std::memcpy(memory, p.stop_replacement.data(), p.stop_replacement.size());
    assert(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old));
    assert(FlushInstructionCache(GetCurrentProcess(), memory, 4096));
    assert(target() == 0x1235 && calls == 2);
    assert(VirtualProtect(memory, 4096, PAGE_READWRITE, &old));
    std::memcpy(memory, stop_prefix.data(), stop_prefix.size());
    assert(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old));
    assert(FlushInstructionCache(GetCurrentProcess(), memory, 4096));
    assert(target() == 0x1234 && calls == 2);
    assert(RtlDeleteFunctionTable(&entry));
    assert(VirtualFree(memory, 0, MEM_RELEASE));
    bool rejected = false;
    try { (void)make_hook_plan(0x10000000,0x900000000,0x80000000); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::cout << "Native relay, trampoline, unwind and restore passed; no game access\n";
}
