#pragma once

// Typed media-information snapshot: the single shape in which media
// information leaves the media layer.
//
// Why this lives in common and not in media:
//   - absphone has to answer the legacy GetMediaInformation(MediaInfo, type)
//     query from it, and conference only relays that query, so L4 and L5 must
//     see the same type without including media internals;
//   - it is a copy-out value, never a pointer into live media state. The old
//     void* GetMemberMediaInfo() handed out a raw pointer whose lifetime
//     ended with the session; this type cannot dangle.
//
// Producers exist for every field except the three marked below; keeping the
// slots now means adding those producers later does not change this type or any
// signature on the path to the UI.
//   TODO(rtcp): packetsLost / jitterUs / rttMs are structurally present but
//   still fed 0 - RTPEngine has no RFC3550 jitter estimator and does not parse
//   RTCP receiver reports yet. Do not "fix" them by guessing on the read side.

#include "MediaStreamKind.h"
#include "NegotiatedCodec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vcs {

// One stream (audio / video / secondary video) of one session.
struct MediaStreamSnapshot {
    MediaStreamKind kind = MediaStreamKind::Audio;

    // True while this kind is part of the session's transport set.
    bool active = false;

    // Local SSRC this endpoint sends with (0 for receive-only streams).
    // Needed to correlate this snapshot with a packet capture.
    uint32_t localSsrc = 0;

    // ---- Negotiated descriptor (retained from the remote SDP) ----
    // This is what was agreed on, not what the pipeline guessed: after
    // negotiation the media layer keeps it for the whole call.
    NegotiatedCodec codec;

    // ---- RTP counters, cumulative since the stream started ----
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint32_t packetsSent = 0;
    uint32_t packetsReceived = 0;
    uint32_t packetsLost = 0;
    uint32_t jitterUs = 0;
    uint32_t rttMs = 0;

    // ---- 1 Hz derived values (what the legacy "*InSecond"/bitrate fields mean) ----
    uint32_t packetsSentInSecond = 0;
    uint32_t packetsReceivedInSecond = 0;
    uint32_t packetsLostInSecond = 0;
    uint32_t sendBitrateBps = 0;      // bytes since last tick * 8
    uint32_t receiveBitrateBps = 0;
    float fractionLost = 0.0f;        // percent, over the last tick interval

    // ---- Processing ----
    uint32_t encodedWidth = 0;        // send direction (0 for audio/data)
    uint32_t encodedHeight = 0;
    uint32_t decodedWidth = 0;        // receive direction (0 for audio/data)
    uint32_t decodedHeight = 0;
    uint32_t framesSentPerSecond = 0;
    uint32_t framesDecodedPerSecond = 0;
    uint64_t framesEncoded = 0;       // cumulative
    uint64_t framesDecoded = 0;       // cumulative
    uint64_t framesDropped = 0;       // cumulative

    // ---- Health: the questions asked when "no picture / no sound" ----
    // steady_clock ms of the last inbound packet; 0 = never received.
    int64_t lastPacketAtMs = 0;
    bool firstKeyframeDecoded = false;
    uint64_t firRequests = 0;         // RTCP FIR sent for this stream

    // ---- Addresses ----
    std::string remoteAddress;        // peer media address ("ip:port")
    std::string localAddress;         // local media address ("ip:port"), empty if unknown
};

// Whole-session snapshot: terminal sessions carry at most three streams.
struct MediaInfoSnapshot {
    std::string confId;
    std::string sessionId;
    int64_t sampledAtMs = 0;          // steady_clock ms

    // false when this is the final snapshot replayed after the call ended
    // (legacy keeps the last media info visible after hangup).
    bool live = true;

    std::vector<MediaStreamSnapshot> streams;

    const MediaStreamSnapshot* Find(MediaStreamKind kind) const {
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (streams[i].kind == kind) {
                return &streams[i];
            }
        }
        return nullptr;
    }
};

} // namespace vcs
