#include "../../src/native/candidate_dof_policy.hpp"
#include <cassert>
#include <iostream>
using namespace qb_candidate;
int main() {
    std::size_t cases = 0;
    for (int start : {0, 119, 981, 90000}) {
        for (int length : {1, 2, 55, 1200}) {
            CurveView c{start, length, 2, true, true};
            const float end = static_cast<float>(start + length);
            for (float fraction : {0.0f, 0.01f, 0.5f, 0.999f}) {
                const float f = end - 1 + fraction;
                // Large frame numbers can round the last fraction to end.
                auto expected = f < end ? Decision::defer_end : Decision::original;
                assert(interpolation(c, 1, 0, f) == expected);
                assert(interpolation(c, 2, 0, f) == expected);
                assert(interpolation(c, 3, 0, f) == Decision::original);
                assert(interpolation(c, 3, 2, f) == Decision::suppress_apply);
                assert(interpolation(c, 1, 2, f) == Decision::original);
                ++cases;
            }
            assert(interpolation(c, 1, 0, end) == Decision::original);
            assert(needs_terminal_cleanup(c));
            c.pending_end = false;
            assert(!needs_terminal_cleanup(c));
            assert(interpolation(c, 1, 0, end - 0.5f) == Decision::original);
            c.pending_end = true;
            c.camera_target = false;
            assert(!needs_terminal_cleanup(c));
            assert(interpolation(c, 3, 2, end - 0.5f) == Decision::original);
            c.camera_target = true;
            c.subtype = 1;
            assert(!needs_terminal_cleanup(c));
            assert(interpolation(c, 3, 2, end - 0.5f) == Decision::original);
        }
    }
    std::cout << "Policy checks passed: " << cases << " boundary cases\n";
}
