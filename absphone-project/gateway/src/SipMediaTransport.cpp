/**
 * @file SipMediaTransport.cpp
 * @brief SIP media transport implementation (RTP passthrough mode)
 *
 * Design principles:
 * 1. PJSIP only does RTP forwarding (socket management), no media codec
 * 2. Media module handles codec and mixing itself
 *
 * Data flow:
 *   Yealink <--RTP--> PJSIP transport --rtp_cb--> Media module
 *                <-- MCU ---> send_rtp --> Yealink
 */

#include "SipMediaTransport.h"
#include "PlatformTime.h"
#include "ILogger.h"

// PJSIP always enabled
#include <pjsua.h>
#include <pjsip_simple.h>
#include <pjmedia.h>
#include <pjmedia/transport.h>
#include <pjmedia/transport_srtp.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pjsua2/call.hpp>
#include <pjsua2/endpoint.hpp>
#include <pjsua2/media.hpp>

#include <cstring>
#include <memory>
#include <cerrno>
#include <chrono>
#include <sstream>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>

namespace vcs { namespace gateway {

namespace {
// Which transport (if any) the current thread is dispatching for. Detach uses
// this to recognise a teardown that originated inside a receive callback:
// waiting for the in-flight count to drain there would wait on itself.
thread_local const void* t_dispatchOwner = nullptr;
thread_local int t_dispatchDepth = 0;

struct DispatchScope {
    explicit DispatchScope(const void* owner) {
        if (t_dispatchDepth++ == 0) t_dispatchOwner = owner;
    }
    ~DispatchScope() {
        if (--t_dispatchDepth == 0) t_dispatchOwner = nullptr;
    }
    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;
};
} // namespace

// ============================================================
// PJSIP RTP callback context
// ============================================================
struct PjsipRtpCallbackContext {
    std::function<void(const uint8_t* data, size_t size)> rtpCallback;
    std::function<void(const uint8_t* data, size_t size)> rtcpCallback;
    SipMediaTransport* transport;  // for statistics
};

// ============================================================
// SipMediaTransport::Impl implementation
// ============================================================
struct SipMediaTransport::Impl {
    // PJSIP media transport pointer (audio)
    pjmedia_transport* pjTransport = nullptr;

    // PJSIP video media transport pointer (H264 video)
    pjmedia_transport* pjVideoTransport = nullptr;

    // Local endpoint, captured ONCE on the SIP thread when the PJSIP
    // transport is bound. GetLocalAddress/GetLocalPort are called from the
    // media/conference threads (RecordStreamShape, UI polling), and pjmedia's
    // get_info is not a thread-safe read: SRTP's variant appends an entry to
    // the caller-owned spc_info array and asserts specific_info_cnt < 4, so
    // the struct must be initialized and the call must not be repeated from
    // arbitrary threads. A cached copy is the only safe shape for a getter.
    std::mutex endpointMutex;
    std::string localAddress;
    uint16_t localPort = 0;

    // Which setter installed the external callbacks (see header). Written by
    // StartReceiving, read by Disconnect.
    ExternalCallbackKind audioExtKind = ExternalCallbackKind::None;
    ExternalCallbackKind videoExtKind = ExternalCallbackKind::None;

    // Connection status
    std::atomic<bool> connected{false};

    // Callback
    IRtpTransportCallback* callback = nullptr;

    // RTP Callback context
    PjsipRtpCallbackContext rtpCtx;

