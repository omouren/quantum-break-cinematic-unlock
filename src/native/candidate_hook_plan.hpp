// Pure construction of runtime patch bytes. No process access or installation.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace qb_candidate {
inline constexpr std::uint32_t stop_rva = 0x2DBE40;
inline constexpr std::uint32_t curve_slot_rva = 0xAB9158;
inline constexpr std::uint32_t curve_original_rva = 0x147930;
inline constexpr std::array<std::uint32_t, 2> cap_branch_rvas{0x2DACB8, 0x2DBCC6};
inline constexpr std::array<std::uint8_t, 11> stop_prefix{
    0x48,0x8B,0xC4, // mov rax,rsp
    0x57,           // push rdi
    0x48,0x81,0xEC,0x80,0x01,0x00,0x00 // sub rsp,0x180
};

struct HookPlan {
    // Offset 0: relay to replacement. Offset 32: original-function trampoline.
    // Offset 64: UNWIND_INFO for the copied original prologue.
    std::array<std::uint8_t, 76> executable{};
    std::array<std::uint8_t, 11> stop_replacement{};
    std::uint32_t function_begin = 32;
    std::uint32_t function_end = 56;
    std::uint32_t unwind_info = 64;
};

inline void absolute_jump(std::uint8_t* out, std::uintptr_t target) noexcept {
    // jmp qword ptr [rip+0]; does not clobber RAX or any argument register.
    out[0] = 0xFF; out[1] = 0x25;
    std::memset(out + 2, 0, 4);
    std::memcpy(out + 6, &target, 8);
}

inline HookPlan make_hook_plan(std::uintptr_t module, std::uintptr_t allocation,
                               std::uintptr_t replacement_stop) {
    static_assert(sizeof(std::uintptr_t) == 8, "x64 only");
    const auto next = module + stop_rva + 5;
    const auto distance = static_cast<std::int64_t>(allocation)
                        - static_cast<std::int64_t>(next);
    if (distance < (std::numeric_limits<std::int32_t>::min)()
        || distance > (std::numeric_limits<std::int32_t>::max)())
        throw std::invalid_argument("Stop relay must be within rel32 reach");
    HookPlan p;
    absolute_jump(p.executable.data(), replacement_stop);
    std::memcpy(p.executable.data() + 32, stop_prefix.data(), stop_prefix.size());
    // Do not use FF 25 here: Windows recognizes it as an epilogue jump and
    // would skip the unwind codes although the original frame is still live.
    // R11 is volatile and unused by the copied prologue/entry contract.
    p.executable[43] = 0x49; p.executable[44] = 0xBB; // mov r11,imm64
    const auto resume = module + stop_rva + 11;
    std::memcpy(p.executable.data() + 45, &resume, 8);
    p.executable[53] = 0x41; p.executable[54] = 0xFF;
    p.executable[55] = 0xE3; // jmp r11 (not a mod=00 epilogue jump)
    // Version 1, prologue length 11, three unwind slots, no frame register.
    // UWOP_ALLOC_LARGE: size 0x180 / 8 = 0x30, then UWOP_PUSH_NONVOL RDI.
    const std::uint8_t unwind[]{1,11,3,0, 11,1,0x30,0, 4,0x70,0,0};
    std::memcpy(p.executable.data() + 64, unwind, sizeof unwind);
    p.stop_replacement.fill(0x90);
    p.stop_replacement[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(distance);
    std::memcpy(p.stop_replacement.data() + 1, &rel, 4);
    return p;
}
}
