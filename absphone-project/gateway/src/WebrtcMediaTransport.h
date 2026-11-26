#pragma once

#include "IRtpTransport.h"
#include <string>
#include <memory>

namespace vcs { namespace gateway {

/**
 * @brief WebRTC Media Transport
 *
 * WebRTC DTLS/SRTP media transport implementation.
 * Uses encrypted channels to transmit RTP/RTCP packets.
 *
 * Design principles:
 * 1. Each WebRTC session corresponds to a WebrtcMediaTransport instance
 * 2. Uses DTLS for key exchange, SRTP for encrypted transmission
 * 3. Works in conjunction with the WebRTC protocol stack
 */
class WebrtcMediaTransport : public IRtpTransport {
public:
    WebrtcMediaTransport(const std::string& connectionId,
                          const std::string& remoteAddress);
    ~WebrtcMediaTransport() override;

    // IRtpTransport implementation
    const std::string& GetType() const override { return type_; }
    const std::string& GetConnectionId() const override { return connectionId_; }
    uint32_t GetSsrc() const override { return ssrc_; }
    void SetSsrc(uint32_t ssrc) override;
    bool SendRtp(const RtpPacket& packet) override;
    bool SendRtcp(const RtcpPacket& packet) override;
    bool SetCallback(IRtpTransportCallback* callback) override;
    IRtpTransportCallback* GetCallback() const override { return callback_; }
    void SetContext(const vcs::SessionContext& context) override;
    const vcs::SessionContext& GetContext() const override;
    bool IsConnected() const override { return connected_; }
    std::string GetRemoteAddress() const override { return remoteAddress_; }

    // Receive management interface
    bool StartReceiving() override { return true; }  // WebRTC uses protocol-stack-managed receive
    void StopReceiving() override {}  // WebRTC uses protocol-stack-managed receive
    bool IsReceiving() const override { return connected_; }

    // FIR key frame request
    bool SendFir(uint32_t remoteSsrc, uint32_t localSsrc) override { return false; }  // WebRTC does not support this yet

    // [V5.5 Removed] Legacy SendVideoFrame interface.
    // RTP packetization has moved to the Media layer (RTPEngine::SendVideoRtp).
    // The WebRTC layer is only responsible for DTLS/SRTP encapsulation.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string type_;
    std::string connectionId_;
    std::string remoteAddress_;
    uint32_t ssrc_;
    bool connected_;
    IRtpTransportCallback* callback_;
    vcs::SessionContext context_;  // Session context
};

} // namespace gateway
} // namespace vcs
