// =============================================================================
// @file RtpPortAllocator.h
// @brief Thread-safe RTP port allocator for multi-terminal conference support.
//
// [V7.1 Fix 2026-08-13] CRITICAL FIX for multi-terminal join failure.
//
// Background:
//   The MCU used to bind audio/video RTP sockets to a single global port pair
//   (e.g. 4004/4006) and set SO_REUSEADDR=true. When a second terminal joined
//   the same conference, its SimpleRtpTransport re-bound the same ports. With
//   SO_REUSEADDR the kernel silently allowed the second bind, but the OS
//   demultiplexer then split incoming UDP datagrams between the two sockets
//   in an implementation-defined way. The first terminal's audio SSRC
//   started oscillating with the second terminal's SSRC, RTPEngine
//   interpreted it as a "source change" and the second terminal's transport
//   was starved of packets.
//
// Fix:
//   - Disable SO_REUSEADDR so a second bind on the same port fails loudly.
//   - Allocate a fresh audio+video port pair to every inbound INVITE from
//     a free pool (contiguous even numbers, e.g. 4004/4006, 4008/4010, ...).
//   - Return the pair to the pool on call teardown (DISCONNECTED) so other
//     inbound calls can reuse it.
//
// The allocator is a SipSignalingHandler member so it is shared by all
// INVITEs that the handler processes. It is thread-safe (single mutex).
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace vcs { namespace gateway {

/**
 * @brief Thread-safe allocator for audio+video RTP port pairs.
 *
 * Pairs are contiguous even numbers: (base, base+1) for a single call, or
 * (base, base+2) if the secondary-video port is also reserved. The default
 * stride is 2, which matches the existing "audio=4004 video=4006" layout
 * while leaving the slot for a future RTCP port.
 */
class RtpPortAllocator {
public:
    /**
     * @param rangeStart  First port to consider (inclusive). Default 4004.
     * @param rangeEnd    Last port to consider (inclusive).   Default 5098.
     * @param stride      How many ports a single call occupies (>= 2).
     *                    Default 2 (audio + video).
     */
    explicit RtpPortAllocator(uint16_t rangeStart = 4004,
                              uint16_t rangeEnd   = 5098,
                              uint16_t stride     = 2);

    ~RtpPortAllocator() = default;

    // Non-copyable, non-movable (owns a mutex).
    RtpPortAllocator(const RtpPortAllocator&) = delete;
    RtpPortAllocator& operator=(const RtpPortAllocator&) = delete;

    /**
     * @brief Allocate a fresh port pair.
     *
     * @return true on success, with @p audioPort / @p videoPort filled in;
     *         false if the pool is exhausted.
     */
    bool Allocate(uint16_t& audioPort, uint16_t& videoPort);

    /**
     * @brief Return a previously-allocated pair to the pool.
     *
     * Safe to call with a port that is not currently in use (no-op).
     */
    void Release(uint16_t audioPort, uint16_t videoPort);

    /// @return number of free port pairs currently in the pool.
    size_t FreeCount() const;

    /// @return total number of port pairs in the configured range.
    size_t TotalCount() const;

    /// Override the configured range at runtime (clears all allocations).
    void Reset(uint16_t rangeStart, uint16_t rangeEnd, uint16_t stride);

    /// Move the start of the range, keeping the configured end and stride.
    /// Used to keep the pool clear of the port block pjmedia reserves.
    void SetRangeStart(uint16_t rangeStart) { Reset(rangeStart, rangeEnd_, stride_); }

private:
    mutable std::mutex mutex_;
    std::set<uint16_t>   freeAudioPorts_;  // all unallocated audio ports
    uint16_t            rangeStart_  = 4004;
    uint16_t            rangeEnd_    = 5098;
    uint16_t            stride_      = 2;
};

} // namespace gateway
} // namespace vcs