    // Statistics
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> packetsSent{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> packetsFailed{0};
    int64_t lastTxLogMs = 0;

    // Remote address (from PJSIP)
    pj_sockaddr_in remoteAddr;

    // ============================================================
    // Async send queue (solves cross-thread PJSIP call issue)
    // ============================================================
    struct SendTask {
        std::vector<uint8_t> data;
        uint32_t ssrc;
        uint64_t timestamp;
        uint16_t sequenceNumber;
        bool isRtcp = false;
    };

    std::mutex sendQueueMutex;
    std::condition_variable sendQueueCond;
    std::queue<SendTask> sendQueue;
    std::atomic<bool> sendThreadRunning{false};
    std::unique_ptr<std::thread> sendThread;

    // Send thread handler function
    void SendThreadFunc(SipMediaTransport* transport) {
// PJSIP always enabled
        // Register thread to PJSIP (must be done before calling any pjlib function)
        pj_thread_t* thread;
        pj_thread_desc desc;
        pj_bzero(desc, sizeof(desc));
        pj_status_t status = pj_thread_register("rtp_send", desc, &thread);
        if (status != PJ_SUCCESS) {
            VCS_GATEWAY_LOG_ERROR("Failed to register send thread: status=%d", status);
            return;
        }
        VCS_GATEWAY_LOG_INFO("RTP send thread registered with PJSIP");

        while (sendThreadRunning) {
            SendTask task;
            bool hasTask = false;
            size_t queued = 0;
            {
                std::unique_lock<std::mutex> lock(sendQueueMutex);
                sendQueueCond.wait_for(lock, std::chrono::milliseconds(10), [this] {
                    return !sendQueue.empty() || !sendThreadRunning;
                });

                if (!sendThreadRunning) break;
                if (!sendQueue.empty()) {
                    task = std::move(sendQueue.front());
                    sendQueue.pop();
                    hasTask = true;
                }
                queued = sendQueue.size();
            }

            if (hasTask) {
                auto sendOnce = [&](const SendTask& t) {
                    return t.isRtcp
                        ? transport->SendRtcpInternal(t.data.data(), t.data.size())
                        : transport->SendRtpInternal(t.data.data(), t.data.size(),
                                                     t.ssrc, t.timestamp, t.sequenceNumber);
                };

                // Execute sending in PJSIP thread context
                if (sendOnce(task)) {
                    if (!task.isRtcp) {
                        packetsSent++;
                        bytesSent += task.data.size();
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (sendOnce(task)) {
                        if (!task.isRtcp) {
                            packetsSent++;
                            bytesSent += task.data.size();
                        }
                    }
                    else if (!task.isRtcp) {
                        packetsFailed++;
                    }
                }
            }

            // 1 Hz 出线诊断：与 media 的 audio TX diag 对齐时间轴，
            // 用来区分"没编码"与"编码了但没发出去"。
            const int64_t nowMs = static_cast<int64_t>(vcs::base::platform::UtcNowEpochMs());
            if (nowMs - lastTxLogMs >= 1000) {
                lastTxLogMs = nowMs;
                VCS_GATEWAY_LOG_INFO(
                    "[RTP TX] connectionId=%s, sentPkts=%llu, sentBytes=%llu, failedPkts=%llu, queued=%zu",
                    transport->connectionId_.c_str(),
                    static_cast<unsigned long long>(packetsSent.load()),
                    static_cast<unsigned long long>(bytesSent.load()),
                    static_cast<unsigned long long>(packetsFailed.load()),
                    queued);
            }
        }

        VCS_GATEWAY_LOG_INFO("RTP send thread exiting");
    }
};

SipMediaTransport::SipMediaTransport(const std::string& connectionId,
                                     const std::string& remoteAddress,
                                     uint16_t remotePort,
                                     vcs::MediaStreamKind kind)
    : impl_(std::make_unique<Impl>())
    , type_("SIP")
    , connectionId_(connectionId)
    , remoteAddress_(remoteAddress)
    , remotePort_(remotePort)
    , ssrc_(0)
    , kind_(kind)
    , connected_(false)
    , sendDropWarned_(false)
    , dispatchNullWarned_(false)
    , callback_(nullptr) {
    VCS_GATEWAY_LOG_DEBUG("SipMediaTransport created (RTP passthrough): connectionId=%s, kind=%s",
                          connectionId.c_str(), vcs::MediaStreamKindToString(kind));

    // Initialize remote address
    pj_bzero(&impl_->remoteAddr, sizeof(impl_->remoteAddr));
}

SipMediaTransport::~SipMediaTransport() {
    Disconnect();
}

bool SipMediaTransport::Connect() {
    if (connected_) {
        return true;
    }

    VCS_GATEWAY_LOG_INFO("SipMediaTransport connecting (RTP passthrough): connectionId=%s", connectionId_.c_str());

    // Start async send thread (must register to PJSIP)
    impl_->sendThreadRunning = true;
    impl_->sendThread = std::make_unique<std::thread>(&Impl::SendThreadFunc, impl_.get(), this);

    impl_->connected = true;
    connected_ = true;
    sendDropWarned_ = false;

    VCS_GATEWAY_LOG_INFO("SipMediaTransport connected (RTP passthrough): connectionId=%s", connectionId_.c_str());
    return true;
}

void SipMediaTransport::Disconnect() {
	// 先摘收包入口：pjmedia 在自己的线程里派发，必须赶在 pjTransport 指针
	// 置空和回调对象销毁之前断掉，否则拆链窗口里仍会走进悬空的 callback_。
	DetachReceivers();

	impl_->connected = false;
	connected_ = false;

	{
		std::lock_guard<std::mutex> lock(impl_->sendQueueMutex);
		std::queue<Impl::SendTask> empty;
		std::swap(impl_->sendQueue, empty);
	}
	impl_->sendThreadRunning = false;
	impl_->sendQueueCond.notify_all();

	if (impl_->sendThread && impl_->sendThread->joinable()) {
		impl_->sendThread->join();
	}
	impl_->sendThread.reset();

	impl_->pjTransport = nullptr;
	impl_->pjVideoTransport = nullptr;

	VCS_GATEWAY_LOG_INFO("SipMediaTransport disconnected: connectionId=%s", connectionId_.c_str());
}

IRtpTransportCallback* SipMediaTransport::BeginDispatch() {
	std::lock_guard<std::mutex> lock(dispatchMutex_);
	IRtpTransportCallback* cb = callback_.load(std::memory_order_acquire);
	if (cb) {
		++dispatchInFlight_;
	}
	return cb;
}

void SipMediaTransport::EndDispatch() {
	std::lock_guard<std::mutex> lock(dispatchMutex_);
	if (--dispatchInFlight_ == 0) {
		dispatchCv_.notify_all();
	}
}

void SipMediaTransport::DetachReceivers() {
	// 1. 卸载 pjmedia 侧入口。必须用安装时的同一个 API：srtp 变体不校验
	//    transport 类型，UDP 变体按 UDP 布局直写字段，混用会写坏 transport。
	if (impl_->pjTransport && impl_->audioExtKind != ExternalCallbackKind::None) {
		if (impl_->audioExtKind == ExternalCallbackKind::Srtp) {
			pjmedia_transport_srtp_set_external_callback(
				impl_->pjTransport, nullptr, nullptr, nullptr);
		} else {
			pjmedia_transport_set_external_callback(
				impl_->pjTransport, nullptr, nullptr, nullptr);
		}
		impl_->audioExtKind = ExternalCallbackKind::None;
	}
	if (impl_->pjVideoTransport && impl_->videoExtKind != ExternalCallbackKind::None) {
		if (impl_->videoExtKind == ExternalCallbackKind::Srtp) {
			pjmedia_transport_srtp_set_external_callback(
				impl_->pjVideoTransport, nullptr, nullptr, nullptr);
		} else {
			pjmedia_transport_set_external_callback(
				impl_->pjVideoTransport, nullptr, nullptr, nullptr);
		}
		impl_->videoExtKind = ExternalCallbackKind::None;
	}

	// 2. 屏障：先摘指针（此后新包都不再计数），再等在途派发退出。派发期间
	//    不持锁跑用户代码，所以这里等的是计数而不是互斥量。返回后 media
	//    即可销毁回调对象。
	{
		std::unique_lock<std::mutex> lock(dispatchMutex_);
		callback_.store(nullptr, std::memory_order_release);
		impl_->callback = nullptr;
		rtpCallback_ = nullptr;
		rtcpCallback_ = nullptr;
		dispatchNullWarned_ = false;

		if (t_dispatchDepth > 0 && t_dispatchOwner == this) {
			// 回调里直接拆链：不能等自己的这一层，只能保证后续包看不到回调。
			VCS_GATEWAY_LOG_WARNING(
				"SipMediaTransport detached from inside a receive callback, not waiting on self: "
				"connectionId=%s, inFlight=%d",
				connectionId_.c_str(), dispatchInFlight_);
		}
		else {
			dispatchCv_.wait(lock, [this] { return dispatchInFlight_ == 0; });
		}
	}

	VCS_GATEWAY_LOG_INFO("SipMediaTransport receive callbacks detached: connectionId=%s",
		connectionId_.c_str());
}

void* SipMediaTransport::PickEgressTransport() const {
    // Exactly one of the two pointers is set for an instance created through
    // SipAdapter::OnCallConnected; the kind says which one this stream is.
    pjmedia_transport* tp = (kind_ == vcs::MediaStreamKind::Audio)
        ? impl_->pjTransport
        : impl_->pjVideoTransport;
    if (!tp) {
        return nullptr;
    }

    // pjsua always wraps the stack transport in transport_srtp, even when
    // SRTP is negotiated as DISABLED. Sending through the SRTP wrapper on a
    // session where the SRTP session was never started silently drops the
    // datagram, so egress uses the member transport directly.
    //
    // A user-supplied transport (on_create_media_transport) is not wrapped,
    // and pjmedia_transport_srtp_get_member() is an unchecked downcast, so
    // the type must be verified before unwrapping.
    if (tp->type == PJMEDIA_TRANSPORT_TYPE_SRTP) {
        pjmedia_transport* member = pjmedia_transport_srtp_get_member(tp);
        if (member) return static_cast<void*>(member);
    }
    return static_cast<void*>(tp);
}

bool SipMediaTransport::EnqueueSend(const uint8_t* data, size_t size,
                                    uint32_t ssrc, uint64_t timestamp,
                                    uint16_t sequenceNumber, bool isRtcp) {
    if (!connected_ || !impl_->sendThreadRunning) {
        if (!sendDropWarned_.exchange(true)) {
            VCS_GATEWAY_LOG_WARNING(
                "Send dropped: transport not connected: connectionId=%s, kind=%s, rtcp=%d",
                connectionId_.c_str(), vcs::MediaStreamKindToString(kind_), isRtcp ? 1 : 0);
        }
        return false;
    }

    Impl::SendTask task;
    task.data.assign(data, data + size);
    task.ssrc = ssrc;
    task.timestamp = timestamp;
    task.sequenceNumber = sequenceNumber;
    task.isRtcp = isRtcp;

    {
        std::lock_guard<std::mutex> lock(impl_->sendQueueMutex);
        impl_->sendQueue.push(std::move(task));
    }
    impl_->sendQueueCond.notify_one();
    return true;
}

bool SipMediaTransport::SendRtp(const RtpPacket& packet) {
    if (!connected_ || !PickEgressTransport()) {
        // 拆传输→停媒体窗口内媒体线程仍在发包；每个断连窗口只报一次，
        // 既保留"整通没有传输"的可见性，又不做逐包刷屏（A12 量测）。
        if (!sendDropWarned_.exchange(true)) {
            VCS_GATEWAY_LOG_WARNING("SendRtp dropped: not connected or no transport: connectionId=%s, kind=%s",
                                   connectionId_.c_str(), vcs::MediaStreamKindToString(kind_));
        }
        return false;
    }

    // [Design Principle] Parse metadata from RTP header
    // RTP header format: V(2)+P(1)+X(1)+CC(4) | M(1)+PT(7) | Seq(16) | Timestamp(32) | SSRC(32)
    uint32_t ssrc = 0;
    uint64_t timestamp = 0;
    uint16_t sequenceNumber = 0;
    if (packet.length >= 12) {
        sequenceNumber = ntohs(*(uint16_t*)(packet.data + 2));   // Seq: bytes 2-3
        timestamp = ntohl(*(uint32_t*)(packet.data + 4));        // Timestamp: bytes 4-7
        ssrc = ntohl(*(uint32_t*)(packet.data + 8));             // SSRC: bytes 8-11
    }

    return EnqueueSend(packet.data, packet.length, ssrc, timestamp, sequenceNumber, false);
}

// Internal send method (called by send thread, executed in PJSIP thread context)
bool SipMediaTransport::SendRtpInternal(const uint8_t* data, size_t length,
                                       uint32_t /*ssrc*/, uint64_t /*timestamp*/,
                                       uint16_t /*sequenceNumber*/) {
// PJSIP always enabled
    pjmedia_transport* transportToUse =
        static_cast<pjmedia_transport*>(PickEgressTransport());

    if (!connected_ || !transportToUse) {
        VCS_GATEWAY_LOG_DEBUG("SendRtpInternal: not connected or no transport (send thread draining)");
        return false;
    }

    // Send RTP packet via PJSIP transport
    VCS_GATEWAY_LOG_TRACE("SendRtpInternal: sending RTP, transport=%p, size=%zu", (void*)transportToUse, length);
    pj_status_t status = pjmedia_transport_send_rtp(
        transportToUse,
        (const pjmedia_rtp_hdr*)data,
        length
    );

    if (status != PJ_SUCCESS) {
        // 拆链瞬间 pjmedia 可能返回超时等瞬时错误，带状态码留 WARN 可诊断
        VCS_GATEWAY_LOG_WARNING("Failed to send RTP: status=%d", status);
        return false;
    }
    VCS_GATEWAY_LOG_DEBUG("SendRtpInternal: RTP sent successfully, size=%zu", length);
    return true;
}

bool SipMediaTransport::SendRtcpInternal(const uint8_t* data, size_t length) {
    pjmedia_transport* transportToUse =
        static_cast<pjmedia_transport*>(PickEgressTransport());
    if (!connected_ || !transportToUse) {
        return false;
    }

    pj_status_t status = pjmedia_transport_send_rtcp(transportToUse, data, length);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_WARNING("Failed to send RTCP: status=%d, connectionId=%s, kind=%s, len=%zu",
                                status, connectionId_.c_str(),
                                vcs::MediaStreamKindToString(kind_), length);
        return false;
    }
    return true;
}

bool SipMediaTransport::SendRtcp(const RtcpPacket& packet) {
    if (!connected_ || !packet.data || packet.length == 0) {
        return false;
    }
    // RTCP (including FIR) goes through the same pjlib-registered send thread
    // as RTP. pjmedia's SRTP/ioqueue path must not be entered from the media
    // or conference thread.
    return EnqueueSend(packet.data, packet.length, 0, 0, 0, true);
}

bool SipMediaTransport::SendFir(uint32_t remoteSsrc, uint32_t localSsrc) {
    if (!connected_ || !PickEgressTransport()) {
        VCS_GATEWAY_LOG_WARNING("SendFir: transport not connected: connectionId=%s, kind=%s",
                                connectionId_.c_str(), vcs::MediaStreamKindToString(kind_));
        return false;
    }

    // 20-byte RFC 5104 FIR (PSFB, FMT=4):
    //   byte 0    : V=2 | P=0 | FMT=4        => 0x84
    //   byte 1    : PT = 206 (PSFB)
    //   bytes 2-3 : length = 4 (20/4 - 1)
    //   bytes 4-7 : SSRC of packet sender (local)
    //   bytes 8-11: SSRC of media source (remote)
    //   bytes 12-15: FCI - SSRC of media source (repeated)
    //   byte 16   : FIR sequence number
    //   bytes 17-19: reserved
    uint8_t packet[20] = {0};
    packet[0] = 0x84;
    packet[1] = 206;
    packet[2] = 0x00;
    packet[3] = 0x04;

    auto writeBE32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[3] = static_cast<uint8_t>(v & 0xFF);
    };
    writeBE32(packet + 4, localSsrc);
    writeBE32(packet + 8, remoteSsrc);
    writeBE32(packet + 12, remoteSsrc);

