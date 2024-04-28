#pragma once
#include <algorithm>

namespace uring_features {

#ifndef EXAMPLE_MULTISHOT_CONFIG
inline constexpr bool use_multishot = true;
#else
inline constexpr bool use_multishot = EXAMPLE_MULTISHOT_CONFIG;
#endif

#ifndef EXAMPLE_SQPOLL_CONFIG
inline constexpr bool use_sqpoll = false;
#else
inline constexpr bool use_sqpoll = EXAMPLE_SQPOLL_CONFIG;
#endif

inline constexpr bool inflight_conflict = use_multishot || use_sqpoll;

// 1. Multishot can genertae multiple cqe by one sqe.
// 2. SQPOLL handles submissions in kernel side.
// In these cases, _inflight is no more accurate.
// NOTE: It is still accurate when you are not actullay using multishot or SQPOLL.
inline void inflight_conflict_workaround(size_t &inflight, size_t done) {
    if constexpr (inflight_conflict) {
        inflight = std::max(inflight, done);
    }
}

}
