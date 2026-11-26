#pragma once

// Session context handed to transport callbacks so that the receiver needs no
// extra lookup: one session maps to exactly one media pipeline.

#include <cstdint>
#include <string>

namespace vcs {

struct SessionContext {
    std::string sessionId;      // Gateway-level session id (primary key)
    std::string memberId;       // Conference member id (usually == sessionId)
    std::string conferenceId;   // Owning conference id
    std::string transportType;  // SimpleRTP / SIP / WebRTC

    // Stream identity deliberately lives in the transport object: one object
    // is pinned to one MediaStreamKind, and the media layer classifies
    // inbound packets by SSRC / negotiated codec on its own. SSRC and payload
    // type fields used to sit here, were written once and never read, and
    // invited callers to treat this struct as a media policy channel.

    bool IsValid() const {
        return !sessionId.empty() && !memberId.empty() && !conferenceId.empty();
    }

    SessionContext() = default;

    SessionContext(const std::string& sid, const std::string& mid, const std::string& cid)
        : sessionId(sid), memberId(mid), conferenceId(cid) {}

    std::string ToString() const {
        return "SessionContext{sessionId=" + sessionId +
               ", memberId=" + memberId +
               ", conferenceId=" + conferenceId +
               ", transportType=" + transportType + "}";
    }
};

} // namespace vcs