    static std::atomic<uint8_t> s_firSeq{0};
    packet[16] = s_firSeq.fetch_add(1, std::memory_order_relaxed);

    const bool queued = EnqueueSend(packet, sizeof(packet), 0, 0, 0, true);
    if (queued) {
        VCS_GATEWAY_LOG_INFO("[FIR] queued: connectionId=%s, kind=%s, remoteSsrc=0x%08X, localSsrc=0x%08X",
                             connectionId_.c_str(), vcs::MediaStreamKindToString(kind_),
                             remoteSsrc, localSsrc);
    }
    return queued;
}

bool SipMediaTransport::SetCallback(IRtpTransportCallback* callback) {
    std::unique_lock<std::mutex> lock(dispatchMutex_);

    VCS_GATEWAY_LOG_DEBUG("SetCallback: connectionId=%s, callback=%p (old=%p)",
                          connectionId_.c_str(), (void*)callback,
                          (void*)callback_.load(std::memory_order_relaxed));
    if (callback && callback_.load(std::memory_order_relaxed) && callback != callback_) {
        // 正常流程不应覆盖回调；真出现说明会话/传输对象错位，保留 WARN
        VCS_GATEWAY_LOG_WARNING("SetCallback overwrites existing callback: connectionId=%s, old=%p, new=%p",
                                connectionId_.c_str(), (void*)callback_.load(std::memory_order_relaxed),
                                (void*)callback);
    }

    if (callback) {
        dispatchNullWarned_ = false;
    }
    callback_.store(callback, std::memory_order_release);
    impl_->callback = callback;

    // 传 nullptr 表示调用方马上要销毁回调对象：必须等在途派发退出。
    // 从回调内部自摘时不等待（否则等自己），只保证后续包看不到回调。
    if (!callback) {
        rtpCallback_ = nullptr;
        rtcpCallback_ = nullptr;
        if (t_dispatchDepth > 0 && t_dispatchOwner == this) {
            VCS_GATEWAY_LOG_WARNING(
                "SetCallback(nullptr) from inside a receive callback, not waiting on self: "
                "connectionId=%s, inFlight=%d", connectionId_.c_str(), dispatchInFlight_);
        }
        else {
            dispatchCv_.wait(lock, [this] { return dispatchInFlight_ == 0; });
        }
    }
    return true;
}

void SipMediaTransport::SetContext(const vcs::SessionContext& context) {
    context_ = context;
}

const vcs::SessionContext& SipMediaTransport::GetContext() const {
    return context_;
}

bool SipMediaTransport::IsReceiving() const {
    return connected_;
}

std::string SipMediaTransport::GetRemoteAddress() const {
    std::ostringstream oss;
    oss << remoteAddress_ << ":" << remotePort_;
    return oss.str();
}

std::string SipMediaTransport::GetLocalAddress() const {
    std::lock_guard<std::mutex> lock(impl_->endpointMutex);
    return impl_->localAddress.empty() ? "0.0.0.0" : impl_->localAddress;
}

uint16_t SipMediaTransport::GetLocalPort() const {
    std::lock_guard<std::mutex> lock(impl_->endpointMutex);
    return impl_->localPort;
}

void SipMediaTransport::CaptureLocalEndpoint() {
    // Prefer the audio pointer, fall back to the video one: each
    // SipMediaTransport instance carries exactly one of them.
    pjmedia_transport* tp = impl_->pjTransport ? impl_->pjTransport
                                               : impl_->pjVideoTransport;
    if (!tp) {
        return;
    }

    // pjmedia requires the caller to initialise this struct. SRTP's get_info
    // asserts info->specific_info_cnt < 4 and then appends its own entry; an
    // uninitialised stack struct made every call a coin flip on that assert.
    pjmedia_transport_info info;
    pjmedia_transport_info_init(&info);
    if (pjmedia_transport_get_info(tp, &info) != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_WARNING(
            "%s: local endpoint unavailable (get_info failed)",
            connectionId_.c_str());
        return;
    }

    char addr[PJ_INET_ADDRSTRLEN] = {0};
    pj_inet_ntop(PJ_AF_INET,
        pj_sockaddr_get_addr(&info.sock_info.rtp_addr_name),
        addr, sizeof(addr));
    const uint16_t port = pj_sockaddr_get_port(&info.sock_info.rtp_addr_name);

    {
        std::lock_guard<std::mutex> lock(impl_->endpointMutex);
        impl_->localAddress = addr;
        impl_->localPort = port;
    }
    VCS_GATEWAY_LOG_INFO("%s: local endpoint %s:%u",
        connectionId_.c_str(), addr, static_cast<unsigned>(port));
}

void SipMediaTransport::SetSsrc(uint32_t ssrc) {
    ssrc_ = ssrc;
}

void SipMediaTransport::SetLocalAddress(const std::string& address, uint16_t port) {
    // In RTP passthrough mode, local address is managed by PJSIP, no need to set
    VCS_GATEWAY_LOG_DEBUG("SetLocalAddress called but local address is managed by PJSIP");
}

uint64_t SipMediaTransport::GetPacketsReceived() const {
    return impl_->packetsReceived;
}

uint64_t SipMediaTransport::GetPacketsSent() const {
    return impl_->packetsSent;
}

uint64_t SipMediaTransport::GetBytesReceived() const {
    return impl_->bytesReceived;
}

uint64_t SipMediaTransport::GetBytesSent() const {
    return impl_->bytesSent;
}

std::string SipMediaTransport::GetRtpAddress() const {
    return remoteAddress_;
}

uint16_t SipMediaTransport::GetRtpPort() const {
    return remotePort_;
}

// [V5.5 Deleted] SendVideoFrame legacy interface
// RTP encapsulation has been moved to Media layer (RTPEngine::SendVideoRtp)
// Gateway layer only handles RTP passthrough (SendRtp interface)

// ============================================================
// Set PJSIP transport (called by SipSignalingHandler)
// ============================================================

// Helper function: attach the negotiated remote address to the PJSIP media transport
//
// Why this is required: MakeCall() sets pjsua media direction to NONE so that
// pjmedia neither encodes nor receives (the real media plane is our external
// RTPEngine). That also means pjsua never creates a stream, and the stream was
// what called pjmedia_transport_attach() — the only place that sets the UDP
// member's rem_rtp_addr and wires the receive callbacks. Without the attach,
// pjmedia_transport_send_rtp() fails with the system error for an invalid
// destination (WSAEFAULT / 130014) and inbound RTP has no callback.
//
// Attach the transport pjsua gave us (the SRTP wrapper) rather than its UDP
// member: transport_srtp forwards the address to the member while keeping its
// own rtp_cb2, which is what dispatches our external passthrough callback.
static bool AttachRemoteAddress(pjmedia_transport* srtpTransport,
                                const std::string& remoteAddress,
                                uint16_t remotePort,
                                const char* logPrefix) {
// PJSIP always enabled
    if (!srtpTransport) return false;

    // Get the underlying UDP transport of SRTP transport
    pjmedia_transport* udpTransport = pjmedia_transport_srtp_get_member(srtpTransport);
    if (!udpTransport) {
        VCS_GATEWAY_LOG_ERROR("%s: No underlying UDP transport found in SRTP transport!", logPrefix);
        return false;
    }

    VCS_GATEWAY_LOG_INFO("%s: transport=%p (udp member=%p), attaching remote address %s:%u",
                        logPrefix, (void*)srtpTransport, (void*)udpTransport,
                        remoteAddress.c_str(), remotePort);

    // Try to get actual remote address from transport (from received packets)
    pjmedia_transport_info tpInfo;
    pjmedia_transport_info_init(&tpInfo);
    pj_status_t status = pjmedia_transport_get_info(udpTransport, &tpInfo);

    // Determine which address to use
    std::string useAddr = remoteAddress;
    uint16_t usePort = remotePort;

    if (status == PJ_SUCCESS && pj_sockaddr_has_addr(&tpInfo.src_rtp_name)) {
        // Use the source address from received packets
        char addrBuf[64];
        pj_inet_ntop(PJ_AF_INET, pj_sockaddr_get_addr(&tpInfo.src_rtp_name),
                     addrBuf, sizeof(addrBuf));
        uint16_t port = pj_sockaddr_get_port(&tpInfo.src_rtp_name);
        VCS_GATEWAY_LOG_INFO("%s: Using detected remote address from received packets: %s:%u",
                            logPrefix, addrBuf, port);
        usePort = port;
        useAddr = addrBuf;
    }

    // Set remote address (using pj_sockaddr)
    pj_sockaddr remAddr;
    pj_str_t addrStr = pj_str(const_cast<char*>(useAddr.c_str()));
    pj_status_t addrStatus = pj_sockaddr_init(PJ_AF_INET, &remAddr, &addrStr, usePort);
    if (addrStatus != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("%s: Failed to init remote address %s:%u: status=%d",
                            logPrefix, useAddr.c_str(), usePort, addrStatus);
        return false;
    }

    // Call attach2 to set remote address (this is the key fix!)
    // attach() sets the member's rem_rtp_addr, after which send_rtp() can send correctly
    pjmedia_transport_attach_param attParam;
    pj_bzero(&attParam, sizeof(attParam));
    attParam.user_data = nullptr;  // No stream callbacks; receive runs via external callback
    pj_sockaddr_cp(&attParam.rem_addr, &remAddr);
    attParam.addr_len = pj_sockaddr_get_len(&remAddr);
    attParam.rtp_cb = nullptr;
    attParam.rtp_cb2 = nullptr;
    attParam.rtcp_cb = nullptr;

    status = pjmedia_transport_attach2(srtpTransport, &attParam);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("%s: Failed to attach remote address: status=%d", logPrefix, status);
        return false;
    }

