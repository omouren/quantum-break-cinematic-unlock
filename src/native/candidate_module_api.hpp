#pragma once
#include <cstdint>
struct QBPatch {
    std::uint64_t address;
    std::uint32_t length;
    std::uint8_t original[16];
    std::uint8_t replacement[16];
};
struct QBPlan {
    std::uint32_t version;
    std::uint32_t count;
    std::uint64_t image;
    std::uint64_t relay;
    QBPatch patches[4];
};
static_assert(sizeof(QBPatch) == 48);
static_assert(sizeof(QBPlan) == 216);
// QBPrepare(NULL): prepare for the current executable, without installing.
// QBGetPlan(): immutable plan after successful preparation, otherwise nullptr.
// QBCopyPlan(buffer): copies 216 bytes into a caller-owned buffer in this
// process and returns a DWORD status; suitable for a remote-thread entry.
// The prepared module/allocation must remain loaded until process exit.
// Installation/removal requires a separate coordinated transaction.
