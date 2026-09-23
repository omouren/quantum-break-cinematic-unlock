#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <new>
#include "candidate_engine_adapter.hpp"
#include "candidate_hook_plan.hpp"
#include "candidate_module_api.hpp"

namespace {
using namespace qb_candidate;
QBPlan plan{};
RUNTIME_FUNCTION runtime_entry{};
std::atomic<bool> ready{false};
std::atomic_flag preparing = ATOMIC_FLAG_INIT;
alignas(Adapter) std::byte adapter_storage[sizeof(Adapter)];
Adapter* adapter = nullptr;

int target_type(void* target) {
    auto vtable = field<std::uintptr_t>(target, 0);
    auto fn = field<int(*)(void*)>(reinterpret_cast<void*>(vtable), 0x190);
    return fn(target);
}
void curve_hook(void* event, int pass, float frame) {
    adapter->interpolate(event, pass, frame);
}
void stop_hook(void* timeline) { adapter->stop(timeline); }

bool same(std::uintptr_t image, std::uint32_t rva, const void* bytes, std::size_t n) {
    return std::memcmp(reinterpret_cast<void*>(image + rva), bytes, n) == 0;
}

// The allocation belongs to this process. Search by Windows allocation
// granularity; VirtualAlloc returns nullptr for occupied addresses.
void* allocate_near(std::uintptr_t target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto step = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto center = target & ~(step - 1);
    for (std::uintptr_t delta = step; delta < 0x7FFF0000; delta += step) {
        if (center > delta) {
            if (auto p = VirtualAlloc(reinterpret_cast<void*>(center - delta),
                4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
        }
        if (auto p = VirtualAlloc(reinterpret_cast<void*>(center + delta),
            4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
    }
    return nullptr;
}

DWORD prepare(std::uintptr_t image) {
    if (ready.load(std::memory_order_acquire))
        return plan.image == image ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
    if (preparing.test_and_set()) return ERROR_BUSY;
    struct Unlock { ~Unlock() { preparing.clear(); } } unlock;
    if (ready.load(std::memory_order_acquire))
        return plan.image == image ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0
        || dos->e_lfanew > 0x1000) return ERROR_BAD_EXE_FORMAT;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.SizeOfImage != 0x1316000) return ERROR_BAD_EXE_FORMAT;
    const std::uint8_t gate[]{0x80,0xB9,0x74,0x07,0,0,0,0x74,0x11};
    const std::uint8_t callback[]{0x83,0xFA,0x02,0x75,0x5B};
    const std::uint8_t cleanup[]{0x40,0x53,0x48,0x83,0xEC,0x30,0x83,0x79,0x38,0x02};
    if (!same(image, 0x2DACB1, gate, sizeof gate)
        || !same(image, 0x2DBCBF, gate, sizeof gate)
        || !same(image, stop_rva, stop_prefix.data(), stop_prefix.size())
        || !same(image, curve_original_rva, callback, sizeof callback)
        || !same(image, 0x147810, cleanup, sizeof cleanup)
        || field<std::uintptr_t>(reinterpret_cast<void*>(image + curve_slot_rva),0)
             != image + curve_original_rva) return ERROR_REVISION_MISMATCH;
    auto memory = static_cast<std::uint8_t*>(allocate_near(image + stop_rva));
    if (!memory) return ERROR_NOT_ENOUGH_MEMORY;
    auto hook = make_hook_plan(image, reinterpret_cast<std::uintptr_t>(memory),
                              reinterpret_cast<std::uintptr_t>(&stop_hook));
    std::memcpy(memory, hook.executable.data(), hook.executable.size());
    DWORD old = 0;
    if (!VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old)
        || !FlushInstructionCache(GetCurrentProcess(), memory, 4096)) {
        auto error = GetLastError(); VirtualFree(memory, 0, MEM_RELEASE); return error;
    }
    runtime_entry = {hook.function_begin, hook.function_end, hook.unwind_info};
    if (!RtlAddFunctionTable(&runtime_entry,1,reinterpret_cast<DWORD64>(memory))) {
        VirtualFree(memory,0,MEM_RELEASE); return ERROR_INVALID_FUNCTION;
    }
    adapter = new(adapter_storage) Adapter({image + 0xB044A8, image + 0xAB90D8,
        reinterpret_cast<void(*)(void*,int,float)>(image + curve_original_rva),
        reinterpret_cast<void(*)(void*)>(image + 0x147810),
        reinterpret_cast<void(*)(void*)>(memory + 32), target_type});
    plan.version = 1; plan.count = 4; plan.image = image;
    plan.relay = reinterpret_cast<std::uint64_t>(memory);
    // Order: DOF callback and stop cleanup first; uncap last. A loader must
    // nevertheless commit all entries while engine threads are quiescent.
    auto add = [&](int i, std::uint32_t rva, const void* replacement, std::uint32_t n) {
        auto& p = plan.patches[i]; p.address = image + rva; p.length = n;
        std::memcpy(p.original, reinterpret_cast<void*>(p.address), n);
        std::memcpy(p.replacement, replacement, n);
    };
    const auto curve = reinterpret_cast<std::uintptr_t>(&curve_hook);
    add(0,curve_slot_rva,&curve,8);
    add(1,stop_rva,hook.stop_replacement.data(),11);
    const std::uint8_t jump = 0xEB;
    add(2,cap_branch_rvas[0],&jump,1);
    add(3,cap_branch_rvas[1],&jump,1);
    ready.store(true,std::memory_order_release);
    return ERROR_SUCCESS;
}
}

extern "C" __declspec(dllexport) DWORD WINAPI QBPrepare(void*) {
    return prepare(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}
extern "C" __declspec(dllexport) const QBPlan* QBGetPlan() {
    return ready.load(std::memory_order_acquire) ? &plan : nullptr;
}
extern "C" __declspec(dllexport) DWORD WINAPI QBCopyPlan(void* destination) {
    if (!destination) return ERROR_INVALID_PARAMETER;
    if (!ready.load(std::memory_order_acquire)) return ERROR_NOT_READY;
    std::memcpy(destination,&plan,sizeof plan);
    return ERROR_SUCCESS;
}
#ifdef QB_MODULE_TEST
extern "C" __declspec(dllexport) DWORD QBPrepareTest(std::uintptr_t image) { return prepare(image); }
#elif !defined(QB_ASI)
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
#endif
