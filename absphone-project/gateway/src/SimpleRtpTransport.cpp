/**
 * @file SimpleRtpTransport.cpp
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

#include "SimpleRtpTransport.h"
#include "ILogger.h"
#include "TimeUtils.h"
#include <memory>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>

namespace vcs { namespace gateway {

// Note: GetCurrentTimeMs() is now provided by the shared
// "TimeUtils.h" so it can be reused by other gateway TUs
// (e.g., SignalingHandler, SessionManager).

// ============================================================================
// Constructor and Destructor
// ============================================================================

SimpleRtpTransport::SimpleRtpTransport(const std::string& connectionId,
                                       const SimpleRtpConfig& config)
    : connectionId_(connectionId)
    , localAddress_(config.localAddress)
    , localPort_(config.localPort)
    , ssrc_(config.ssrc)
    , payloadType_(config.defaultPayloadType)
    , socket_(nullptr)
    , primaryRemoteAddr_(nullptr) {

    // Handle destinations from configuration
    for (const auto& dest : config.destinations) {
        AddDestination(dest);
    }
}

SimpleRtpTransport::SimpleRtpTransport(const std::string& connectionId,
                                       const SimpleRtpConfig& config,
                                       IRtpTransportCallback* initialCallback)
    : connectionId_(connectionId)
    , localAddress_(config.localAddress)
    , localPort_(config.localPort)
    , ssrc_(config.ssrc)
    , payloadType_(config.defaultPayloadType)
    , socket_(nullptr)
    , primaryRemoteAddr_(nullptr)
    , callback_(initialCallback)
    , callbackReady_(initialCallback != nullptr) {

    // Handle destinations from configuration
    for (const auto& dest : config.destinations) {
        AddDestination(dest);
    }

    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport created: connectionId=%s, callback=%s",
                         connectionId.c_str(), initialCallback ? "SET" : "NULL");
}

SimpleRtpTransport::~SimpleRtpTransport() {
    Close();
}

// ============================================================================
// IRtpTransport Interface Implementation
// ============================================================================

void SimpleRtpTransport::SetSsrc(uint32_t ssrc) {
    ssrc_ = ssrc;
}

bool SimpleRtpTransport::SendRtp(const RtpPacket& packet) {
    if (!socket_ || !socket_->isOpen() || !connected_) {
        return false;
    }

    // [V7.0] Each transport now owns a single primary remote address.
    // No more payload-type based destination lookup.
    if (!primaryRemoteAddr_) {
        VCS_GATEWAY_LOG_WARNING("SendRtp: no remote address configured");
        return false;
    }

    // [Log rate control] Print send statistics once per second
    static uint64_t lastSendLogTime = 0;
    static uint64_t sendCountSinceLastLog = 0;
    uint64_t nowMs = GetCurrentTimeMs();
    sendCountSinceLastLog++;

    size_t sent = 0;
    vcs::base::SocketError err = socket_->sendTo(packet.data, packet.length, *primaryRemoteAddr_, sent);

    if (err == vcs::base::SocketError::Success && sent > 0) {
        stats_.packetsSent++;
        stats_.bytesSent += sent;
        stats_.lastSendTime = GetCurrentTimeMs();

        // [Log rate control] Print send statistics once per second
        if (nowMs - lastSendLogTime >= 1000) {
            VCS_GATEWAY_LOG_INFO("SendRtp: 1s=%llu packets, total=%llu",
                                 (unsigned long long)sendCountSinceLastLog,
                                 (unsigned long long)stats_.packetsSent);
            lastSendLogTime = nowMs;
            sendCountSinceLastLog = 0;
        }
        return true;
    }

    VCS_GATEWAY_LOG_WARNING("Failed to send RTP: %s",
                            vcs::base::SocketException::errorToString(err).c_str());
    return false;
}

bool SimpleRtpTransport::SendRtcp(const RtcpPacket& packet) {
    if (!socket_ || !socket_->isOpen() || !connected_) {
        return false;
    }

    // [V7.0] Use primary remote address directly.
    if (!primaryRemoteAddr_) {
        VCS_GATEWAY_LOG_WARNING("SendRtcp: no remote address configured");
        return false;
    }

    size_t sent = 0;
    vcs::base::SocketError err = socket_->sendTo(packet.data, packet.length, *primaryRemoteAddr_, sent);

    if (err == vcs::base::SocketError::Success && sent > 0) {
        return true;
    }

    VCS_GATEWAY_LOG_WARNING("Failed to send RTCP: %s",
                            vcs::base::SocketException::errorToString(err).c_str());
    return false;
}

// ============================================================================
// FIR (Full Intra Request) Key frame request - V5.5 New
// ============================================================================
//
// RTCP FIR packet layout (RFC 5104 §4.3.1, payload type PSFB=206, FMT=4):
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|  FMT=4  |   PT=206      |          length=3             |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                  SSRC of packet sender (local SSRC)           |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                  SSRC of media source  (remote SSRC)          |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                  FCI: FIR command (8 bytes)                   |
//  |    SSRC of media source       | Seq nr.  |R R R R R R R R R R|
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
//  Total: 4 (header) + 4 (sender SSRC) + 4 (media SSRC) + 8 (FCI) = 20 bytes.
//
//  NOTE on `length` field (RFC 3550 §6.1):
//    length = (packet_size_in_32bit_words) - 1 = 20/4 - 1 = 4.
//    The previous version of this function set length=2 and placed FMT
//    in the wrong byte (offset 2 instead of the low 5 bits of byte 0).
//    That packet is still accepted by some lenient endpoints (e.g. some
//    Yealink firmwares ignore malformed FIR), but it is not RFC 5104
//    compliant. This rewrite fixes both defects and the static-counter
//    race at the same time.

bool SimpleRtpTransport::SendFir(uint32_t remoteSsrc, uint32_t localSsrc) {
    if (!socket_ || !socket_->isOpen() || !connected_) {
        VCS_GATEWAY_LOG_WARNING("SendFir: socket not connected");
        return false;
    }

    // [V7.0] Use primary remote address directly.
    // Each transport corresponds to a single media stream, no need to look up by payload type.
    if (!primaryRemoteAddr_) {
        VCS_GATEWAY_LOG_WARNING("SendFir: no remote address configured");
        return false;
    }

    vcs::base::IPAddress* targetAddr = primaryRemoteAddr_.get();

    // Build a 20-byte RTCP FIR message (RFC 5104).
    //   byte 0  : V(2)=10 | P(1)=0 | FMT(5)=00100  => 0x84
    //   byte 1  : PT = 206 (PSFB)
    //   bytes 2-3: length = 4 (because 20/4 - 1 = 4, NOT 2 as the old code did)
    //   bytes 4-7 : SSRC of packet sender (the MCU, i.e. localSsrc)
    //   bytes 8-11: SSRC of media source  (the endpoint, i.e. remoteSsrc)
    //   bytes 12-15: FCI - SSRC of media source (repeated)
    //   byte 16 : FIR command sequence number (atomic, 0..255, wrap-around)
    //   bytes 17-19: 3 reserved bytes (zero)
    uint8_t rtcpPacket[20] = {0};
    size_t offset = 0;

    // RTCP header (4 bytes)
    rtcpPacket[offset++] = 0x84;  // V=2, P=0, FMT=4 (FIR)
    rtcpPacket[offset++] = 206;   // PT = 206 (PSFB, not RTPFB - that's 205)
    rtcpPacket[offset++] = 0x00;  // length high byte
    rtcpPacket[offset++] = 0x04;  // length low byte = 4 (i.e. 5 words minus 1 = 4)

    // Inline little helper for big-endian 32-bit writes.
    auto writeBE32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
        p[3] = static_cast<uint8_t>( v        & 0xFF);
    };

    // SSRC of packet sender (the MCU itself - the entity that sends this FIR).
    writeBE32(rtcpPacket + offset, localSsrc);
    offset += 4;

    // SSRC of media source (the endpoint that needs to send a key frame).
    writeBE32(rtcpPacket + offset, remoteSsrc);
    offset += 4;

    // FCI (Feedback Command Information) - 8 bytes
    // First 4 bytes: SSRC of media source (same as above, per RFC 5104 §4.3.1.1)
    writeBE32(rtcpPacket + offset, remoteSsrc);
    offset += 4;

    // Next 4 bytes: Sequence number (1 byte) + Reserved (3 bytes)
    // [Thread-safety fix] Use std::atomic to make the increment race-free
    // and to match the implementation used by RTPEngine::SendFir.
    static std::atomic<uint8_t> s_firSeq{0};
    uint8_t seq = s_firSeq.fetch_add(1, std::memory_order_relaxed);
    rtcpPacket[offset++] = seq;
    rtcpPacket[offset++] = 0x00;  // Reserved
    rtcpPacket[offset++] = 0x00;  // Reserved
    rtcpPacket[offset++] = 0x00;  // Reserved

    // Sanity check: we should have written exactly 20 bytes.
    // If this ever fires, the packet length field above must be updated.
    if (offset != sizeof(rtcpPacket)) {
        VCS_GATEWAY_LOG_ERROR("[FIR] internal size mismatch: wrote %zu, expected %zu",
                              offset, sizeof(rtcpPacket));
        return false;
    }

    // RTCP Port = RTPPort + 1 (RFC 5761)
    // Note: RTCP usually sends to port+1, but some devices may use the same port.
    uint16_t rtcpPort = targetAddr->port();
    vcs::base::IPAddress rtcpAddr(targetAddr->toString(),
                                  static_cast<uint16_t>(rtcpPort + 1),
                                  targetAddr->family());

    size_t sent = 0;
    vcs::base::SocketError err = socket_->sendTo(rtcpPacket, sizeof(rtcpPacket),
                                                 rtcpAddr, sent);

    if (err == vcs::base::SocketError::Success && sent > 0) {
        VCS_GATEWAY_LOG_INFO("[FIR] Sent FIR: remoteSsrc=0x%08X, localSsrc=0x%08X, seq=%u, rtcpPort=%u",
                             remoteSsrc, localSsrc, seq, rtcpPort + 1);
        return true;
    }

    // If RTCP port fails, try the same port (some endpoints put RTCP on the RTP port).
    VCS_GATEWAY_LOG_WARNING("[FIR] RTCP port failed, trying same port as RTP");
    err = socket_->sendTo(rtcpPacket, sizeof(rtcpPacket), *targetAddr, sent);

    if (err == vcs::base::SocketError::Success && sent > 0) {
        VCS_GATEWAY_LOG_INFO("[FIR] Sent FIR via RTP port: remoteSsrc=0x%08X, localSsrc=0x%08X",
                             remoteSsrc, localSsrc);
        return true;
    }

    VCS_GATEWAY_LOG_ERROR("[FIR] Failed to send FIR: %s",
                          vcs::base::SocketException::errorToString(err).c_str());
    return false;
}

bool SimpleRtpTransport::SetCallback(IRtpTransportCallback* callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);

    // [FixV5.3] Remove receiving_ check, allow callback update during receive
    // Reasons:
    // 1. callbackMutex_ already protects concurrent access to callback_, thread-safe
    // 2. ReceiveThread uses callbackReady_ atomic variable to check if callback is ready
    // 3. Support transport being SetCallback() multiple times after StartReceiving()
    //    (e.g., SipSignalingHandler creates transport, then StartReceiving(),
    //     later MediaControl calls SetCallback())
    // 4. ReceiveThread holds lock when reading callback_ (or uses callbackReady_ atomic check)

    callback_ = callback;
    // Memory barrier: ensure callbackReady_ becomes true only after callback_ write completes
    std::atomic_thread_fence(std::memory_order_release);
    callbackReady_ = (callback != nullptr);

    VCS_GATEWAY_LOG_DEBUG("[SimpleRtpTransport::SetCallback] connectionId=%s, callback=%s, receiving=%d",
                          connectionId_.c_str(), callback ? "SET" : "NULL", receiving_.load());
    return true;
}

void SimpleRtpTransport::SetContext(const vcs::SessionContext& context) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    context_ = context;
    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport context set: sessionId=%s, memberId=%s, conferenceId=%s",
        context.sessionId.c_str(), context.memberId.c_str(), context.conferenceId.c_str());
}

// ============================================================================
// SimpleRtpTransport Specific Interface Implementation
// ============================================================================

bool SimpleRtpTransport::Open() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (socket_ && socket_->isOpen()) {
        VCS_GATEWAY_LOG_WARNING("SimpleRtpTransport already opened");
        return true;
    }

    // Create UDP socket
    socket_ = std::make_unique<vcs::base::UdpSocket>();

    // Bind local address first (this creates the actual socket)
    vcs::base::SocketError err = socket_->bind(ioContext_, localPort_, localAddress_);
    if (err != vcs::base::SocketError::Success) {
        const char* errStr = vcs::base::SocketException::errorToString(err).c_str();
        // [V7.1 Fix 2026-08-13] Add explicit diagnosis for the
        // Address-already-in-use case, which is exactly the symptom of a
        // port-allocator bug we just fixed. With SO_REUSEADDR off, a
        // EADDRINUSE here means the per-call port assignment is broken
        // and the call MUST be torn down — do not silently retry.
        if (err == vcs::base::SocketError::AddressInUse) {
            VCS_GATEWAY_LOG_ERROR(
                "PORT CONFLICT: Failed to bind %s:%u - address already in use. "
                "This usually means RtpPortAllocator handed out a port that is "
                "still occupied by a previous call (the previous call's "
                "DISCONNECTED-state Release() never fired, or the allocator's "
                "range is too narrow). Cause: %s",
                localAddress_.c_str(), localPort_, errStr);
        } else {
            VCS_GATEWAY_LOG_ERROR("Failed to bind socket to %s:%u: %s",
                                  localAddress_.c_str(), localPort_, errStr);
        }
        socket_.reset();
        return false;
    }

    // [V5.5 Fix] Set socket options AFTER bind (socket must exist before setting options)
    vcs::base::SocketOptions options;
    // [V7.1 Fix 2026-08-13] Turn SO_REUSEADDR OFF. Leaving it on caused a
    // silent port conflict: when a second terminal joined a conference
    // whose first terminal was still alive, both SimpleRtpTransports
    // bound to the same global RTP port (4004/4006), the kernel
    // demultiplexed UDP datagrams between them in an implementation-
    // defined way, and RTPEngine observed wild SSRC oscillation.
    // Now that SipSignalingHandler hands each call its own port pair,
    // we want the second bind() to FAIL loudly instead of silently
    // corrupting traffic.
    options.reuseAddress = false;
	options.receiveBufferSize = 4 * 1024 * 1024;  // 4MB
	options.sendBufferSize = 4 * 1024 * 1024;      // 4MB

    err = socket_->setOptions(options);
    if (err != vcs::base::SocketError::Success) {
        VCS_GATEWAY_LOG_WARNING("Failed to set socket options: %s",
                                vcs::base::SocketException::errorToString(err).c_str());
    }

	// 收发缓冲由上面的 setOptions 经 Boost.Asio 设置。这里不再对
	// native_handle 直接调 setsockopt：那是平台分支，且与 setOptions 重复。

    // Get actual bound port
    // Note: Boost.Asio does not directly provide a method to get local address,
    // if localPort_ is 0, try to get from system (simplified handling, keep original value)
    // Actual port on some platforms may need to get via getsockname

    connected_ = true;
    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport opened: local=%s:%u, socket=%s, callback=%s",
                         localAddress_.c_str(), localPort_,
                         socket_->isOpen() ? "OPEN" : "CLOSED",
                         callbackReady_ ? "READY" : "NOT_SET");

    // If remote address is already configured, set remote
    if (!remoteAddress_.empty() && remotePort_ > 0) {
        SetRemoteAddress(remoteAddress_, remotePort_);
    }

    // [Design principles] Open() only initializes socket, does not start receive thread
    // Caller is responsible for calling StartReceiving() after SetCallback() and Open()
    // This ensures callback set order: SetCallback -> Open -> StartReceiving

    return true;
}

void SimpleRtpTransport::Close() {
    std::lock_guard<std::mutex> lock(mutex_);

    StopReceiving();

    if (socket_) {
        socket_->close();
        socket_.reset();
    }

    // Clear all addresses
    primaryRemoteAddr_.reset();
    destinations_.clear();

    connected_ = false;
    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport closed");
}

bool SimpleRtpTransport::StartReceiving() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!socket_ || !socket_->isOpen()) {
        VCS_GATEWAY_LOG_ERROR("Cannot start receiving: socket not opened");
        return false;
    }

    // [Thread safety] Must be called after SetCallback
    if (!callback_) {
        VCS_GATEWAY_LOG_ERROR("Cannot start receiving: callback not set (call SetCallback first)");
        return false;
    }

    if (receiving_) {
        VCS_GATEWAY_LOG_DEBUG("[DIAG-StartReceiving] this=%p, already receiving, return true", this);
        return true;
    }

    receiving_ = true;

    // [V7.1] Reset the io_context for a fresh event loop. (If a previous
    // StopReceiving() called stop(), we need restart() before run() can
    // dispatch new work.)
    ioContext_.restart();

    receiveThread_ = std::make_unique<std::thread>(&SimpleRtpTransport::ReceiveLoop, this);
    VCS_GATEWAY_LOG_DEBUG("[DIAG-StartReceiving] this=%p, receive thread CREATED, thread_id=...", this);

    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport receive thread started (async mode)");
    return true;
}

void SimpleRtpTransport::StopReceiving() {
    VCS_GATEWAY_LOG_DEBUG("[DIAG-StopReceiving] ENTER this=%p, receiving_=%d, thread_joinable=%d",
        this, receiving_.load() ? 1 : 0, (receiveThread_ && receiveThread_->joinable()) ? 1 : 0);
    if (!receiving_) {
        VCS_GATEWAY_LOG_DEBUG("[DIAG-StopReceiving] EXIT (already not receiving) this=%p", this);
        return;
    }

    receiving_ = false;

    // [Fix 2026-08-12] Close the socket BEFORE join() to unblock a stuck
    // receive_from() call.
    //
    // ReceiveLoop uses a synchronous Boost.Asio receive_from() with no
    // timeout. If the peer stops sending packets after BYE, the loop
    // thread will be parked inside that syscall indefinitely.  Setting
    // receiving_=false alone is not enough because the loop body never
    // gets a chance to re-check the flag while it is blocked in the
    // kernel.  StopReceiving() would then hang on join() forever, which
    // in turn hangs HandleExternalBye(), which in turn prevents
    // MemberLeave() from running and keeps the encoder spinning.
    //
    // Calling socket_->close() here wakes the blocked syscall: the
    // pending receive_from() returns with an error, the loop body
    // re-evaluates receiving_ (or hits the !isOpen() check at the top
    // of the loop) and exits cleanly.  This is the standard
    // "unblock-by-close" pattern for synchronous Boost.Asio sockets.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (socket_ && socket_->isOpen()) {
            VCS_GATEWAY_LOG_DEBUG("[DIAG-StopReceiving] cancelling socket to unblock async_receive_from this=%p", this);
            socket_->cancel();
        }
    }

    if (receiveThread_ && receiveThread_->joinable()) {
        VCS_GATEWAY_LOG_DEBUG("[DIAG-StopReceiving] about to join() this=%p", this);
        receiveThread_->join();
        VCS_GATEWAY_LOG_DEBUG("[DIAG-StopReceiving] join() returned this=%p", this);
        receiveThread_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback_=nullptr;
    }

    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport receive thread stopped");
    VCS_GATEWAY_LOG_DEBUG("[DIAG-StopReceiving] EXIT (normal) this=%p", this);
}

void SimpleRtpTransport::SetRemoteAddress(const std::string& address, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);

    remoteAddress_ = address;
    remotePort_ = port;

    // Create new IPAddress object
    primaryRemoteAddr_ = std::make_unique<vcs::base::IPAddress>(address, port, AF_INET);

    // Also add to destinations_
    RtpDestination dest;
    dest.address = address;
    dest.port = port;
    dest.payloadType = 0;  // Default target, regardless of payload type
    destinations_.push_back(dest);

    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport remote address set: %s:%u", address.c_str(), port);
}

void SimpleRtpTransport::AddDestination(const RtpDestination& destination) {
    std::lock_guard<std::mutex> lock(mutex_);

    destinations_.push_back(destination);

    // [V7.0] Each transport now owns a single primary remote address.
    // payloadType is no longer used for routing; the new AddDestination simply
    // overwrites the previous primary address. In the multi-transport
    // architecture, each transport is created with exactly one destination.
    primaryRemoteAddr_ = std::make_unique<vcs::base::IPAddress>(
        destination.address, destination.port, AF_INET);

    VCS_GATEWAY_LOG_INFO("destination added: %s:%u (pt=%u)",
                         destination.address.c_str(), destination.port, destination.payloadType);
}

void SimpleRtpTransport::ClearDestinations() {
    std::lock_guard<std::mutex> lock(mutex_);

    primaryRemoteAddr_.reset();
    destinations_.clear();

    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport destinations cleared");
}

std::string SimpleRtpTransport::GetLocalAddress() const {
    return localAddress_;
}

uint16_t SimpleRtpTransport::GetLocalPort() const {
    return localPort_;
}

SimpleRtpTransport::Statistics SimpleRtpTransport::GetStatistics() const {
    return stats_;
}

// ============================================================================
// Private Methods Implementation
// ============================================================================

// ============================================================================
// ReceiveThread function
//
// [Design principles] Gateway layer only does network I/O, does not parse any media content
//
// 1. Receive UDP packets
// 2. Directly callback raw data to media layer
// 3. Media layer is responsible for all parsing (RTP header, PT, SSRC, audio/video distinction, etc.)
//
// Media module is responsible for:
// - Parse RTP header (version, PT, seq, timestamp, SSRC)
// - Distinguish audio/video based on SSRC
// - Determine codec format based on PT
// - FU-A fragmentation and reassembly
// - Decode, mix, encode
// ============================================================================
// [V7.1] Async receive loop, driven by Boost::Asio io_context.
//
// The previous synchronous design called socket_->receive_from() in a
// while(receiving_) loop, which translates to a blocking recvfrom() on
// Linux. On Linux close(fd) from another thread does NOT reliably wake
// a thread parked in recvfrom(), so the join() in StopReceiving() could
// hang for hundreds of ms (or forever), deadlocking the second-call
// teardown. The "fix" was initially a watchdog thread + SO_RCVTIMEO,
// but those were patches on top of a fundamentally broken design.
//
// The proper fix is to switch the receive path to Boost::Asio's async
// I/O model:
//   1. We post one async_receive_from on the io_context.
//   2. A worker thread runs io_context_.run() which dispatches the
//      completion handler when a packet arrives (or the op is cancelled).
//   3. The handler either re-arms (more packets wanted) or returns
//      (cancelled or socket closed).
//   4. Stopping is just socket_->cancel() + join(): Boost routes cancel
//      through CancelIoEx on Windows and through epoll/kqueue on Linux,
//      so the in-flight op completes immediately on every platform.
//   5. When run() has no more work to do, it returns. join() unblocks.
//
// No watchdog, no setsockopt(SO_RCVTIMEO), no #ifdef, identical
// behaviour on Windows and Linux.
// ============================================================================
void SimpleRtpTransport::ReceiveLoop() {
    VCS_GATEWAY_LOG_DEBUG("[DIAG-ReceiveLoop] START this=%p, receiving_=%d (async mode)",
        this, receiving_.load() ? 1 : 0);
    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport receive loop started (async mode)");

    // Prime the pump: post the first async_receive_from, then run().
    // The completion handler (OnAsyncReceiveComplete) re-posts for the
    // next packet as long as receiving_=true.
    PostAsyncReceive();
    ioContext_.run();

    VCS_GATEWAY_LOG_DEBUG("[DIAG-ReceiveLoop] EXIT this=%p (io_context returned, receiving_=%d)",
        this, receiving_.load() ? 1 : 0);
    VCS_GATEWAY_LOG_INFO("SimpleRtpTransport receive loop exited (async mode)");
}

void SimpleRtpTransport::PostAsyncReceive() {
    if (!socket_ || !socket_->isOpen() || !receiving_.load()) {
        return;
    }
    socket_->native().async_receive_from(
        boost::asio::buffer(recvBuffer_), recvSenderEndpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytes) {
            OnAsyncReceiveComplete(ec, bytes);
        });
}

void SimpleRtpTransport::OnAsyncReceiveComplete(
    const boost::system::error_code& ec, std::size_t bytes) {
    if (ec) {
        if (ec == boost::asio::error::operation_aborted) {
            // Clean shutdown: StopReceiving() called cancel().
            VCS_GATEWAY_LOG_DEBUG(
                "[DIAG-ReceiveLoop] this=%p, async_receive_from cancelled",
                this);
            return;
        }
        VCS_GATEWAY_LOG_WARNING(
            "[DIAG-ReceiveLoop] this=%p, async_receive_from error: %s",
            this, ec.message().c_str());
        // Do not re-arm on unexpected errors. The next StartReceiving()
        // call will re-establish the receive path; for now exit so
        // StopReceiving() can join().
        return;
    }

    if (bytes > 0) {
        stats_.packetsReceived++;
        stats_.bytesReceived += bytes;
        stats_.lastReceiveTime = GetCurrentTimeMs();

        // [Log rate control] Print statistics every 5 seconds.
        uint64_t nowMs = GetCurrentTimeMs();
        packetCountSinceLastLog_++;
        if (nowMs - lastLogTime_ >= LOG_INTERVAL_MS) {
            VCS_GATEWAY_LOG_INFO(
                "UDP received: last 5s=%llu packets, total=%llu, bytes=%llu",
                (unsigned long long)packetCountSinceLastLog_,
                (unsigned long long)stats_.packetsReceived,
                (unsigned long long)stats_.bytesReceived);
            lastLogTime_ = nowMs;
            packetCountSinceLastLog_ = 0;
        }

        // Forward to the media layer. callback_ is protected by
        // callbackMutex_; we hold it only for the duration of the
        // pointer read, NOT for the callback itself, because the media
        // layer may call back into us (e.g. SetCallback) and we must
        // not deadlock.
        IRtpTransportCallback* cb = nullptr;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            cb = callback_;
        }
        if (cb) {
            RtpPacket packet;
            packet.data = recvBuffer_.data();
            packet.length = bytes;
            VCS_GATEWAY_LOG_TRACE(
                ">>> UDP_RECV: connId=%s size=%zu callback=%p",
                connectionId_.c_str(), bytes, (void*)cb);
            cb->OnRtpReceived(packet, context_);
        } else {
            VCS_GATEWAY_LOG_WARNING(
                ">>> UDP_RECV: callback_ is NULL! Dropping packet. connId=%s",
                connectionId_.c_str());
        }
    }

    // Re-arm for the next packet if we are still supposed to be
    // receiving. The async chain naturally stops when receiving_=false
    // (StopReceiving() flipped it).
    if (receiving_.load() && socket_ && socket_->isOpen()) {
        PostAsyncReceive();
    }
}

}} // namespace vcs::gateway
