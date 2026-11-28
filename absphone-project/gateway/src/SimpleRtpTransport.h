/**
 * @file SimpleRtpTransport.h
 * @brief Simple RTP Transport - Cross-platform using Boost.Asio
 *
 * True cross-platform implementation using Boost.Asio.
 * No platform-specific headers or macros.
 *
 * Architecture:
 * Yealink <--UDP Socket--> SimpleRtpTransport <--> MCU
 *
 * Design principles:
 * 1. Use Boost.Asio for all networking (works on Windows, Linux, macOS, iOS)
 * 2. Gateway layer only does UDP passthrough
 * 3. Media layer handles all RTP parsing
 */
#pragma once

#include "IRtpTransport.h"
#include "PlatformSocket.h"
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>
#include <vector>
#include <array>

namespace vcs { namespace gateway {

/**
 * @brief RTP destination configuration
 */
struct RtpDestination {
    std::string address;      // Remote address
    uint16_t port = 0;       // Remote port
    uint8_t payloadType = 0; // Payload type (0 = all types)
    uint32_t ssrc = 0;       // SSRC
};

/**
 * @brief Simple RTP transport configuration
 */
struct SimpleRtpConfig {
    std::string localAddress = "0.0.0.0";  // Local listen address
    uint16_t localPort = 0;                 // Local RTP port (0 = auto)
    std::vector<RtpDestination> destinations;  // Remote destinations
    uint32_t ssrc = 0;                      // Default SSRC
    uint8_t defaultPayloadType = 97;        // Default payload type (H264)
    bool enableRtcp = false;               // Enable RTCP
};

/**
 * @brief Simple RTP Transport using Boost.Asio
 *
 * Cross-platform UDP socket implementation using Boost.Asio.
 * Works on Windows, Linux, macOS, iOS without any platform-specific code.
 */
class SimpleRtpTransport : public IRtpTransport {
public:
    explicit SimpleRtpTransport(const std::string& connectionId,
                                const SimpleRtpConfig& config);

    explicit SimpleRtpTransport(const std::string& connectionId,
                                const SimpleRtpConfig& config,
                                IRtpTransportCallback* initialCallback);

    ~SimpleRtpTransport() override;

    // IRtpTransport interface
    const std::string& GetType() const override { return type_; }
    const std::string& GetConnectionId() const override { return connectionId_; }
    uint32_t GetSsrc() const override { return ssrc_; }
    void SetSsrc(uint32_t ssrc) override;

    bool SendRtp(const RtpPacket& packet) override;
    bool SendRtcp(const RtcpPacket& packet) override;
    bool SendFir(uint32_t remoteSsrc, uint32_t localSsrc) override;

    /// [RFC 5761] Select where outgoing RTCP goes. Must be set from the
    /// remote SDP before media starts: true = same port as RTP (a=rtcp-mux),
    /// false = RTP port + 1 (classic split, the historical behaviour).
    void SetRtcpMux(bool enabled) { rtcpMux_.store(enabled); }
    bool IsRtcpMuxEnabled() const { return rtcpMux_.load(); }

    bool SetCallback(IRtpTransportCallback* callback) override;
    IRtpTransportCallback* GetCallback() const override { return callback_; }

    void SetContext(const vcs::SessionContext& context) override;
    const vcs::SessionContext& GetContext() const override { return context_; }

    bool IsReceiving() const override { return receiving_.load(); }
    bool IsConnected() const override { return connected_.load(); }

    std::string GetRemoteAddress() const override { return remoteAddress_; }

    // IRtpTransport interface (media-info diagnostics)
    std::string GetLocalAddress() const override;
    uint16_t GetLocalPort() const override;

    // SimpleRtpTransport interface
    bool Open();
    void Close();
    bool StartReceiving() override;
    void StopReceiving();

    void SetRemoteAddress(const std::string& address, uint16_t port);
    void AddDestination(const RtpDestination& destination);
    void ClearDestinations();

    const std::vector<RtpDestination>& GetDestinations() const { return destinations_; }

    struct Statistics {
        uint64_t packetsSent = 0;
        uint64_t packetsReceived = 0;
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint64_t lastSendTime = 0;
        uint64_t lastReceiveTime = 0;
    };
    Statistics GetStatistics() const;

private:
    void ReceiveLoop();

