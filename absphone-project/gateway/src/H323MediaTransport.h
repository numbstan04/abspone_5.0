#pragma once

#include "IRtpTransport.h"
#include <string>
#include <memory>

namespace vcs { namespace gateway {

/**
 * @brief H.323 Media Transport
 *
 * RTP media transport implementation for the H.323 protocol.
 * H.323 uses a different mechanism from SIP to establish media channels.
 *
 * Design principles:
 * 1. Each H.323 session corresponds to an H323MediaTransport instance
 * 2. Works in conjunction with the H.323 protocol stack
 * 3. Handles RTP packet assembly and disassembly
 */
class H323MediaTransport : public IRtpTransport {
public:
    H323MediaTransport(const std::string& connectionId,
                        const std::string& remoteAddress,
                        uint16_t remotePort);
    ~H323MediaTransport() override;

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
    std::string GetRemoteAddress() const override;

    // Receive management interface
    bool StartReceiving() override { return true; }  // H.323 uses protocol-stack-managed receive
    void StopReceiving() override {}  // H.323 uses protocol-stack-managed receive
    bool IsReceiving() const override { return connected_; }

    // FIR key frame request
    bool SendFir(uint32_t remoteSsrc, uint32_t localSsrc) override { return false; }  // H.323 does not support this yet

    // [V5.5 Removed] Legacy SendVideoFrame interface.
    // RTP packetization has moved to the Media layer (RTPEngine::SendVideoRtp).
    // The H.323 layer is only responsible for protocol encapsulation.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string type_;
    std::string connectionId_;
    std::string remoteAddress_;
    uint16_t remotePort_;
    uint32_t ssrc_;
    bool connected_;
    IRtpTransportCallback* callback_;
    vcs::SessionContext context_;  // Session context
};

} // namespace gateway
} // namespace vcs