    VCS_GATEWAY_LOG_INFO("%s: Successfully attached remote address %s:%u",
                        logPrefix, useAddr.c_str(), usePort);

    // [Critical Fix] Start the UDP transport media session.
    // PJSIP's normal pipeline calls pjmedia_transport_media_start() when creating a stream,
    // which sets udp->started=TRUE and starts async recv operations. Our bypass (direction=NONE)
    // skips stream creation, so we must call media_start() manually. Without this:
    // 1. udp->started remains FALSE, causing send_rtp() to return SUCCESS without sending
    // 2. No async recv operations are started, so incoming RTP/RTCP packets are not received
    status = pjmedia_transport_media_start(udpTransport, nullptr, nullptr, nullptr, 0);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("%s: Failed to start UDP transport media: status=%d", logPrefix, status);
        return false;
    }

    VCS_GATEWAY_LOG_INFO("%s: UDP transport media started (enabled send/recv)", logPrefix);
    return true;
}

void SipMediaTransport::SetPjMediaTransport(void* transport) {
    if (kind_ != vcs::MediaStreamKind::Audio) {
        // One pjmedia transport carries one external-callback slot. Binding it
        // from a wrapper of another kind would redirect that stream's receive
        // path into this object and starve its real owner.
        VCS_GATEWAY_LOG_ERROR(
            "SetPjMediaTransport rejected: instance kind=%s, audio transport can only be bound to an audio wrapper",
            vcs::MediaStreamKindToString(kind_));
        return;
    }

    impl_->pjTransport = static_cast<pjmedia_transport*>(transport);

    if (impl_->pjTransport) {
        // pjmedia 的 UDP transport 只有在 attach 之后才有 rem_rtp_addr 和收包
        // 回调；pjsua 媒体方向为 NONE 时它不建流，也就不会替我们 attach。
        AttachRemoteAddress(impl_->pjTransport, remoteAddress_, remotePort_, "SetPjMediaTransport");
        CaptureLocalEndpoint();

        VCS_GATEWAY_LOG_INFO("PJSIP audio transport set: connectionId=%s",
                            connectionId_.c_str());
    }
}

