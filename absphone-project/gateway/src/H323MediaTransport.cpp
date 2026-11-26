#include "H323MediaTransport.h"
#include <memory>
#include <sstream>

namespace vcs { namespace gateway {

struct H323MediaTransport::Impl {
    // H.323 MediaTransport related resources
    // TODO: Need to hold channel handle when integrating OpenH323/H323Plus
    void* h323Stack = nullptr;     // H.323 protocol stack handle
    int socketFd = -1;            // UDP socket
};

H323MediaTransport::H323MediaTransport(const std::string& connectionId,
                                      const std::string& remoteAddress,
                                      uint16_t remotePort)
    : impl_(std::make_unique<Impl>())
    , type_("H323")
    , connectionId_(connectionId)
    , remoteAddress_(remoteAddress)
    , remotePort_(remotePort)
    , ssrc_(0)
    , connected_(false)
    , callback_(nullptr) {
}

H323MediaTransport::~H323MediaTransport() = default;

void H323MediaTransport::SetSsrc(uint32_t ssrc) {
    ssrc_ = ssrc;
}

bool H323MediaTransport::SendRtp(const RtpPacket& packet) {
    if (!connected_) {
        return false;
    }

    // TODO: Send RTP packet through H.323 protocol stack
    // Call OpenH323 RTP Send interface
    return true;
}

bool H323MediaTransport::SendRtcp(const RtcpPacket& packet) {
    if (!connected_) {
        return false;
    }

    // TODO: Send RTCP packet through H.323 protocol stack
    return true;
}

bool H323MediaTransport::SetCallback(IRtpTransportCallback* callback) {
    callback_ = callback;
    return true;
}

void H323MediaTransport::SetContext(const vcs::SessionContext& context) {
    context_ = context;
}

const vcs::SessionContext& H323MediaTransport::GetContext() const {
    return context_;
}

std::string H323MediaTransport::GetRemoteAddress() const {
    std::ostringstream oss;
    oss << remoteAddress_ << ":" << remotePort_;
    return oss.str();
}

// [V5.5 Removed] SendVideoFrame legacy interface
// RTP packing moved to Media layer (RTPEngine::SendVideoRtp)

} // namespace gateway
} // namespace vcs
