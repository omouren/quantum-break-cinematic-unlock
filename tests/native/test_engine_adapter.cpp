#include "../../src/native/candidate_engine_adapter.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <vector>
using namespace qb_candidate;
template<std::size_t N> using Object = std::array<std::byte, N>;
template<class T> void put(void* p, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(p) + offset, &value, sizeof value);
}
constexpr std::uintptr_t TV = 0xB044A8, CV = 0xAB90D8;
std::vector<int> calls;
int mode = 4;
void cleanup(void* e) {
    calls.push_back(10);
    mode = 0;
    put<std::uint8_t>(e, 0x60, 0);
}
void interpolate_original(void* e, int pass, float f) {
    calls.push_back(pass);
    if (pass == 0 && field<std::uint8_t>(e, 0x60)) cleanup(e);
    const int start = field<int>(e, 8), length = field<int>(e, 12);
    if (pass == 2 && f >= start && f < start + length) mode = 4;
}
int get_type(void* p) { return field<int>(p, 0); }
void original_stop(void* t) {
    calls.push_back(20);
    put<int>(t, 0x1D8, 3);
    auto tracks = field<void**>(t, 0x700);
    for (std::uint64_t i = 0; i < field<std::uint64_t>(t, 0x708); ++i) {
        auto events = field<void**>(tracks[i], 0);
        for (std::uint64_t j = 0; j < field<std::uint64_t>(tracks[i], 8); ++j) {
            auto e = events[j];
            if (field<std::uint8_t>(e, 0x30)) {
                put<std::uint8_t>(e, 0x30, 0);
                put<std::uint8_t>(e, 0x60, 1);
            }
        }
    }
}
int main() {
    Object<0x780> timeline{};
    Object<0x48> track{};
    Object<0x68> curve{}, unrelated{};
    Object<8> target{};
    void* events[] = {curve.data(), unrelated.data()};
    void* tracks[] = {track.data()};
    put(timeline.data(), 0, TV);
    put<int>(timeline.data(), 0x1D8, 1);
    put<void**>(timeline.data(), 0x700, tracks);
    put<std::uint64_t>(timeline.data(), 0x708, 1);
    put<std::uint8_t>(timeline.data(), 0x774, 1);
    put<void**>(track.data(), 0, events);
    put<std::uint64_t>(track.data(), 8, 2);
    put<void*>(track.data(), 0x28, target.data());
    put<void*>(track.data(), 0x40, timeline.data());
    put<int>(target.data(), 0, 0x2B);
    put(curve.data(), 0, CV);
    put<int>(curve.data(), 8, 981);
    put<int>(curve.data(), 12, 55);
    put<void*>(curve.data(), 0x28, track.data());
    put<int>(curve.data(), 0x38, 2);
    put<std::uint8_t>(curve.data(), 0x60, 1);
    Adapter adapter({TV, CV, interpolate_original, cleanup, original_stop, get_type});
    adapter.interpolate(curve.data(), 0, 1035.5f);
    assert(calls.empty());
    adapter.interpolate(curve.data(), 2, 1035.5f);
    assert(mode == 4 && field<std::uint8_t>(curve.data(), 0x60) == 1);
    adapter.interpolate(curve.data(), 0, 1036.f);
    assert(mode == 0 && field<std::uint8_t>(curve.data(), 0x60) == 0);

    // Stop while active; no subsequent interpolation is needed for cleanup.
    calls.clear();
    mode = 4;
    put<std::uint8_t>(curve.data(), 0x30, 1);
    adapter.stop(timeline.data());
    assert((calls == std::vector<int>{20, 10}));
    assert(mode == 0 && field<std::uint8_t>(curve.data(), 0x60) == 0);
    adapter.interpolate(curve.data(), 2, 1000.f);
    assert((calls == std::vector<int>{20, 10}) && mode == 0);

    // A pending end survives even if the event was already deactivated.
    calls.clear();
    put<std::uint8_t>(curve.data(), 0x60, 1);
    adapter.stop(timeline.data());
    assert((calls == std::vector<int>{20, 10}));

    // Pass-through on a different event class, including at terminal state.
    calls.clear();
    adapter.interpolate(unrelated.data(), 2, 1000.f);
    assert((calls == std::vector<int>{2}));

    // No writes to the forced request flag, no scene-specific threshold.
    assert(!adapter.request_forced_mode(timeline.data()));
    put<std::uint8_t>(timeline.data(), 0x6F8, 1);
    assert(!adapter.request_forced_mode(timeline.data()));
    assert(field<std::uint8_t>(timeline.data(), 0x774) == 1);
    put<std::uintptr_t>(timeline.data(), 0, 0x1234);
    assert(adapter.request_forced_mode(timeline.data()));
    put(timeline.data(), 0, TV);
    put<std::uint8_t>(timeline.data(), 0x774, 0);
    put<std::uint8_t>(timeline.data(), 0x6F8, 0);
    assert(!adapter.request_forced_mode(timeline.data()));
    std::cout << "Adapter integration fixture passed (offline, no game access)\n";
}