void SipMediaTransport::SetPjVideoTransport(void* transport) {
// PJSIP always enabled
    if (kind_ != vcs::MediaStreamKind::Video &&
        kind_ != vcs::MediaStreamKind::SecondaryVideo) {
        VCS_GATEWAY_LOG_ERROR(
            "SetPjVideoTransport rejected: instance kind=%s, video transport can only be bound to a video wrapper",
            vcs::MediaStreamKindToString(kind_));
        return;
    }

    impl_->pjVideoTransport = static_cast<pjmedia_transport*>(transport);

    if (impl_->pjVideoTransport) {
        // 同音频：pjsua 不建流，远端地址与收包回调必须由我们 attach
        AttachRemoteAddress(impl_->pjVideoTransport, remoteAddress_, remotePort_, "SetPjVideoTransport");
        CaptureLocalEndpoint();

        VCS_GATEWAY_LOG_INFO("PJSIP video transport set: connectionId=%s",
                            connectionId_.c_str());
    }
}

void* SipMediaTransport::GetBoundPjMediaTransport() const {
	// Mirrors PickEgressTransport's selection; kept separate because egress
	// unwraps the SRTP wrapper and this reports the bound pjmedia object.
	return static_cast<void*>((kind_ == vcs::MediaStreamKind::Audio)
		? impl_->pjTransport
		: impl_->pjVideoTransport);
}

