#pragma once

// SDP codec parsing shared by SipCall (outbound, active-remote SDP) and
// SipSignalingHandler (INVITE-intercepted inbound, active-local SDP).
// First m=audio / m=video line wins; its fmt[0] is the negotiated PT.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include <pjmedia/sdp.h>

#include "NegotiatedCodec.h"

namespace vcs { namespace gateway { namespace sdp {

// Static audio payload types may appear without an a=rtpmap line.
inline const char* StaticAudioCodecName(unsigned pt) {
    switch (pt) {
    case 0: return "PCMU";
    case 8: return "PCMA";
    case 9: return "G722";
    default: return nullptr;
    }
}

// "97 H264/90000" -> codecName "H264", clockRate 90000 (only when pt matches)
inline bool ParseRtpmapValue(const std::string& value, unsigned expectedPt,
                             std::string& codecName, uint32_t& clockRate) {
    std::istringstream iss(value);
    unsigned pt = 0;
    std::string enc;
    if (!(iss >> pt >> enc) || pt != expectedPt) return false;
    const size_t slash = enc.find('/');
    if (slash == std::string::npos) return false;
    codecName = enc.substr(0, slash);
    if (codecName.empty()) return false;
    std::transform(codecName.begin(), codecName.end(), codecName.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    clockRate = static_cast<uint32_t>(std::strtoul(enc.c_str() + slash + 1, nullptr, 10));
    return true;
}

// "97 profile-level-id=42e01f;sprop-parameter-sets=..." -> strip "<pt> " prefix
inline bool ParseFmtpValue(const std::string& value, unsigned expectedPt,
                           std::string& fmtp) {
    std::istringstream iss(value);
    unsigned pt = 0;
    if (!(iss >> pt) || pt != expectedPt) return false;
    std::getline(iss, fmtp);
    if (!fmtp.empty() && fmtp.front() == ' ') fmtp.erase(0, 1);
    return true;
}

inline void FillNegotiatedCodec(const pjmedia_sdp_media* m,
                                vcs::NegotiatedCodec& out) {
    out = vcs::NegotiatedCodec();
    if (!m || m->desc.fmt_count == 0) return;

    const unsigned pt = static_cast<unsigned>(pj_strtoul(&m->desc.fmt[0]));
    out.payloadType = static_cast<uint8_t>(pt);

    for (unsigned i = 0; i < m->attr_count; ++i) {
        const pjmedia_sdp_attr* a = m->attr[i];
        if (!a || !a->name.ptr) continue;
        const std::string name(a->name.ptr, a->name.slen);
        const std::string value = a->value.ptr
            ? std::string(a->value.ptr, a->value.slen)
            : std::string();
        if (name == "rtpmap") {
            ParseRtpmapValue(value, pt, out.codecName, out.clockRate);
        }
        else if (name == "fmtp") {
            ParseFmtpValue(value, pt, out.fmtp);
        }
    }

    if (out.codecName.empty()) {
        if (const char* n = StaticAudioCodecName(pt)) {
            out.codecName = n;
            out.clockRate = 8000;
        }
    }
}

// Fills the first audio and first video m-lines. Missing kinds stay empty;
// an unparseable SDP/m-line is not an error (callers fall back to defaults).
inline bool ParseNegotiatedCodecs(const pjmedia_sdp_session* sdp,
                                  vcs::NegotiatedCodec& audioCodec,
                                  vcs::NegotiatedCodec& videoCodec) {
    audioCodec = vcs::NegotiatedCodec();
    videoCodec = vcs::NegotiatedCodec();
    if (!sdp) return false;

    bool audioDone = false;
    bool videoDone = false;
    bool foundAudio = false;
    bool foundVideo = false;
    for (unsigned i = 0; i < sdp->media_count && !(audioDone && videoDone); ++i) {
        const pjmedia_sdp_media* m = sdp->media[i];
        if (!m) continue;
        if (pj_strcmp2(&m->desc.media, "audio") == 0 && !audioDone) {
            FillNegotiatedCodec(m, audioCodec);
            audioDone = true;
            foundAudio = !audioCodec.codecName.empty();
        }
        else if (pj_strcmp2(&m->desc.media, "video") == 0 && !videoDone) {
            FillNegotiatedCodec(m, videoCodec);
            videoDone = true;
            foundVideo = !videoCodec.codecName.empty();
        }
    }
    return foundAudio || foundVideo;
}

}}} // namespace vcs::gateway::sdp
