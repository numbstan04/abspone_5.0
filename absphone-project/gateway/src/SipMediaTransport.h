/**
 * @file SipMediaTransport.h
 * @brief SIP media transport (RTP passthrough mode)
 *
 * Design principles:
 * 1. PJSIP only does SDP negotiation and RTP forwarding (socket management), no media codec processing
 * 2. Media module handles codec and mixing itself
 *
 * Data flow:
 *   Yealink <--RTP--> PJSIP transport --rtp_cb--> Media module
 *                <-- MCU ---> send_rtp --> Yealink
 *
 * Key design:
 * 1. Use PJSIP media transport (no independent socket creation)
 * 2. Receive RTP packets via PJSIP's rtp_cb callback
 * 3. Send RTP packets via PJSIP's send_rtp
 */
#pragma once

#include "IRtpTransport.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>

// Forward declaration
namespace pj {
class Endpoint;
class Call;
}

namespace vcs { namespace gateway {

/**
 * @brief SIP media transport (RTP passthrough mode, using PJSIP transport)
 *
 * Design principles:
 * 1. PJSIP only does SDP negotiation and RTP forwarding, Media module handles all codec
 * 2. Use PJSIP media transport to get RTP socket
 *
 * Data flow:
 *   Yealink <--RTP--> PJSIP transport --rtp_cb--> Media module (codec + mixing)
 *
 * Key design:
 * - Use PJSIP transport: reuse PJSIP's existing RTP socket
 * - RTP callback: PJSIP passes RTP to Media module via callback
 * - Send: Media module sends via pjmedia_transport_send_rtp after synthesis
 */
class SipMediaTransport : public IRtpTransport {
public:
    // kind pins this instance to exactly one media stream. The setters refuse
    // a pjmedia transport of the other kind, and egress picks the pjmedia
    // transport by kind instead of "video first". One pjmedia transport has a
    // single external-callback slot, so letting the video wrapper bind the
    // audio transport silently steals its receive path.
    SipMediaTransport(const std::string& connectionId,
                      const std::string& remoteAddress,
                      uint16_t remotePort,
                      vcs::MediaStreamKind kind = vcs::MediaStreamKind::Audio);
    ~SipMediaTransport() override;

    // IRtpTransport Implementation
    const std::string& GetType() const override { return type_; }
    const std::string& GetConnectionId() const override { return connectionId_; }
    uint32_t GetSsrc() const override { return ssrc_; }
    void SetSsrc(uint32_t ssrc) override;
    bool SendRtp(const RtpPacket& packet) override;
    bool SendRtcp(const RtcpPacket& packet) override;
    bool SetCallback(IRtpTransportCallback* callback) override;
    IRtpTransportCallback* GetCallback() const override {
        return callback_.load(std::memory_order_acquire);
    }
    void SetContext(const vcs::SessionContext& context) override;
    const vcs::SessionContext& GetContext() const override;
    bool IsConnected() const override { return connected_; }
    std::string GetRemoteAddress() const override;
    bool IsReceiving() const override;

    // IRtpTransport interface (media-info diagnostics)
    std::string GetLocalAddress() const override;
    uint16_t GetLocalPort() const override;

    // Extended API
    bool Connect();
    void Disconnect();
    void SetLocalAddress(const std::string& address, uint16_t port);

    /**
     * @brief Start RTP receive loop
     *
     * Receive RTP packets in a separate thread and dispatch via callback
     * Must call Connect() before calling this method
     *
     * @return true on success, false on failure
     * @note Implements IRtpTransport interface
     */
    bool StartReceiving() override;

    /**
     * @brief Stop RTP receiving
     *
     * Returns only after the pjmedia external callbacks are uninstalled and
     * no dispatch is in flight, so the media layer may destroy its callback
     * object as soon as this returns.
     */
    void StopReceiving() override;

    // FIR keyframe request. Sent through the pjlib-registered send thread.
    // remoteSsrc may be 0 before the first inbound packet has taught us the
    // peer's SSRC; the peer treats it as a bootstrap request (same shape the
    // external UDP path has always sent).
    bool SendFir(uint32_t remoteSsrc, uint32_t localSsrc) override;

    // Statistics
    uint64_t GetPacketsReceived() const;
    uint64_t GetPacketsSent() const;
    uint64_t GetBytesReceived() const;
    uint64_t GetBytesSent() const;

    // ============================================================
    // RTP passthrough mode interface
    // ============================================================

    /**
     * @brief Set RTP receive callback (receive raw RTP packets directly)
     *
     * Called when PJSIP transport receives RTP packets.
     * Data is raw RTP packet, Media module is responsible for decoding.
     *
     * @param callback Callback function, receives RTP packet data
     */
    void SetRtpCallback(std::function<void(const uint8_t* data, size_t size)> callback) {
        rtpCallback_ = callback;
    }

    /**
     * @brief Set RTCP receive callback
     *
     * @param callback Callback function
     */
    void SetRtcpCallback(std::function<void(const uint8_t* data, size_t size)> callback) {
        rtcpCallback_ = callback;
    }

    // [V5.5 Deleted] SendVideoFrame legacy interface
    // RTP encapsulation has been moved to Media layer (RTPEngine::SendVideoRtp)
    // Gateway layer only handles RTP passthrough (SendRtp interface)

    /**
     * @brief Get remote RTP address
     */
    std::string GetRtpAddress() const;
    uint16_t GetRtpPort() const;