    // [V7.1] Post one async_receive_from on the io_context. Called by
    // ReceiveLoop() (initial post) and recursively by the completion
    // handler OnAsyncReceiveComplete() (re-post after each packet).
    // Must be a member function � using a local `auto` lambda here
    // would trigger "use of name before deduction of auto" because the
    // lambda's body refers to itself before its type is deduced.
    void PostAsyncReceive();

    // [V7.1] Completion handler for async_receive_from. Receives
    // either a packet (bytes > 0) or a status code (operation_aborted
    // on shutdown). On success re-posts by calling PostAsyncReceive().
    void OnAsyncReceiveComplete(const boost::system::error_code& ec,
                                std::size_t bytes);

    static constexpr const char* TYPE = "SimpleRTP";
    std::string type_ = TYPE;
    std::string connectionId_;

    // Boost.Asio UDP socket
    std::unique_ptr<vcs::base::UdpSocket> socket_;
    boost::asio::io_context ioContext_;

    // Primary remote address for backward compatibility
    std::unique_ptr<vcs::base::IPAddress> primaryRemoteAddr_;

    // Configuration
    std::string localAddress_;
    uint16_t localPort_ = 0;
    std::string remoteAddress_;
    uint16_t remotePort_ = 0;
    uint32_t ssrc_ = 0;
    uint8_t payloadType_ = 97;

    // [V7.0] Multiple destinations replaced by a single primary remote address.
    // Each transport now corresponds to a single media stream (audio / main video / secondary video),
    // so payload-type-based destination routing is no longer needed.
    std::vector<RtpDestination> destinations_;

    // State
    std::atomic<bool> connected_{false};
    std::atomic<bool> receiving_{false};

    // Callback
    IRtpTransportCallback* callback_ = nullptr;
    std::atomic<bool> callbackReady_{false};
    std::mutex callbackMutex_;

    // Session context
    vcs::SessionContext context_;

    // Thread
    std::unique_ptr<std::thread> receiveThread_;
    std::mutex mutex_;

    // [RFC 5761] RTCP destination policy.
    //   true  : the peer negotiated a=rtcp-mux -> RTCP shares the RTP port
    //   false : classic RTP/RTCP split -> RTCP goes to RTP port + 1
    // Flipped from the remote SDP before the first packet is sent.
    std::atomic<bool> rtcpMux_{false};

    // Serialises every datagram sent on socket_.
    //
    // boost::asio documents shared objects as NOT thread-safe, and the webrtc
    // media engine now sends from two of its own threads: the Call network
    // thread (pacer RTP + sender reports) and the Call worker thread
    // (receiver reports / NACK / PLI from the receive stream). Both land in
    // SendRtp/SendRtcp here while the 5.0 media threads may still send too,
    // so the socket access has to be serialised.
    std::mutex sendMutex_;

    // [V7.1] Async receive state.
    //
    // The receive path is event-driven: a single thread runs
    // ioContext_.run() which dispatches the async_receive_from completion
    // handler. Stopping the transport is a clean three-step shutdown:
    //   1. receiving_ = false     �� handler will not re-arm on next packet
    //   2. socket_->cancel()      �� Boost::Asio, cross-platform (Windows:
    //                                CancelIoEx, Linux: epoll/kqueue wakeup).
    //                                The in-flight async op completes with
    //                                operation_aborted.
    //   3. receiveThread_->join() �� run() processes the cancel completion,
    //                                handler sees receiving_=false, returns,
    //                                run() sees no more work, returns.
    //
    // This design eliminates the "stuck recvfrom on Linux" deadlock at
    // the architectural level. No watchdog, no platform-specific
    // setsockopt, no #ifdef �� just the async I/O model that Boost::Asio
    // was designed for.
    std::array<uint8_t, 2048> recvBuffer_{};
    boost::asio::ip::udp::endpoint recvSenderEndpoint_{};

    // Statistics
    Statistics stats_;

    static constexpr uint64_t LOG_INTERVAL_MS = 5000;
    uint64_t lastLogTime_ = 0;
    uint64_t packetCountSinceLastLog_ = 0;
    // Consecutive soft receive errors (e.g. WSAECONNRESET caused by an ICMP
    // port-unreachable for an earlier send). Reset on a successful receive;
    // a runaway counter means the socket is really gone.
    int consecutiveSoftRecvErrors_ = 0;
};

}} // namespace vcs::gateway
