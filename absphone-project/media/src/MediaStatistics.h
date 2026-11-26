#pragma once

// Per-session media statistics: the single writer-side store behind
// MediaInfoSnapshot.
//
// Threading model
//   - AddReceived/AddSent come straight off the media hot path (RTP ingress on
//     MediaWorker, RTP egress on the encoder threads) and only touch per-stream
//     atomics, so several producers may call them concurrently.
//   - Everything else (session shape, negotiated codec, the 1 Hz Tick, the
//     processor counters pushed by MediaControl) runs under one mutex. There
//     is exactly one ticker per resource: MediaWorker's thread.
//   - GetSnapshot copies under that same mutex; the caller never receives a
//     pointer into live state.
//
// This replaces the previous member-level collector whose counters had no
// reader and whose video/audio numbers could not be told apart by stream.

#include "media_export.h"
#include "MediaInfoSnapshot.h"
#include "MediaStreamKind.h"
#include "NegotiatedCodec.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vcs { namespace media {

class MediaStatisticsCollector {
public:
    // Hot-path handle for one (member, stream kind).
    //
    // Resolving the member under Impl::mutex on every packet put the
    // control-plane lock (1 Hz tick, GetSnapshot, codec/address updates) on
    // the RTP ingress/egress path: a snapshot copied on another thread could
    // stall pjmedia's ioqueue and freeze that transport. A handle is resolved
    // once at session setup, after which the per-packet path is atomics only.
    //
    // The handle owns a reference to the member's counter block, so packets
    // still in flight while RemoveMember() runs increment an orphan block
    // instead of touching freed memory.
    class StreamCounter {
    public:
        VCS_MEDIA_API StreamCounter();
        VCS_MEDIA_API ~StreamCounter();

        StreamCounter(const StreamCounter&) = delete;
        StreamCounter& operator=(const StreamCounter&) = delete;

        VCS_MEDIA_API void AddReceived(uint64_t bytes, uint64_t packets,
                                        uint64_t lost, uint64_t jitterUs) const;
        VCS_MEDIA_API void AddSent(uint64_t bytes, uint64_t packets) const;

    private:
        friend class MediaStatisticsCollector;
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };

    VCS_MEDIA_API MediaStatisticsCollector();
    VCS_MEDIA_API ~MediaStatisticsCollector();

    MediaStatisticsCollector(const MediaStatisticsCollector&) = delete;
    MediaStatisticsCollector& operator=(const MediaStatisticsCollector&) = delete;

    // Returns null when the member is not registered yet (callers keep using
    // AddReceived/AddSent as the slow fallback). Never blocks on the hot path.
    VCS_MEDIA_API std::shared_ptr<StreamCounter> GetStreamCounter(
        const std::string& memberId, vcs::MediaStreamKind kind);

    // ---- Session lifetime ----
    VCS_MEDIA_API void AddMember(const std::string& memberId);
    VCS_MEDIA_API void RemoveMember(const std::string& memberId);
    VCS_MEDIA_API void ClearAll();
    VCS_MEDIA_API bool HasMember(const std::string& memberId) const;

    // ---- Session shape (stream set, addresses, SSRC) ----
    VCS_MEDIA_API void SetStreamActive(const std::string& memberId,
                                        vcs::MediaStreamKind kind,
                                        bool active,
                                        uint32_t localSsrc,
                                        const std::string& remoteAddress,
                                        const std::string& localAddress);

    // ---- Negotiation result: retained for the whole call ----
    VCS_MEDIA_API void SetNegotiatedCodec(const std::string& memberId,
                                           vcs::MediaStreamKind kind,
                                           const vcs::NegotiatedCodec& codec);

    // ---- RTP hot path (per packet) ----
    VCS_MEDIA_API void AddReceived(const std::string& memberId,
                                    vcs::MediaStreamKind kind,
                                    uint64_t bytes, uint64_t packets,
                                    uint64_t lost, uint64_t jitterUs);
    VCS_MEDIA_API void AddSent(const std::string& memberId,
                                vcs::MediaStreamKind kind,
                                uint64_t bytes, uint64_t packets);

    // ---- Processing counters (cumulative) ----
    // Pushed once per second by MediaControl from VideoProcessor's report; the
    // tick turns the cumulative numbers into per-second rates.
    VCS_MEDIA_API void UpdateVideoProcess(const std::string& memberId,
                                           vcs::MediaStreamKind kind,
                                           uint32_t encodedWidth, uint32_t encodedHeight,
                                           uint32_t decodedWidth, uint32_t decodedHeight,
                                           uint64_t framesEncoded, uint64_t framesDecoded,
                                           uint64_t framesDropped, uint64_t firRequests,
                                           bool firstKeyframeDecoded);
    VCS_MEDIA_API void UpdateAudioProcess(const std::string& memberId,
                                           uint64_t framesEncoded, uint64_t framesDecoded);

    // ---- 1 Hz tick ----
    // Turns cumulative counters into the per-second values (bitrate, packet
    // deltas, frame rates, fraction lost). Called once per second from the
    // resource's MediaWorker thread.
    VCS_MEDIA_API void Tick(const std::string& memberId);

    // ---- Query ----
    VCS_MEDIA_API bool GetSnapshot(const std::string& memberId,
                                    vcs::MediaInfoSnapshot& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace media
} // namespace vcs