    /**
     * @brief Set PJSIP media transport pointer (audio)
     *
     * Called by SipSignalingHandler in OnCallMediaReady
     * Set PJSIP's internal pjmedia_transport into SipMediaTransport
     *
     * @param transport PJSIP's pjmedia_transport pointer
     */
    void SetPjMediaTransport(void* transport);

    /**
     * @brief Set PJSIP video media transport pointer (video)
     *
     * Used for H264 video RTP passthrough
     *
     * @param transport PJSIP's pjmedia_transport pointer
     */
    void SetPjVideoTransport(void* transport);

    /**
     * @brief pjmedia transport this instance is bound to, by kind (nullptr if none)
     *
     * The adapter compares this against the stack's current transport when a
     * re-INVITE re-confirms the call: pjsua may replace it, and the media
     * layer - which pinned this object on the first CONFIRMED - cannot follow
     * such a swap. A mismatch must be reported and the old object detached
     * rather than silently sending on a freed pointer.
     */
    void* GetBoundPjMediaTransport() const;

    // Payload-type classification was removed on purpose: this instance is
    // pinned to one kind_ at construction, and the media layer classifies
    // streams by SSRC / negotiated codec. A PT list here had no functional
    // consumer and only invited callers to "configure" something inert.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Which pjmedia API installed the external receive callbacks. Teardown
    // must uninstall through the same one - the two setters write different
    // transport layouts and the SRTP variant does not validate its argument.
    enum class ExternalCallbackKind {
        None,
        Srtp,
        Udp
    };

    // Stop pjmedia from entering this object, then barrier any dispatch that
    // is already running, then drop our callback. After this returns neither
    // pjmedia nor the media layer can reach the IRtpTransportCallback, which
    // is what lets the media layer destroy it right after SetCallback(nullptr).
    void DetachReceivers();

    // Read the local socket endpoint from pjmedia once, on the SIP thread,
    // and cache it. GetLocalAddress/GetLocalPort must be pure reads: pjmedia's
    // transport get_info initialises a caller-owned array, so calling it from
    // the media/UI threads is both unsafe and assert-prone.
    void CaptureLocalEndpoint();

    // Internal send path shared by RTP / RTCP / FIR. The task is queued and
    // executed on the pjlib-registered send thread; returns false (and warns
    // once per disconnect window) when the transport is not connected.
    bool EnqueueSend(const uint8_t* data, size_t size,
                     uint32_t ssrc, uint64_t timestamp,
                     uint16_t sequenceNumber, bool isRtcp);

    bool SendRtpInternal(const uint8_t* data, size_t length,
                         uint32_t ssrc, uint64_t timestamp,
                         uint16_t sequenceNumber);
    bool SendRtcpInternal(const uint8_t* data, size_t length);

    // pjmedia transport this instance may use for egress, selected by kind_.
    // Returns the underlying member when pjsua wrapped the UDP transport in
    // SRTP (always the case), so plain RTP/RTCP keeps flowing on a session
    // where SRTP was never activated.
    void* PickEgressTransport() const;

    // Dispatch-vs-detach coordination.
    //
    // A dispatch registers itself in the in-flight count while holding
    // dispatchMutex_, then RELEASES the lock before invoking the media
    // callback. Detach waits for the count to drain.
    //
    // Consequences (these are the contract, not advice):
    //   - no user callback ever runs under a transport lock, so a slow media
    //     handler cannot block SetCallback/Disconnect or another packet;
    //   - a callback may call SetCallback() again without self-deadlock;
    //   - Disconnect() from inside a callback is detected (thread-local
    //     owner) and does not wait on its own packet; it only clears the
    //     pointer so later packets see none.
    IRtpTransportCallback* BeginDispatch();
    void EndDispatch();

    // RAII so a throwing callback cannot leave the in-flight count raised and
    // wedge every later detach.
    struct DispatchGuard {
        explicit DispatchGuard(SipMediaTransport& self) : self_(self) {}
        ~DispatchGuard() { self_.EndDispatch(); }
        DispatchGuard(const DispatchGuard&) = delete;
        DispatchGuard& operator=(const DispatchGuard&) = delete;
        SipMediaTransport& self_;
    };

    std::string type_;
    std::string connectionId_;
    std::string remoteAddress_;
    uint16_t remotePort_;
    uint32_t ssrc_;
    vcs::MediaStreamKind kind_;
    std::atomic<bool> connected_;
    std::atomic<bool> sendDropWarned_;   // 每个断连窗口只报一次丢包，避免逐包刷屏
    std::atomic<bool> dispatchNullWarned_;  // 同上：拆链后仍在派的包只报一次

    // Guards the callback pointer and the in-flight count. NEVER held while
    // user code runs; see BeginDispatch/EndDispatch above.
    mutable std::mutex dispatchMutex_;
    std::condition_variable dispatchCv_;
    int dispatchInFlight_ = 0;
    std::atomic<IRtpTransportCallback*> callback_;
    vcs::SessionContext context_;  // Session context

    // RTP callback (passthrough mode)
    std::function<void(const uint8_t* data, size_t size)> rtpCallback_;
    std::function<void(const uint8_t* data, size_t size)> rtcpCallback_;

    // Internal callback handling methods
    void OnRtpReceived(const uint8_t* data, size_t size);
    void OnRtcpReceived(const uint8_t* data, size_t size);
};

} // namespace gateway
} // namespace vcs
