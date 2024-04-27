#pragma once
#include <algorithm>

namespace uring_features {

#ifndef EXAMPLE_MULTISHOT_CONFIG
inline constexpr bool use_multishot = true;
#else
inline constexpr bool use_multishot = EXAMPLE_MULTISHOT_CONFIG;
#endif

// Multishot can genertae multiple cqe by one sqe.
// In this case, _inflight is no more accurate.
// NOTE: It is still accurate when you are not actullay using multishot.
inline void multishot_workaround_inflight(size_t &inflight, size_t done) {
    if constexpr (use_multishot) {
        inflight = std::max(inflight, done);
    }
}

}
