#pragma once

// Negotiated codec information for one media kind of one session.
//
// Produced by the gateway (parsed from the remote SDP) and consumed by the
// media module (decoder/encoder configuration). Defined in common so neither
// side depends on the other's headers (docs/11 §1).

#include <cstdint>
#include <string>

namespace vcs {

struct NegotiatedCodec {
    std::string codecName;      // "H264", "G722", "PCMU", ... (upper case)
    uint8_t payloadType = 0;
    uint32_t clockRate = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t bitrate = 0;
    std::string fmtp;           // raw "a=fmtp" value, without the "<pt> " prefix
};

} // namespace vcs