bool SipMediaTransport::StartReceiving() {
    // In RTP passthrough mode, use PJSIP external callback
    VCS_GATEWAY_LOG_INFO("StartReceiving called - setting up RTP callback for passthrough mode");

// PJSIP always enabled
    if (impl_->pjTransport) {
        // Use SRTP-specific external callback function
        // This function calls external callback before SRTP processing, allowing us to receive RTP packets without decryption
        pj_status_t status = pjmedia_transport_srtp_set_external_callback(
            impl_->pjTransport,
            [](void *user_data, void *pkt, pj_ssize_t len) {
                auto* transport = static_cast<SipMediaTransport*>(user_data);
                if (transport && len > 0) {
                    const uint8_t* data = static_cast<const uint8_t*>(pkt);
                    transport->OnRtpReceived(data, static_cast<size_t>(len));
                }
            },
            [](void *user_data, void *pkt, pj_ssize_t len) {
                auto* transport = static_cast<SipMediaTransport*>(user_data);
                if (transport && len > 0) {
                    const uint8_t* data = static_cast<const uint8_t*>(pkt);
                    transport->OnRtcpReceived(data, static_cast<size_t>(len));
                }
            },
            this
        );

        if (status == PJ_SUCCESS) {
            impl_->audioExtKind = ExternalCallbackKind::Srtp;
            VCS_GATEWAY_LOG_INFO("SRTP external callback set successfully - RTP passthrough enabled");
        } else {
            VCS_GATEWAY_LOG_ERROR("Failed to set SRTP callback: status=%d, trying UDP transport", status);

            // Fallback to UDP transport generic function
            status = pjmedia_transport_set_external_callback(
                impl_->pjTransport,
                [](void *user_data, void *pkt, pj_ssize_t len) {
                    auto* transport = static_cast<SipMediaTransport*>(user_data);
                    if (transport && len > 0) {
                        const uint8_t* data = static_cast<const uint8_t*>(pkt);
                        transport->OnRtpReceived(data, static_cast<size_t>(len));
                    }
                },
                [](void *user_data, void *pkt, pj_ssize_t len) {
                    auto* transport = static_cast<SipMediaTransport*>(user_data);
                    if (transport && len > 0) {
                        const uint8_t* data = static_cast<const uint8_t*>(pkt);
                        transport->OnRtcpReceived(data, static_cast<size_t>(len));
                    }
                },
                this
            );

            if (status == PJ_SUCCESS) {
                impl_->audioExtKind = ExternalCallbackKind::Udp;
                VCS_GATEWAY_LOG_INFO("UDP external callback set successfully");
            } else {
                VCS_GATEWAY_LOG_ERROR("Failed to set UDP callback too: status=%d", status);
            }
        }
    }

    // Set external callback for video transport (H264 video)
    // Important: Video media is SRTP transport, must use pjmedia_transport_srtp_set_external_callback
    // instead of pjmedia_transport_set_external_callback (that is for UDP)
    if (impl_->pjVideoTransport) {
        VCS_GATEWAY_LOG_INFO("Setting up VIDEO transport external callback for H264 (SRTP mode)");

        // Key fix: use SRTP transport-specific external callback function
        pj_status_t status = pjmedia_transport_srtp_set_external_callback(
            impl_->pjVideoTransport,
            [](void *user_data, void *pkt, pj_ssize_t len) {
                auto* transport = static_cast<SipMediaTransport*>(user_data);
                if (transport && len > 0) {
                    const uint8_t* data = static_cast<const uint8_t*>(pkt);
                    transport->OnRtpReceived(data, static_cast<size_t>(len));
                }
            },
            [](void *user_data, void *pkt, pj_ssize_t len) {
                auto* transport = static_cast<SipMediaTransport*>(user_data);
                if (transport && len > 0) {
                    const uint8_t* data = static_cast<const uint8_t*>(pkt);
                    transport->OnRtcpReceived(data, static_cast<size_t>(len));
                }
            },
            this
        );

		if (status == PJ_SUCCESS) {
			impl_->videoExtKind = ExternalCallbackKind::Srtp;
			VCS_GATEWAY_LOG_INFO("Video transport external callback set successfully (SRTP mode)");
		}
		else {
			VCS_GATEWAY_LOG_WARNING("SRTP video callback failed (status=%d), falling back to UDP", status);
			status = pjmedia_transport_set_external_callback(
				impl_->pjVideoTransport,
				[](void *user_data, void *pkt, pj_ssize_t len) {
				auto* transport = static_cast<SipMediaTransport*>(user_data);
				if (transport && len > 0) {
					transport->OnRtpReceived(static_cast<const uint8_t*>(pkt), static_cast<size_t>(len));
				}
			},
				[](void *user_data, void *pkt, pj_ssize_t len) {
				auto* transport = static_cast<SipMediaTransport*>(user_data);
				if (transport && len > 0) {
					transport->OnRtcpReceived(static_cast<const uint8_t*>(pkt), static_cast<size_t>(len));
				}
			},
				this
				);
			if (status == PJ_SUCCESS) {
				impl_->videoExtKind = ExternalCallbackKind::Udp;
				VCS_GATEWAY_LOG_INFO("Video UDP external callback set successfully");
			}
			else {
				VCS_GATEWAY_LOG_ERROR("Failed to set video callback (UDP fallback): status=%d", status);
			}
		}
    }

    return true;
}

