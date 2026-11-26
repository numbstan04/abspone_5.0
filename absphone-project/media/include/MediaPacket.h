/**
 * @file MediaPacket.h
 * @brief Media packet envelope for the async SPSC pipeline (Q1 Section 16)
 *
 * [Design principles]
 * 1. A MediaPacket owns its payload bytes; it is the only object that
 *    crosses the gateway-thread / media-worker-thread boundary. Anything
 *    that is not self-contained (raw pointers, references to gateway-owned
 *    buffers) MUST be copied out before the packet is enqueued.
 * 2. payloadType / ssrc / timestamp / sequenceNumber are decoded from the
 *    RTP header by the producer (gateway thread) so that the consumer
 *    (media worker) can dispatch without re-parsing the header.
 * 3. receivedAt is set on the producer side and is used by the consumer
 *    to compute end-to-end latency for the Q6 observability layer.
 *
 * [References]
 * - Section 16.2 (Q1 core processing loop)
 * - Section 16.4 (Q1 packet envelope choice)
 */

#pragma once

#include "media_export.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace vcs { namespace media {

/**
 * @brief Self-contained media packet carried across the SPSC queue.
 */
struct VCS_MEDIA_API MediaPacket {
    // ---- routing ----
    std::string conferenceId;
    std::string memberId;

    // ---- RTP metadata (decoded by producer) ----
    uint8_t  payloadType     = 0;     // PT (0=PCMU, 8=PCMA, 96+=dynamic)
    uint32_t ssrc            = 0;
    uint64_t timestamp       = 0;     // RTP timestamp
    uint16_t sequenceNumber  = 0;
    bool     markerBit       = false; // M bit: end of an access unit (RFC 3550)

    // ---- payload (deep-copied by producer) ----
    std::vector<uint8_t> payload;

    // ---- diagnostics ----
    std::chrono::steady_clock::time_point receivedAt =
        std::chrono::steady_clock::now();
};

} // namespace media
} // namespace vcs
