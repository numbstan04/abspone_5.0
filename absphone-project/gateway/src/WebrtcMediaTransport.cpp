#include "WebrtcMediaTransport.h"
#include <memory>

namespace vcs { namespace gateway {

struct WebrtcMediaTransport::Impl {
    // WebRTCMediaTransport related resources
    // TODO: When integrating libwebrtc or a similar library, related handles must be held
    void* peerConnection = nullptr;  // PeerConnection handle
    void* dtlsTransport = nullptr;   // DTLSTransport layer
};

WebrtcMediaTransport::WebrtcMediaTransport(const std::string& connectionId,
                                           const std::string& remoteAddress)
    : impl_(std::make_unique<Impl>())
    , type_("WebRTC")
    , connectionId_(connectionId)
    , remoteAddress_(remoteAddress)
    , ssrc_(0)
    , connected_(false)
    , callback_(nullptr) {
}

WebrtcMediaTransport::~WebrtcMediaTransport() = default;

void WebrtcMediaTransport::SetSsrc(uint32_t ssrc) {
    ssrc_ = ssrc;
}

bool WebrtcMediaTransport::SendRtp(const RtpPacket& packet) {
    if (!connected_) {
        return false;
    }

    // TODO: Send encrypted SRTP packets through WebRTC
    // Call libwebrtc's SendRtp interface
    return true;
}

bool WebrtcMediaTransport::SendRtcp(const RtcpPacket& packet) {
    if (!connected_) {
        return false;
    }

    // TODO: Send encrypted SRTCP packets through WebRTC
    return true;
}

bool WebrtcMediaTransport::SetCallback(IRtpTransportCallback* callback) {
    callback_ = callback;
    return true;
}

void WebrtcMediaTransport::SetContext(const vcs::SessionContext& context) {
    context_ = context;
}

const vcs::SessionContext& WebrtcMediaTransport::GetContext() const {
    return context_;
}

// [V5.5 Removed] Legacy SendVideoFrame interface.
// RTP packetization has moved to the Media layer (RTPEngine::SendVideoRtp).

} // namespace gateway
} // namespace vcs
