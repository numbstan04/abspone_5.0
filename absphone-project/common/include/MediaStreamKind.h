#pragma once

// Media stream kind: identifies which stream of one session a transport or a
// packet belongs to. Terminal semantics: one session carries audio + video
// (+ optional secondary video / data channel).

namespace vcs {

enum class MediaStreamKind : unsigned char {
    Audio = 0,          // Primary audio stream (RTP)
    Video = 1,          // Primary video stream (RTP/RTCP)
    SecondaryVideo = 2, // Secondary video, e.g. content sharing
    Data = 3            // Data channel (WebRTC SCTP, SIP MSRP, ...)
};

inline const char* MediaStreamKindToString(MediaStreamKind kind) {
    switch (kind) {
    case MediaStreamKind::Audio:          return "audio";
    case MediaStreamKind::Video:          return "video";
    case MediaStreamKind::SecondaryVideo: return "secondaryVideo";
    case MediaStreamKind::Data:           return "data";
    }
    return "unknown";
}

} // namespace vcs