void SipMediaTransport::StopReceiving() {
	VCS_GATEWAY_LOG_INFO("StopReceiving called -> Disconnect");
	Disconnect();
}

void SipMediaTransport::OnRtpReceived(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;

    impl_->packetsReceived++;
    impl_->bytesReceived += size;

    // [Debug log]Print RTP receive information
    uint8_t pt = (size >= 2) ? (data[1] & 0x7F) : 0;
    uint16_t seq = (size >= 4) ? ntohs(*(uint16_t*)(data + 2)) : 0;
    uint32_t ssrc = (size >= 8) ? ntohl(*(uint32_t*)(data + 8)) : 0;
    VCS_GATEWAY_LOG_TRACE(">>> RTP_RECEIVED: kind=%s, pt=%u, seq=%u, ssrc=0x%08x, size=%zu",
                         vcs::MediaStreamKindToString(kind_), pt, seq, ssrc, size);

    // Gateway layer only does RTP forwarding, does not process media
    // Media layer is responsible for encoding/decoding, NAL parsing and other media processing

    // Register as in-flight, then call with no transport lock held. Detach
    // drains the count before returning, which is what lets media destroy the
    // callback object right after SetCallback(nullptr).
    IRtpTransportCallback* cb = BeginDispatch();
    if (cb) {
        // Guard first, scope second: the scope must clear the thread-local
        // dispatch marker before EndDispatch notifies the detach waiter.
        DispatchGuard guard(*this);
        DispatchScope scope(this);

        RtpPacket packet;
        packet.data = const_cast<uint8_t*>(data);
        packet.length = size;

        // Gateway layer only forwards; the stream kind comes from this
        // instance, not from a payload-type guess.
        VCS_GATEWAY_LOG_DEBUG(">>> RTP_CALLBACK: calling OnRtpReceived (kind=%s)",
                              vcs::MediaStreamKindToString(kind_));
        cb->OnRtpReceived(packet, context_);
    }
	else if (!dispatchNullWarned_.exchange(true)) {
		// 拆链窗口内仍有一个包到达：只报一次，避免逐包刷屏。
		VCS_GATEWAY_LOG_WARNING(">>> RTP_CALLBACK: no media callback attached (teardown window): connectionId=%s",
			connectionId_.c_str());
	}
}

void SipMediaTransport::OnRtcpReceived(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;

    // RTCP was previously delivered only to the legacy std::function hook,
    // which nobody sets - so inbound PLI/FIR silently died here. Dispatch to
    // the media layer through the transport contract instead, under the same
    // in-flight/barrier contract as RTP.
    IRtpTransportCallback* cb = BeginDispatch();
    if (cb) {
        DispatchGuard guard(*this);
        DispatchScope scope(this);

        RtcpPacket packet;
        packet.data = const_cast<uint8_t*>(data);
        packet.length = size;
        cb->OnRtcpReceived(packet, context_);
        return;
    }

    // Legacy hook (unused today). Copied under the lock, invoked without it,
    // so detach cannot clear the callable while it is being entered.
    std::function<void(const uint8_t*, size_t)> legacy;
    {
        std::lock_guard<std::mutex> lock(dispatchMutex_);
        legacy = rtcpCallback_;
    }
    if (legacy) {
        legacy(data, size);
    }
}

} // namespace gateway
} // namespace vcs
