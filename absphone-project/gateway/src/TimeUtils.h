/**
 * @file TimeUtils.h
 * @brief Shared time utility functions for the gateway module
 *
 * Provides high-resolution monotonic timestamp utilities that are
 * cross-platform and dependency-free. Consolidates duplicated
 * `GetCurrentTimeMs` definitions that previously existed in
 * `SimpleRtpTransport.cpp` and other translation units.
 *
 * Design notes:
 *  - Uses `std::chrono::steady_clock` for monotonic, non-decreasing time
 *    (immune to wall-clock changes such as NTP corrections).
 *  - Returned value is millisecond resolution.
 *  - Returned type is signed (`int64_t`) to safely interop with the
 *    `CallSession::startTime / connectTime / lastActiveTime` fields.
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace vcs { namespace gateway {

/**
 * @brief Get current monotonic time in milliseconds
 *
 * Returns the number of milliseconds since the steady_clock epoch
 * (implementation-defined, typically system boot or arbitrary fixed
 * point - do NOT interpret as wall-clock time).
 *
 * Thread-safe. No external dependencies.
 *
 * @return int64_t Current monotonic time in milliseconds
 */
inline int64_t GetCurrentTimeMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/**
 * @brief Unsigned variant of GetCurrentTimeMs for callers that need uint64_t
 *
 * Convenience for code paths that already use `uint64_t` for stats
 * (e.g., `SimpleRtpTransport::Statistics::lastSendTime`). For
 * difference calculations involving signed fields, prefer the
 * signed `GetCurrentTimeMs()`.
 *
 * @return uint64_t Current monotonic time in milliseconds
 */
inline uint64_t GetCurrentTimeMsU() {
    return static_cast<uint64_t>(GetCurrentTimeMs());
}

} // namespace gateway
} // namespace vcs
