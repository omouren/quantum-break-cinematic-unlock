// Build-specific engine adapter, not a loader or installed patch.
// Call only on the engine thread with live objects supplied by the engine.
// Validation of the executable and hook installation is the loader's job.
#pragma once
#include "candidate_dof_policy.hpp"
#include <cstddef>
#include <cstring>

namespace qb_candidate {
template<class T> T field(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}

struct Engine {
    std::uintptr_t timeline_vtable;
    std::uintptr_t curve_vtable;
    // Original implementations, not their patched entry points.
    void (*interpolate_curve)(void*, int, float); // 0x147930
    void (*cleanup_curve)(void*);                // 0x147810
    void (*stop_timeline)(void*);                // 0x2DBE40
    int (*target_type)(void*);                   // target virtual slot +0x190
};

class Adapter {
    Engine engine;

    bool describe(void* event, void*& timeline, CurveView& curve) const noexcept {
        if (!event || field<std::uintptr_t>(event, 0) != engine.curve_vtable)
            return false;
        auto track = field<void*>(event, 0x28);
        if (!track) return false;
        timeline = field<void*>(track, 0x40);
        if (!timeline || field<std::uintptr_t>(timeline, 0) != engine.timeline_vtable)
            return false;
        auto target = field<void*>(track, 0x28);
        if (!target) return false;
        curve = {field<std::int32_t>(event, 0x8),
                 field<std::int32_t>(event, 0xC),
                 field<std::int32_t>(event, 0x38),
                 field<std::uint8_t>(event, 0x60) != 0,
                 engine.target_type(target) == 0x2B};
        return true;
    }

public:
    explicit Adapter(Engine e) noexcept : engine(e) {}

    // Hook body for CurveTimelineEvent's interpolated callback.
    // No clocks, renderer fields, allocations or external-thread writes.
    void interpolate(void* event, int pass, float frame) const {
        void* timeline = nullptr;
        CurveView curve{};
        if (describe(event, timeline, curve)) {
            const auto decision = interpolation(curve,
                field<std::int32_t>(timeline, 0x1D8), pass, frame);
            if (decision != Decision::original) return;
        }
        engine.interpolate_curve(event, pass, frame);
    }

    // Must wrap 0x2DBE40, before its caller dispatches OnTimelineEnd/Skipped.
    // The original stop routine retains runtime tracks in the inspected build.
    // No iteration state or event pointer is retained across calls to stop.
    void stop(void* timeline) const {
        engine.stop_timeline(timeline);
        if (field<std::uintptr_t>(timeline, 0) != engine.timeline_vtable) return;
        auto tracks = field<void**>(timeline, 0x700);
        auto count = field<std::uint64_t>(timeline, 0x708);
        for (std::uint64_t i = 0; i < count; ++i) {
            void* track = tracks[i];
            if (!track || field<void*>(track, 0x40) != timeline) continue;
            auto events = field<void**>(track, 0);
            auto event_count = field<std::uint64_t>(track, 0x8);
            for (std::uint64_t j = 0; j < event_count; ++j) {
                void* owner = nullptr;
                CurveView curve{};
                if (describe(events[j], owner, curve) && owner == timeline
                    && needs_terminal_cleanup(curve))
                    engine.cleanup_curve(events[j]);
            }
        }
    }

    // Decision for both forced-mode renewal sites. Installation must preserve
    // their original branches and register state. Other forced-mode sources
    // remain untouched. +0x774 counts camera events including nested timelines;
    // +0x6F8 is deliberately NOT a gate (PlaySkipable does not set it).
    bool request_forced_mode(void* timeline) const noexcept {
        const bool requested = field<std::uint8_t>(timeline, 0x774) != 0;
        if (field<std::uintptr_t>(timeline, 0) != engine.timeline_vtable)
            return requested;
        return false;
    }
};
}
