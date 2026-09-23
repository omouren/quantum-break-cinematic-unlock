#define QB_MODULE_TEST
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../src/native/asi_transaction.hpp"
#include "../../src/native/candidate_module.cpp"
#include <cassert>
#include <cstdio>
int main() {
    auto image = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x1316000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(image);
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    assert(QBPrepareTest(base) == ERROR_BAD_EXE_FORMAT);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = 0x1316000;
    assert(QBPrepareTest(base) == ERROR_REVISION_MISMATCH);
    const BYTE gate[]{0x80,0xB9,0x74,0x07,0,0,0,0x74,0x11};
    const BYTE callback[]{0x83,0xFA,0x02,0x75,0x5B};
    const BYTE cleanup[]{0x40,0x53,0x48,0x83,0xEC,0x30,0x83,0x79,0x38,0x02};
    std::memcpy(image+0x2DACB1,gate,sizeof gate); std::memcpy(image+0x2DBCBF,gate,sizeof gate);
    std::memcpy(image+0x2DBE40,qb_candidate::stop_prefix.data(),11);
    std::memcpy(image+0x147930,callback,sizeof callback); std::memcpy(image+0x147810,cleanup,sizeof cleanup);
    const auto pointer = base+0x147930; std::memcpy(image+0xAB9158,&pointer,8);
    DWORD old = 0; assert(VirtualProtect(image,0x1316000,PAGE_EXECUTE_READ,&old));
    assert(QBPrepareTest(base) == 0); assert(QBPrepareTest(base) == 0);
    const auto* native_plan = QBGetPlan(); assert(native_plan && native_plan->count == 4 && native_plan->image == base);
    DWORD64 unwind_base = 0;
    assert(RtlLookupFunctionEntry(native_plan->relay+43,&unwind_base,nullptr));
    assert(unwind_base == native_plan->relay);
    auto next = reinterpret_cast<qb_asi::NextThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread"));
    assert(qb_asi::install(*native_plan,next) == 0);
    for (const auto& p : native_plan->patches)
        assert(!std::memcmp(reinterpret_cast<void*>(p.address),p.replacement,p.length));
    std::puts("ASI native module: synthetic image, build/signature guards, prepare, unwind registration and four installed patches OK");
    // Production retains the adapter/relay until process exit; so does this test.
}
