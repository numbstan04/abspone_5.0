/**
 * @file RTPEngine.cpp
 * @brief RTP transport engine implementation
 *
 * Responsible for RTP/RTCP packet sending/receiving and dispatching.
 * [Design Principle] Frame assembly, reordering, dejitter, etc. are completed in RTPEngine,
 * VideoProcessor only handles decoding/composition/encoding.
 *
 * [Q5 architecture]
 * After Q5 step 5d-receive, the receive data plane is a two-hop chain:
 *
 *   gateway thread / MediaWorker
 *      \-> RTPEngine::OnRtpReceived(memberId, RtpPacket)      (public, gateway-facing)
 *            \-> backend_->OnRtpReceived(memberId, data, len)  (interface entry point)
 *                  \-> NativeRtp::Ingress::ParseRtpHeader     (12-byte + CSRC + extension)
 *                        \-> orchestrator's RtpReceiveCallback (installed in Initialize)
 *                              \-> RTPEngine::HandleParsedRtp (this file)
 *                                   |-> SSRC routing (members / backend's ssrcToMember_)
 *                                   |-> H.264 FU-A depacketization
 *                                    \-> dispatch to audio / video / dual callback
 *
 * RTPEngine is now an *orchestrator* of the receive path: it does not
 * parse the RTP header any more, and it does not own the depacketizer
 * buffer pool any more (the depacketizer is still here, but as a
 * session-level member of the Impl struct  -  same as before). What
 * changed is that the parse step is delegated to the backend and the
 * dispatch step is the registered callback.
 */

#include "RTPEngine.h"
#include "BackendFactory.h"   // [Q5] default backend construction
#include "IRtpBackend.h"       // [Q5] injected backend interface
#include "IConfig.h"           // [V7.0+] vcs::base::IConfig - source of the h264-dump switch
#include "PlatformTime.h"
#include "ILogger.h"
#include <algorithm>
#include <atomic>              // [V7.0] std::atomic for the per-process FIR seq counter
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdio>               // fopen/fwrite/fclose/snprintf  -  [h264-dump] group-frame H.264 NALU file dump
#include <cstring>             // memcpy  -  for header reconstruction in HandleParsedRtp
#include <memory>              // std::make_unique for the pimpl
#include <cerrno>              // errno  -  [h264-dump] fopen-failure diagnostics
#include <string>              // std::string for memberId-keyed file name

// [h264-dump feature] Per-member H.264 byte-stream dump, gated by a
// runtime switch so it is OFF by default and incurs no log/file cost.
//
// The switch + output directory are read from the IConfig instance
// passed to RTPEngine::Initialize(MediaConfig, IConfig). Recognised keys:
//
//   media.debug.h264_dump.enable  (bool,   default false)
//   media.debug.h264_dump.dir     (string, default "G:/tmp")
//
// The directory is only consulted when enable is true. The values are
// cached in Impl::h264DumpEnabled_ / Impl::h264DumpDir_ at Initialize
// time, so the per-packet cost is a single bool load.

namespace {
// Built-in default for the h264-dump output directory. Used only when
// the operator enables the feature but leaves the dir key empty. The
// path is Windows-friendly (fopen on Windows accepts forward slashes)
// and also works in WSL.
constexpr const char* kDefaultH264DumpDir = "G:/tmp";
} // namespace

using namespace vcs::base;

// Get current time (milliseconds) - cross-platform using Boost or standard library
static uint64_t GetCurrentTimeMs() {
    return vcs::base::platform::UtcNowEpochMs();
}

namespace {

// [Q5 step 5d-video] H.264 FU-A fragmentation has been moved to
// NativeRtpBackend.cpp (namespace NativeRtp::Fragment). The orchestrator
// no longer fragments video itself  -  SendVideoRtp hands a whole NALU
// to backend_->SendH264Nalu() and the backend takes care of the
// single-NAL vs FU-A decision, marker-bit logic and per-fragment RTP
// header construction.
//
// [Q5 step 5d-receive] The RTP header parse + demux is also gone
// from this file  -  it now lives in NativeRtp::Ingress::ParseRtpHeader
// (NativeRtpBackend.cpp). The orchestrator only sees the *parsed*
// (pt, ssrc, ts, seq, payload, payloadLen) tuple. This anonymous
// namespace is intentionally left empty for now; new file-local
// helpers can be added here in the future.

} // anonymous namespace

namespace vcs { namespace media {

// ============================================================
// RTP payload type definitions
// ============================================================
enum : uint8_t {
    PAYLOAD_PCMU = 0,
    PAYLOAD_PCMA = 8,
    PAYLOAD_G722 = 9,
    PAYLOAD_H264 = 97,
    PAYLOAD_VP8 = 100,
    PAYLOAD_VP9 = 101,
    PAYLOAD_OPUS = 111,
    PAYLOAD_H265 = 102,
};

// ============================================================
// H.264 FU-A Depacketizer
// [Q5 step 5f-receive] MIGRATED to NativeRtpBackend.cpp
// The depacketizer is now a backend responsibility  -  see
// namespace NativeRtp::Ingress::H264 in NativeRtpBackend.cpp and the
// NativeRtpBackend::DepacketizeH264 / ClearH264Depacketizer /
// ClearAllH264Depacketizers methods.
//
// RTPEngine no longer owns the depacketizer buffer pool; it just
// calls backend_->DepacketizeH264(...) from HandleParsedRtp and pushes
// the resulting (complete) NAL unit into the video frame queue.
// ============================================================

// ============================================================
// Internal implementation
// ============================================================
struct RTPEngine::Impl {
    MediaConfig config;
    std::unordered_map<std::string, MemberMediaContext> members;

    // [Q5] The injected IRtpBackend. After Q5 step 5d, the data
    // plane (send and receive) is fully forwarded: SendAudioRtp,
    // SendVideoRtp, SendRtcp, OnRtpReceived and OnRtcpReceived are
    // all thin forwarders to backend_. RTPEngine keeps ownership of
    // the SSRC space, the member context, the statistics counters,
    // and the injection-mode escape hatch. After Q5 step 5f, the
    // H.264 FU-A depacketizer also lives in the backend
    // (NativeRtpBackend::DepacketizeH264). After Q5 step 5g, the
    // backend's ssrcToMember_ is the single source of truth for SSRC
    // routing  -  the legacy impl_->ssrcToMember double-write was
    // removed. The orchestrator's role is now strictly "thin
    // forwarding + business-level state".
    std::unique_ptr<IRtpBackend> backend_;

    //  Determine if PT is video/audio (based on configured PT values)
    bool ListHasVideoPt(uint8_t pt) const {
        for (uint8_t videoPt : config.videoPayloadTypes) {
            if (pt == videoPt) return true;
        }
        return false;
    }

    bool ListHasAudioPt(uint8_t pt) const {
        for (uint8_t audioPt : config.audioPayloadTypes) {
            if (pt == audioPt) return true;
        }
        return false;
    }

    enum class PayloadKind { Unknown, Audio, Video };

    //  Classification used by the receive path. The PT negotiated for this
    // member (from SDP) wins over the static MediaConfig lists: peers are
    // free to answer with a PT the lists do not contain (e.g. H264 on 96),
    // and a negotiated audio PT may collide with a list entry of the other
    // kind (Opus on 100 vs VP8 on 100).
    PayloadKind ClassifyPayload(const MemberMediaContext& ctx, uint8_t pt) const {
        if (ctx.negotiatedAudioPt >= 0 &&
            pt == static_cast<uint8_t>(ctx.negotiatedAudioPt)) {
            return PayloadKind::Audio;
        }
        if (ctx.negotiatedVideoPt >= 0 &&
            pt == static_cast<uint8_t>(ctx.negotiatedVideoPt)) {
            return PayloadKind::Video;
        }
        if (ListHasVideoPt(pt)) return PayloadKind::Video;
        if (ListHasAudioPt(pt)) return PayloadKind::Audio;
        return PayloadKind::Unknown;
    }

    // Output callbacks
    RtpPacketCallback audioCallback;
    RtpPacketCallback videoCallback;
    RtpPacketCallback dualCallback;
    RtcpCallback rtcpCallback;

    // [WebRTC media engine] Playout PCM produced by the engine's ADM.
    // Wired by MediaControl to AudioProcessor::RenderPcm.
    std::function<void(const std::string&, const int16_t*, size_t, int, int)>
        audioPlayoutPcmCallback;

    // Inbound keyframe request (PLI/FIR) handler, wired by MediaControl to
    // the video encoder. Protected by the same mutex as rtcpCallback.
    RemoteKeyFrameRequestCallback keyFrameRequestCallback;

    // Per-kind throttle so a peer repeating PLI cannot push the encoder into
    // back-to-back IDR generation. Written from the transport receive thread.
    std::atomic<int64_t> lastMainKeyFrameRequestMs{0};
    std::atomic<int64_t> lastSecondaryKeyFrameRequestMs{0};

    // Media send callback (injection mode)
    // When this callback is set, send via this callback instead of transport->SendRtp()
    MediaSendCallback mediaSendCallback;

    // Statistics collector
    std::unique_ptr<MediaStatisticsCollector> statsCollector;

    // [Q5 step 5g] The legacy sequence-number seed map and the
    // audioSentCount / videoSentCount / audioBytesSent / videoBytesSent
    // counters were removed. Sequence numbers are now generated by the
    // backend (NativeRtp::Egress::BuildRtpDatagram / FragmentAndSendH264
    // walk them through with the caller-supplied start value), and the
    // canonical per-member statistics live on NativeRtpBackend and
    // MediaStatisticsCollector.

    // ============================================================
    // Video frame queue pool
    // RTPEngine as producer, pushes video frames to queue
    // VideoProcessor as consumer, fetches frames from queue
    // ============================================================
    std::shared_ptr<VideoFrameQueuePool> videoQueuePool;

    // Separate locks: mutex_ for member management, queueMutex_ for queue pool
    // This is industry recommended practice, avoid using recursive_mutex
    mutable std::mutex mutex;           // Protect members, callbacks, etc.
    mutable std::mutex queueMutex_;    // Specifically protect videoQueuePool

    // ============================================================
    // [V7.0+] H.264 dump feature (per-member .h264 byte-stream dump)
    // --------------------------------------------------------------
    // Default-OFF. Read once from the IConfig passed to the two-arg
    // Initialize overload; thereafter the runtime check is a single
    // bool load. The directory string is consulted only when
    // h264DumpEnabled_ is true. An empty h264DumpDir_ means "use
    // built-in default" (resolved inside HandleParsedRtp).
    // ============================================================
    bool        h264DumpEnabled_ = false;
    std::string h264DumpDir_;          // empty == "use built-in default"

    // [V7.0+] Receive gate. BeginReceive() flips this to true and
    // installs the backend receive callbacks. StopReceive() flips it
    // back to false and clears the callbacks. Initialized to true
    // because Initialize() (the 2-arg form) installs the callbacks
    // eagerly for backward compatibility with the pre-V7 call sites;
    // a future refactor that wants to enforce the "no receive before
    // BeginReceive" contract can flip this default to false once
    // every caller has been audited.
    bool        isReceiving_ = true;

    // ============================================================
    // Periodic RTCP Sender Report timer
    // ----------------------------------------------------------
    // RFC 3550 Section 6.2: every active sender MUST send RTCP SR
    // periodically so receivers (and MCUs) can identify the SSRC and
    // synchronise the RTP clock with wall-clock time. Without SR the
    // MCU does not recognise this endpoint as an active sender and
    // will not include its uplink audio in the conference mix.
    //
    // The timer fires every rtcpSrIntervalMs_ (default 3 s, well
    // above the RFC 5 s minimum burst but fast enough for prompt MCU
    // convergence). It is started in Initialize() and stopped in the
    // destructor. The thread takes a snapshot of the member map under
    // mutex_ and builds+sends one SR per member.
    // ============================================================
    std::thread              rtcpTimerThread_;
    std::atomic<bool>        rtcpTimerStop_{false};
    static constexpr int     rtcpSrIntervalMs_ = 3000;
};

RTPEngine::RTPEngine()
    : RTPEngine(CreateDefaultBackend()) {
    // Delegating constructor: the default ctor used to construct a
    // hand-rolled RTPEngine; with Q5 it just creates the default
    // backend (NativeRtp) and forwards to the injecting ctor. Every
    // existing call site keeps working unchanged.
}

RTPEngine::RTPEngine(std::unique_ptr<IRtpBackend> backend)
    : impl_(std::make_unique<Impl>()) {
    impl_->statsCollector = std::make_unique<MediaStatisticsCollector>();
    impl_->backend_ = std::move(backend);
    if (impl_->backend_) {
        VCS_MEDIA_LOG_INFO("RTPEngine created with backend: %s",
                          impl_->backend_->GetBackendType().c_str());
        // [Yealink no-video fix] Flip the backend to the "running"
        // state immediately after construction. Without this call,
        // NativeRtpBackend's `running_` flag stays at its default
        // value of `false`, and every AddMember (which is the very
        // next thing the call setup does) fails with
        // MediaTransportNotReady. The far-end then never gets
        // registered as a transport endpoint, no media can be sent,
        // and the call drops with a "video timeout" - exactly the
        // symptom seen on the Yealink endpoint.
        //
        // We call Start() here (rather than waiting for the
        // orchestrator's own Initialize) because:
        //   1. RTPEngine::Initialize(MediaConfig) does not forward
        //      its config to backend_->Initialize(BackendMediaConfig)
        //      -  the two types are different and have to be mapped,
        //      and the mapping is not done yet.
        //   2. The backend has all the information it needs to start
        //      (it was created with default config; any per-call
        //      overrides land in AddMember).
        //   3. Start() is contractually idempotent, so calling it
        //      here is safe even if a future change moves the call to
        //      RTPEngine::Initialize.
        impl_->backend_->Start();
    } else {
        // Defensive: a null backend means the factory could not
        // allocate. This is a programming error (CreateDefaultBackend
        // always returns non-null on allocation success), but we do
        // not want to crash the process on it. The legacy code paths
        // will continue to work; the new backend-forwarding paths
        // (added in steps 5b-5d) will simply no-op.
        VCS_MEDIA_LOG_ERROR("RTPEngine created with NULL backend");
    }
}

std::string RTPEngine::GetBackendType() const {
    if (impl_ && impl_->backend_) return impl_->backend_->GetBackendType();
    return "none";
}

RTPEngine::~RTPEngine() {
    // Stop the RTCP SR timer first — it references impl_->backend_ and
    // impl_->members, both of which are about to be torn down.
    StopRtcpTimer();

    // [Q5 step 5d-receive] Detach from the backend BEFORE the legacy
    // state below is torn down. Otherwise a backend that fires its
    // callback on shutdown would call into a half-destroyed RTPEngine
    // and dereference a dangling impl_.
    ClearBackendReceiveCallback();
}

void RTPEngine::Initialize(const MediaConfig& config) {
    // Backward-compatible single-arg overload. The h264-dump feature
    // is OFF by default; use the two-arg overload to enable it.
    Initialize(config, nullptr);
}

void RTPEngine::Initialize(const MediaConfig& config,
                           const std::shared_ptr<base::IConfig>& debugConfig) {
    VCS_MEDIA_LOG_INFO("Initializing RTPEngine: enableAudio=%d, enableVideo=%d, enableRtcp=%d",
                      (int)config.enableAudio, (int)config.enableVideo, (int)config.enableRtcp);
    impl_->config = config;

    // [V7.0+] Read the h264-dump debug feature from the project config.
    // The key name is `<section>.<PascalCaseProperty>`: the section
    // comes from the [SectionName] header in absphone.ini, the property
    // from the row below it. With the default ini shipped in this repo,
    // the section is "media" and the properties are "H264DumpEnable"
    // / "H264DumpDir", giving the full lookup paths
    //   media.H264DumpEnable  /  media.H264DumpDir  .
    // Other media features (audio dump, rtp dump, ...) just add a new
    // PascalCase property to the same [media] section. A null IConfig
    // is fine and is equivalent to the feature being disabled.
    if (debugConfig) {
        impl_->h264DumpEnabled_ =
            debugConfig->GetBool("media.H264DumpEnable", false);
        impl_->h264DumpDir_ =
            debugConfig->GetString("media.H264DumpDir", "");
    } else {
        impl_->h264DumpEnabled_ = false;
        impl_->h264DumpDir_.clear();
    }
    VCS_MEDIA_LOG_INFO(
        "[h264-dump] feature=%s, dir=%s (configured via %s)",
        impl_->h264DumpEnabled_ ? "ON" : "OFF",
        impl_->h264DumpDir_.empty() ? "<default G:/tmp>" : impl_->h264DumpDir_.c_str(),
        debugConfig ? "IConfig" : "default-off");

    // [Q5 step 5d-receive] Wire the orchestrator-side receive callback
    // into the backend. From this point on, the backend will call back
    // into HandleParsedRtp / HandleParsedRtcp whenever the gateway
    // transport (or the backend's own socket, for a future WebRTC
    // implementation) hands it a fresh datagram.
    InstallBackendReceiveCallback();

    // Start the periodic RTCP Sender Report timer. Without SR the MCU
    // does not recognise this endpoint as an active sender and will not
    // include its uplink audio in the conference mix.
    StartRtcpTimer();
}

bool RTPEngine::IsH264DumpEnabled() const {
    return impl_ && impl_->h264DumpEnabled_;
}

void RTPEngine::AddMember(const std::string& memberId,
                           uint32_t ssrc,
                           const MemberMediaTransports& transports,
                           uint32_t audioSsrc) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    VCS_MEDIA_LOG_INFO("Adding member: memberId=%s, ssrc=%u, streamCount=%zu, total members=%zu",
                      memberId.c_str(), ssrc, transports.size(),
                      impl_->members.size() + 1);

    MemberMediaContext ctx;
    ctx.memberId = memberId;
    ctx.ssrc = ssrc;  // Local SSRC (for sending RTP to peer)
    // [Key Fix - 2026-08-10] Do NOT pre-assign audioSsrc / videoSsrc
    // to the local SSRC. The remote party picks its own audio/video
    // SSRCs independently during SDP negotiation, and we have no way
    // to know them in advance. They are learned from the first
    // incoming packet of each kind (classified by PT in
    // HandleParsedRtp). Pre-assigning both to the same value caused
    // every received packet to be misrouted down the audio path
    // because the routing chain `if (ssrc == ctx.audioSsrc) ... else
    // if (ssrc == ctx.videoSsrc) ...` always matched the audio branch
    // first. Leave audioSsrc / videoSsrc / h239Ssrc at 0 (uninitialized)
    // until learned.
    ctx.transports = transports;
    // Legacy alias: pick the audio transport if present, else any kind.
    auto audioIt = transports.find(MediaStreamKind::Audio);
    if (audioIt != transports.end()) {
        ctx.transport = audioIt->second;
    } else if (!transports.empty()) {
        ctx.transport = transports.begin()->second;
    }
    ctx.isConnected = true;

    impl_->members[memberId] = ctx;
    impl_->statsCollector->AddMember(memberId);

    // Resolve the hot-path counter handles now that the member exists in the
    // statistics store: the per-packet send path must not take the store's
    // control-plane mutex.
    if (impl_->statsCollector) {
        auto ctxIt = impl_->members.find(memberId);
        if (ctxIt != impl_->members.end()) {
            ctxIt->second.audioStats = impl_->statsCollector->GetStreamCounter(
                memberId, ::vcs::MediaStreamKind::Audio);
            ctxIt->second.videoStats = impl_->statsCollector->GetStreamCounter(
                memberId, ::vcs::MediaStreamKind::Video);
            ctxIt->second.secondaryVideoStats = impl_->statsCollector->GetStreamCounter(
                memberId, ::vcs::MediaStreamKind::SecondaryVideo);
        }
    }

    VCS_MEDIA_LOG_DEBUG("Member added: memberId=%s, audioSsrc=%u, videoSsrc=%u, kinds=%zu",
                       memberId.c_str(), ctx.audioSsrc, ctx.videoSsrc, transports.size());

    // [Q5 step 5a] Double-write: also keep the injected backend in sync.
    // [V7.0] Hand the full per-kind transport map to the backend so it
    // can route outgoing RTP packets by stream kind.
    if (impl_->backend_) {
        BackendMemberConfig backend_cfg;
        backend_cfg.memberId        = memberId;
        backend_cfg.ssrcAudio       = ctx.audioSsrc;
        backend_cfg.ssrcVideo       = ctx.videoSsrc;
        backend_cfg.payloadTypeAudio = ctx.sendAudioPayloadType;
        backend_cfg.payloadTypeVideo = ctx.sendVideoPayloadType;
        vcs::ErrorCode rc = impl_->backend_->AddMember(memberId, backend_cfg);
        if (rc != vcs::ErrorCode::Success) {
            VCS_MEDIA_LOG_WARNING(
                "AddMember: backend_->AddMember failed for memberId=%s, err=%s",
                memberId.c_str(), ToString(rc));
        }
        // Hand the full transport map to the backend. The backend is
        // responsible for per-kind routing on the egress side.
        impl_->backend_->SetTransports(memberId, transports);

        // [WebRTC media engine] Hand over the local (outbound) SSRCs so the
        // engine can build its AudioSendStream / VideoSendStream.
        impl_->backend_->SetMemberLocalSsrc(memberId, audioSsrc, ssrc);
        impl_->backend_->UpdateMemberPayloadTypes(memberId,
            ctx.sendAudioPayloadType, ctx.sendVideoPayloadType);
    }
}

void RTPEngine::UpdateMemberSsrc(const std::string& memberId,
                                   uint32_t audioSsrc,
                                   uint32_t videoSsrc,
                                   uint32_t h239Ssrc) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    VCS_MEDIA_LOG_INFO("Updating member SSRC: memberId=%s, audioSsrc=%u, videoSsrc=%u, h239Ssrc=%u",
                      memberId.c_str(), audioSsrc, videoSsrc, h239Ssrc);

    auto it = impl_->members.find(memberId);
    if (it == impl_->members.end()) {
        VCS_MEDIA_LOG_DEBUG("Member not found: memberId=%s", memberId.c_str());
        return;
    }

    // Update the orchestrator-side MemberMediaContext. The business
    // state (audioSsrc / videoSsrc / h239Ssrc) is the orchestrator's
    // responsibility  -  the backend only knows the opaque SSRC -> member
    // routing index (it has no concept of "audio SSRC vs video SSRC").
    it->second.audioSsrc = audioSsrc;
    it->second.videoSsrc = videoSsrc;
    it->second.h239Ssrc  = h239Ssrc;

    // [Q5 step 5g] The legacy impl_->ssrcToMember double-write was
    // removed  -  the backend now owns the SSRC routing index, and
    // FindMemberBySsrc consults it as the single source of truth.
    // We MUST call backend_->UpdateMemberSsrc here so the backend
    // knows to remove the OLD audio/video SSRCs from its ssrcToMember_
    // index and register the NEW ones. Without this, the backend would
    // still answer FindMemberBySsrc with the original AddMember SSRC,
    // which would mismatch the orchestrator's view and break routing
    // after SDP renegotiation.
    if (impl_->backend_) {
        impl_->backend_->UpdateMemberSsrc(memberId, audioSsrc, videoSsrc, h239Ssrc);
    }
}

void RTPEngine::UpdateMemberSendPayloadType(const std::string& memberId,
                                             uint8_t audioPayloadType,
                                             uint8_t videoPayloadType) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (audioPayloadType > 127 || videoPayloadType > 127) {
        VCS_MEDIA_LOG_ERROR("Invalid payload type: audio=%u, video=%u", audioPayloadType, videoPayloadType);
        return;
    }

    VCS_MEDIA_LOG_INFO("UpdateMemberSendPayloadType: memberId=%s, audioPT=%u, videoPT=%u",
                      memberId.c_str(), audioPayloadType, videoPayloadType);

    auto it = impl_->members.find(memberId);
    if (it == impl_->members.end()) {
        VCS_MEDIA_LOG_DEBUG("Member not found: memberId=%s", memberId.c_str());
        return;
    }

    it->second.sendAudioPayloadType = audioPayloadType;
    it->second.sendVideoPayloadType = videoPayloadType;

    if (impl_->backend_) {
        impl_->backend_->UpdateMemberPayloadTypes(memberId,
            audioPayloadType, videoPayloadType);
    }

    VCS_MEDIA_LOG_INFO("Member send PT updated: audio=%u, video=%u",
                      audioPayloadType, videoPayloadType);
}

void RTPEngine::UpdateMemberAudioSendPayloadType(const std::string& memberId,
                                                 uint8_t audioPayloadType) {
    if (audioPayloadType > 127) {
        VCS_MEDIA_LOG_ERROR(
            "UpdateMemberAudioSendPayloadType: invalid audio PT %u",
            (unsigned)audioPayloadType);
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->members.find(memberId);
    if (it == impl_->members.end()) {
        VCS_MEDIA_LOG_WARNING(
            "UpdateMemberAudioSendPayloadType: member not found: %s",
            memberId.c_str());
        return;
    }

    const uint8_t oldPt = it->second.sendAudioPayloadType;
    it->second.sendAudioPayloadType = audioPayloadType;
    it->second.negotiatedAudioPt = static_cast<int16_t>(audioPayloadType);
    if (impl_->backend_) {
        impl_->backend_->UpdateMemberPayloadTypes(memberId, audioPayloadType,
            it->second.sendVideoPayloadType);
    }
    VCS_MEDIA_LOG_INFO("[AudioPT] member=%s, send PT %u -> %u",
                       memberId.c_str(), (unsigned)oldPt,
                       (unsigned)audioPayloadType);

    if (it->second.negotiatedVideoPt >= 0 &&
        it->second.negotiatedVideoPt == it->second.negotiatedAudioPt) {
        VCS_MEDIA_LOG_WARNING(
            "[AudioPT] member=%s: audio and video negotiated the same PT %u; "
            "inbound packets cannot be split by PT",
            memberId.c_str(), (unsigned)audioPayloadType);
    }
}

void RTPEngine::UpdateMemberVideoSendPayloadType(const std::string& memberId,
                                                 uint8_t videoPayloadType) {
    if (videoPayloadType > 127) {
        VCS_MEDIA_LOG_ERROR(
            "UpdateMemberVideoSendPayloadType: invalid video PT %u",
            (unsigned)videoPayloadType);
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->members.find(memberId);
    if (it == impl_->members.end()) {
        VCS_MEDIA_LOG_WARNING(
            "UpdateMemberVideoSendPayloadType: member not found: %s",
            memberId.c_str());
        return;
    }

    const uint8_t oldPt = it->second.sendVideoPayloadType;
    it->second.sendVideoPayloadType = videoPayloadType;
    it->second.negotiatedVideoPt = static_cast<int16_t>(videoPayloadType);
    if (impl_->backend_) {
        impl_->backend_->UpdateMemberPayloadTypes(memberId,
            it->second.sendAudioPayloadType, videoPayloadType);
    }
    VCS_MEDIA_LOG_INFO("[VideoPT] member=%s, send PT %u -> %u",
                       memberId.c_str(), (unsigned)oldPt,
                       (unsigned)videoPayloadType);

    if (it->second.negotiatedAudioPt >= 0 &&
        it->second.negotiatedAudioPt == it->second.negotiatedVideoPt) {
        VCS_MEDIA_LOG_WARNING(
            "[VideoPT] member=%s: audio and video negotiated the same PT %u; "
            "inbound packets cannot be split by PT",
            memberId.c_str(), (unsigned)videoPayloadType);
    }
}

void RTPEngine::RemoveMember(const std::string& memberId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->members.find(memberId);
    if (it != impl_->members.end()) {
        VCS_MEDIA_LOG_DEBUG("Removing SSRC mappings: audio=%u, video=%u, h239=%u",
                           it->second.audioSsrc, it->second.videoSsrc, it->second.h239Ssrc);
    }

    impl_->members.erase(memberId);

    // Log AFTER erase so a missing member cannot underflow size()-1.
    VCS_MEDIA_LOG_INFO("Removing member: memberId=%s, remaining members=%zu",
                      memberId.c_str(), impl_->members.size());

    impl_->statsCollector->RemoveMember(memberId);

    // [Q5 step 5f-receive] Clear this member's FU-A depacketizer buffer
    // (now lives on the backend).
    if (impl_->backend_) {
        impl_->backend_->ClearH264Depacketizer(memberId);
    }

    // [Q5 step 5b] Double-write: tell the injected backend to tear down
    // the transport channel for this member. The backend is contractually
    // obligated to make RemoveMember() safe to call on an unknown
    // memberId (it is a no-op in that case), so we do not need to mirror
    // the `members.find` guard above. We DO still call it unconditionally
    // so the backend's internal map can be kept consistent even if the
    // legacy map had already been cleared via ClearAllMembers().
    if (impl_->backend_) {
        impl_->backend_->RemoveMember(memberId);
    }
}

void RTPEngine::ClearAllMembers() {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    VCS_MEDIA_LOG_INFO("Clearing all members: count=%zu", impl_->members.size());

    // [Q5 step 5b] Snapshot the member ids BEFORE we clear the legacy
    // maps. The backend has no ClearAllMembers() (it is a per-protocol
    // stateless abstraction; one backend may map many conferences and
    // must not lose unrelated members), so we have to remove them one
    // by one. Doing the snapshot under the same lock that erases the
    // maps is safe: a second ClearAllMembers() or RemoveMember() cannot
    // sneak in because we hold impl_->mutex for the whole operation.
    std::vector<std::string> memberIds;
    memberIds.reserve(impl_->members.size());
    for (const auto& kv : impl_->members) {
        memberIds.push_back(kv.first);
    }

    impl_->members.clear();
    impl_->statsCollector->ClearAll();

    // [Q5 step 5f-receive] Clear all FU-A depacketizer buffers
    // (now lives on the backend).
    if (impl_->backend_) {
        impl_->backend_->ClearAllH264Depacketizers();
    }

    // [Q5 step 5b] Double-write: tear down the backend's transport
    // channels for every member we just removed. We do this AFTER the
    // legacy state is cleared so a backend that synchronously calls
    // back into RTPEngine (e.g. through a destroy notification) sees
    // the post-clear state and behaves correctly.
    if (impl_->backend_) {
        for (const auto& mid : memberIds) {
            impl_->backend_->RemoveMember(mid);
        }
    }
}

void RTPEngine::SetAudioOutputCallback(RtpPacketCallback callback) {
    VCS_MEDIA_LOG_DEBUG("Audio output callback %s", callback ? "set" : "removed");
    impl_->audioCallback = callback;
}

void RTPEngine::SetVideoOutputCallback(RtpPacketCallback callback) {
    VCS_MEDIA_LOG_DEBUG("Video output callback %s", callback ? "set" : "removed");
    impl_->videoCallback = callback;
}

void RTPEngine::SetDualOutputCallback(RtpPacketCallback callback) {
    VCS_MEDIA_LOG_DEBUG("Dual output callback %s", callback ? "set" : "removed");
    impl_->dualCallback = callback;
}

void RTPEngine::SetRtcpCallback(RtcpCallback callback) {
    VCS_MEDIA_LOG_DEBUG("RTCP callback %s", callback ? "set" : "removed");
    impl_->rtcpCallback = callback;
}

void RTPEngine::SetRemoteKeyFrameRequestCallback(RemoteKeyFrameRequestCallback callback) {
    VCS_MEDIA_LOG_DEBUG("Remote keyframe request callback %s", callback ? "set" : "removed");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->keyFrameRequestCallback = std::move(callback);
}

void RTPEngine::HandleInboundRtcp(const std::string& memberId,
                                   vcs::MediaStreamKind kind,
                                   const uint8_t* data, size_t len) {
    if (!data || len < 8) {
        return;
    }

    // Compound RTCP: walk the length fields (RFC 3550 §6.1). We only need
    // PSFB (PT 206) FMT 1 = PLI and FMT 4 = FIR; everything else is left to
    // the raw RtcpCallback hook.
    bool keyFrameRequested = false;
    size_t offset = 0;
    while (offset + 4 <= len) {
        const uint8_t* p = data + offset;
        if ((p[0] >> 6) != 2) {  // version must be 2
            break;
        }
        const uint8_t pt = p[1];
        const uint8_t fmt = p[0] & 0x1F;
        const size_t words = static_cast<size_t>((p[2] << 8) | p[3]) + 1;
        const size_t packetLen = words * 4;
        if (packetLen < 4 || offset + packetLen > len) {
            break;
        }

        if (pt == 206 && (fmt == 1 || fmt == 4)) {
            keyFrameRequested = true;
            VCS_MEDIA_LOG_INFO("[RTCP] %s from member=%s, kind=%s",
                               fmt == 1 ? "PLI" : "FIR", memberId.c_str(),
                               vcs::MediaStreamKindToString(kind));
        }
        offset += packetLen;
    }

    // [WebRTC media engine] RTCP must reach the Call so its RTCP modules can
    // generate NACKs/PLIs and process reports. The legacy PLI/FIR parsing
    // below stays active because it drives the local encoder's IDR cadence.
    if (impl_->backend_ && impl_->backend_->UsesWebRtcMediaEngine()) {
        impl_->backend_->OnRtcpReceived(memberId, data, len);
    }

    RemoteKeyFrameRequestCallback kfCallback;
    RtcpCallback rawCallback;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (keyFrameRequested) {
            kfCallback = impl_->keyFrameRequestCallback;
        }
        rawCallback = impl_->rtcpCallback;
    }

    if (keyFrameRequested && kfCallback) {
        std::atomic<int64_t>& last =
            (kind == vcs::MediaStreamKind::SecondaryVideo)
                ? impl_->lastSecondaryKeyFrameRequestMs
                : impl_->lastMainKeyFrameRequestMs;
        const int64_t nowMs = static_cast<int64_t>(vcs::base::platform::UtcNowEpochMs());
        const int64_t lastMs = last.load(std::memory_order_relaxed);
        // 500 ms: fast enough for real loss, slow enough that a peer spamming
        // PLI cannot force an IDR for every frame it misses.
        if (lastMs == 0 || nowMs - lastMs >= 500) {
            last.store(nowMs, std::memory_order_relaxed);
            kfCallback(memberId, kind);
        }
    }

    if (rawCallback) {
        rawCallback(memberId, data, len);
    }
}

void RTPEngine::SetMediaSendCallback(MediaSendCallback callback) {
    VCS_MEDIA_LOG_INFO("MediaSendCallback %s (injection mode)", callback ? "set" : "removed");
    impl_->mediaSendCallback = callback;
}

bool RTPEngine::IsUsingInjectionMode() const {
    return impl_->mediaSendCallback != nullptr;
}

void RTPEngine::SetAudioPlayoutPcmCallback(
    std::function<void(const std::string&, const int16_t*, size_t, int, int)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->audioPlayoutPcmCallback = std::move(callback);
}

bool RTPEngine::PushCapturedAudio(const std::string& memberId,
    const int16_t* pcm, size_t samplesPerChannel,
    int sampleRate, int channels) {
    if (!impl_->backend_ || !pcm || samplesPerChannel == 0) return false;
    if (!impl_->backend_->UsesWebRtcMediaEngine()) return false;
    return impl_->backend_->PushCapturedAudio(memberId, pcm,
        samplesPerChannel, sampleRate, channels);
}

void RTPEngine::OnRtpReceived(const std::string& memberId,
                               const RtpPacket& packet) {
    // [Q5 step 5d-receive] Thin forwarder. The actual RTP header parse,
    // SSRC lookup, and dispatch all live on the other side of the
    // backend boundary. RTPEngine no longer cares about the bit layout
    // of the RTP header  -  that is the backend's job (see
    // NativeRtp::Ingress::ParseRtpHeader in NativeRtpBackend.cpp).
    //
    // Null backend = programming error (RTPEngine always owns a backend
    //  -  at worst the default NativeRtp). We log + drop rather than
    // crash because the gateway is the caller and we don't want a bad
    // backend factory to take down the whole media thread.
    if (!impl_->backend_) {
        VCS_MEDIA_LOG_ERROR(">>> RTPENGINE: OnRtpReceived but backend is NULL, memberId=%s, len=%zu",
                            memberId.c_str(), packet.length);
        return;
    }
    if (!packet.data || packet.length == 0) return;
    impl_->backend_->OnRtpReceived(memberId, packet.data, packet.length);
}

void RTPEngine::OnRtpReceivedParsed(const std::string& memberId,
	uint8_t payloadType,
	const uint8_t* data, size_t len,
	uint32_t ssrc, uint64_t timestamp,
	uint16_t sequenceNumber, bool markerBit) {
	if (!impl_ || !impl_->backend_) {
		VCS_MEDIA_LOG_ERROR(">>> RTPENGINE: OnRtpReceivedParsed but backend is NULL");
		return;
	}
	if (!data || len == 0) return;
	impl_->backend_->OnRtpReceivedParsed(memberId, payloadType, data, len,
		ssrc, timestamp, sequenceNumber, markerBit);
}

void RTPEngine::OnRtcpReceived(const std::string& memberId,
                                const RtcpPacket& packet) {
    // [Q5 step 5d-receive] Same shape as OnRtpReceived: forward the raw
    // bytes to the backend, which fires the orchestrator's
    // RtcpReceiveCallback (installed in InstallBackendReceiveCallback).
    if (!impl_->backend_) {
        VCS_MEDIA_LOG_TRACE("RTCP received (no backend): memberId=%s, len=%zu",
                            memberId.c_str(), packet.length);
        return;
    }
    if (!packet.data || packet.length == 0) return;
    impl_->backend_->OnRtcpReceived(memberId, packet.data, packet.length);
}

void RTPEngine::HandleParsedRtcp(const std::string& memberId,
                                  const uint8_t* data, size_t len) {
    // [Q5 step 5d-receive] The orchestrator's RTCP receive callback.
    // The backend has already demuxed the UDP datagram by the time it
    // gets here, so we just forward the raw bytes to the registered
    // RTCP callback (e.g. RTCP statistics / RTCP XR parsing in
    // MediaControl).
    //
    // Copy the callback under the lock and invoke it after unlocking: a
    // handler registered by MediaControl may take processor locks, and the
    // send path holds member locks while calling into processors.
    RtcpCallback cb;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        cb = impl_->rtcpCallback;
    }
    VCS_MEDIA_LOG_TRACE("RTCP received: memberId=%s, len=%zu", memberId.c_str(), len);
    if (cb) {
        cb(memberId, data, len);
    }
}

void RTPEngine::InstallBackendReceiveCallback() {
	if (!impl_->backend_) return;
	auto rtp_cb = [this](const std::string& memberId,
		uint8_t pt, const uint8_t* data, size_t len,
		uint32_t ssrc, uint64_t timestamp, uint16_t seq,
		bool markerBit) {
		this->HandleParsedRtp(memberId, pt, data, len, ssrc, timestamp, seq, markerBit);
	};
	impl_->backend_->SetRtpReceiveCallback(std::move(rtp_cb));
	auto rtcp_cb = [this](const std::string& memberId,
		const uint8_t* data, size_t len) {
		this->HandleParsedRtcp(memberId, data, len);
	};
	impl_->backend_->SetRtcpReceiveCallback(std::move(rtcp_cb));

	// [WebRTC media engine] Complete video frames leave the engine's jitter
	// buffer / decoder pass-through; push them into the same queue the legacy
	// depacketizer feeds, so VideoProcessor is unaware of the change.
	auto encoded_cb = [this](const std::string& memberId,
		const uint8_t* data, size_t len,
		bool isKeyFrame, uint32_t timestamp, uint32_t ssrc,
		bool isSecondary) {
		if (!data || len == 0) return;
		EncodedVideoFrame frame;
		frame.data = const_cast<uint8_t*>(data);
		frame.size = len;
		frame.timestamp = timestamp;
		frame.ssrc = ssrc;
		frame.isKeyFrame = isKeyFrame;
		frame.isH239 = isSecondary;
		frame.codecType = EncodedVideoFrame::CodecType::H264;
		this->PushEncodedVideoFrame(memberId, frame);
	};
	impl_->backend_->SetEncodedVideoFrameCallback(std::move(encoded_cb));

	// [WebRTC media engine] NetEq playout PCM -> AudioProcessor::RenderPcm.
	auto playout_cb = [this](const std::string& memberId,
		const int16_t* pcm, size_t samplesPerChannel,
		int sampleRate, int channels) {
		std::function<void(const std::string&, const int16_t*, size_t, int, int)> cb;
		{
			std::lock_guard<std::mutex> lock(impl_->mutex);
			cb = impl_->audioPlayoutPcmCallback;
		}
		if (cb) {
			cb(memberId, pcm, samplesPerChannel, sampleRate, channels);
		}
	};
	impl_->backend_->SetAudioPlayoutCallback(std::move(playout_cb));
}

void RTPEngine::ClearBackendReceiveCallback() {
    // [Q5 step 5d-receive] Symmetric to InstallBackendReceiveCallback.
    // Called from the destructor so that any pending callback fires
    // after the RTPEngine half is gone are silently dropped.
    if (!impl_->backend_) return;
    impl_->backend_->SetRtpReceiveCallback(nullptr);
    impl_->backend_->SetRtcpReceiveCallback(nullptr);
    impl_->backend_->SetEncodedVideoFrameCallback(nullptr);
    impl_->backend_->SetAudioPlayoutCallback(nullptr);
}

void RTPEngine::BeginReceive() {
    // [V7.0+] "Open the gate" �� install the backend receive
    // callbacks so the backend starts feeding packets into
    // HandleParsedRtp / HandleParsedRtcp. Idempotent: if the
    // callbacks are already installed (e.g. because the 2-arg
    // Initialize() installed them eagerly), re-installing just
    // overwrites the existing lambdas with functionally
    // identical ones.
    InstallBackendReceiveCallback();
    if (impl_) impl_->isReceiving_ = true;
    VCS_MEDIA_LOG_INFO("RTPEngine: BeginReceive (gate open)");
}

void RTPEngine::StopReceive() {
    // [V7.0+] "Close the gate" �� clear the backend receive
    // callbacks. Idempotent: safe to call when the gate is
    // already closed, when no backend is attached, or from the
    // destructor. Mirrors the design contract documented in
    // RTPEngine.h (Initialize does NOT auto-install; caller
    // must BeginReceive to start, StopReceive to halt).
    ClearBackendReceiveCallback();
    if (impl_) impl_->isReceiving_ = false;
    VCS_MEDIA_LOG_INFO("RTPEngine: StopReceive (gate closed)");
}

bool RTPEngine::IsReceiving() const {
    return impl_ && impl_->isReceiving_;
}

void RTPEngine::HandleParsedRtp(const std::string& memberId,
	uint8_t pt, const uint8_t* payload, size_t payloadSize,
	uint32_t ssrc, uint64_t timestamp,
	uint16_t seq, bool markerBit) {
    // [h264-dump feature] Throttled entry trace. Print first 5 + every
    // 30th call so we can confirm whether HandleParsedRtp is being
    // invoked (and with what PT/SSRC) without spamming the log at 60fps.
    // Only runs when the dump feature is enabled; otherwise this is a
    // single bool load and a no-op.
    if (impl_->h264DumpEnabled_) {
        static thread_local int s_entryLogCount = 0;
        if (s_entryLogCount < 5 || (s_entryLogCount % 30) == 0) {
            VCS_MEDIA_LOG_INFO(
                "[h264-dump] HandleParsedRtp ENTER: memberId=%s, pt=%u, "
                "ssrc=0x%08X, payloadSize=%zu, seq=%u",
                memberId.c_str(), pt, ssrc, payloadSize, seq);
        }
        ++s_entryLogCount;
    }
    // [Q5 step 5d-receive] This is the legacy body of OnRtpReceived
    // AFTER the RTP-header parse. The backend has already extracted
    // (pt, ssrc, timestamp, seq, payload, payloadLen) and handed them
    // to us via the RtpReceiveCallback installed in
    // InstallBackendReceiveCallback(). Our job here is:
    //
    //   1. Find the member context (SSRC routing).
    //   2. If the SSRC is known and matches audio / video / dual,
    //      dispatch to the corresponding callback (which in turn feeds
    //      AudioProcessor / VideoProcessor / DualProcessor).
    //   3. If the SSRC is unknown but PT-based classification can
    //      tell audio from video, auto-learn the SSRC mapping and
    //      then dispatch.
    //
    // [Header reconstruction] The audio and dual callbacks expect a
    // full RTP datagram (header + payload), not just the payload. The
    // backend has already parsed off the header, so we re-emit a
    // 12-byte header using the same encoding rules as
    // NativeRtp::Egress::BuildRtpDatagram. For our V=2 P=0 X=0 CC=0
    // wire the two paths produce byte-for-byte identical output.
    // The video callback takes the payload only (it does the FU-A
    // reassembly and writes the reassembled NALU into the queue), so
    // it does not need a reconstructed header.
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // Find member
    auto it = impl_->members.find(memberId);
    if (it == impl_->members.end()) {
        VCS_MEDIA_LOG_ERROR(">>> RTPENGINE: Unknown member: memberId=%s", memberId.c_str());
        return;
    }

    const auto& ctx = it->second;

    // [Key Fix - 2026-08-10] PT-based routing.
    //
    // Why PT and not SSRC:
    //   The remote party picks its own audio/video SSRCs at SDP
    //   negotiation time, and we have no way to know them in advance.
    //   The first incoming packet of each kind carries BOTH the SSRC
    //   and the PT  -  the PT is the only thing we DO know a priori
    //   (it is part of the SDP). So we classify by PT and learn the
    //   SSRC as a side effect, then cache the learned SSRC for
    //   subsequent packets.
    //
    // Why this fixes the "no video" bug:
    //   Previously, AddMember pre-assigned audioSsrc == videoSsrc == ssrc
    //   and HandleParsedRtp had `if (ssrc == ctx.audioSsrc || ssrc == ctx.ssrc)`
    //   as the audio branch. With audioSsrc == ssrc, EVERY incoming
    //   packet matched the audio branch and was never routed to the
    //   video processor. The auto-learn path (which used PT correctly)
    //   was unreachable.
    //
    // [SSRC update side effect] When the remote re-INVITEs and changes
    // its SSRC, the PT stays the same but the SSRC does not match what
    // we cached. We detect this and update the cache + propagate to
    // the backend's ssrcToMember_ index.
    const Impl::PayloadKind ptKind = impl_->ClassifyPayload(ctx, pt);
    if (ptKind == Impl::PayloadKind::Video) {
        if (ctx.videoSsrc != ssrc) {
            if (ctx.videoSsrc == 0) {
                VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Learning VIDEO SSRC: memberId=%s, ssrc=0x%08x, pt=%u",
                                  memberId.c_str(), ssrc, pt);
            } else {
                VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Video SSRC changed: memberId=%s, old=0x%08x, new=0x%08x",
                                  memberId.c_str(), ctx.videoSsrc, ssrc);
            }
            it->second.videoSsrc = ssrc;
            if (impl_->backend_) {
                impl_->backend_->UpdateMemberSsrc(
                    memberId, it->second.audioSsrc, ssrc, it->second.h239Ssrc);
            }
        }
    } else if (ptKind == Impl::PayloadKind::Audio) {
        if (ctx.audioSsrc != ssrc) {
            if (ctx.audioSsrc == 0) {
                VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Learning AUDIO SSRC: memberId=%s, ssrc=0x%08x, pt=%u",
                                  memberId.c_str(), ssrc, pt);
            } else {
                VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Audio SSRC changed: memberId=%s, old=0x%08x, new=0x%08x",
                                  memberId.c_str(), ctx.audioSsrc, ssrc);
            }
            it->second.audioSsrc = ssrc;
            if (impl_->backend_) {
                impl_->backend_->UpdateMemberSsrc(
                    memberId, ssrc, it->second.videoSsrc, it->second.h239Ssrc);
            }
        }
    } else {
        /*VCS_MEDIA_LOG_WARNING(">>> RTPENGINE: Unknown PT, dropping: memberId=%s, pt=%u, ssrc=0x%08x",
                             memberId.c_str(), pt, ssrc);*/
        return;
    }

    // [Note] `ctx` is a const reference to it->second, so the
    // audioSsrc/videoSsrc reads below reflect the values we just
    // wrote above. No need to re-fetch.

    // Route based on (now-updated) SSRC. The first packet of each
    // kind will have just learned its SSRC and so will match here.
    if (ssrc == ctx.audioSsrc) {
        // Audio packet  -  callback expects full RTP datagram.
        //VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Routing AUDIO: memberId=%s, pt=%u", memberId.c_str(), pt);
        // Receive bytes/packets are counted in
        // MediaTransportCallback::OnRtpReceived, not here:
        //   - this path only knows a payload type, so it cannot tell main
        //     video from secondary video;
        //   - FU-A fragments return before reassembly and never reach the
        //     old call site, so it counted frames, not packets;
        //   - with MediaWorker in front, counting here too would double
        //     every complete frame.
        // The transport callback sees every datagram and carries the
        // stream kind, so it is the only correct producer.
        if (impl_->audioCallback) {
            // Reconstruct the 12-byte header so AudioProcessor receives
            // a complete datagram. We allocate a vector per call; if
            // this becomes a hotspot, a thread-local scratch buffer
            // would be the next optimization.
            std::vector<uint8_t> datagram;
            datagram.resize(12 + payloadSize);
            datagram[0] = 0x80;
            datagram[1] = static_cast<uint8_t>(pt & 0x7F);
            datagram[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
            datagram[3] = static_cast<uint8_t>(seq & 0xFF);
            datagram[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
            datagram[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
            datagram[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
            datagram[7] = static_cast<uint8_t>(timestamp & 0xFF);
            datagram[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
            datagram[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
            datagram[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
            datagram[11] = static_cast<uint8_t>(ssrc & 0xFF);
            if (payloadSize > 0 && payload) {
                std::memcpy(datagram.data() + 12, payload, payloadSize);
            }
            impl_->audioCallback(memberId, pt,
                                 datagram.data(), datagram.size(),
                                 ssrc, timestamp, seq);
        } else {
            VCS_MEDIA_LOG_ERROR(">>> RTPENGINE: audioCallback is NULL!");
        }
    } else if (ssrc == ctx.videoSsrc) {
        // [WebRTC media engine] libwebrtc owns the video receive path
        // (jitter buffer, NACK/ARQ, PLI) and delivers complete frames through
        // the encoded-frame sink. SSRC learning above already happened, so
        // just stop here instead of depacketizing a second time.
        if (impl_->backend_ && impl_->backend_->UsesWebRtcMediaEngine()) {
            return;
        }

        // ============================================================
        // [Design Principle] H.264 FU-A depacketization is completed by RTPEngine
        // RTP packet may be FU-A fragments, need to first reassemble into complete NALU then push to queue
        // ============================================================

        // [Q5 step 5g] The legacy "parse and print FU header" diagnostic
        // block was removed. The variables it produced (fuIndicator,
        // fuHeader, nalType, isStart, isEnd) were only consumed by a
        // commented-out VCS_MEDIA_LOG_TRACE. The backend (NativeRtp::
        // Ingress::H264::Process) is now the sole owner of FU-A header
        // parsing, and it returns `isKeyFrame` directly through the
        // H264DepacketizeResult. The actual production NAL-type log
        // below still runs because it is useful and not redundant with
        // anything the backend logs.

        // [Q5 step 5f-receive] H.264 FU-A depacketization is now a
        // backend responsibility. RTPEngine asks the backend to
        // reassemble the fragment stream into a complete NALU, then
        // pushes the result to the video frame queue.
        auto depackResult = impl_->backend_->DepacketizeH264(
            memberId, payload, payloadSize, seq, timestamp, markerBit);
        bool isKeyFrame = depackResult.isKeyFrame;
        std::vector<uint8_t> frameData = std::move(depackResult.nalu);

 
        //  Print hex of first few bytes of RTP payload
        // Help analyze if there are FU-A or single NAL units
        if (payloadSize >= 2) {
            uint8_t firstByte = payload[0];
            uint8_t nalType = firstByte & 0x1F;
            bool isFUA = (nalType == 28);
            uint8_t fuaNalType = isFUA ? (payload[1] & 0x1F) : nalType;

            // [Key Diagnosis] Print all NAL types arriving at RTP layer
            /*VCS_MEDIA_LOG_INFO(">>> RTPENGINE: RTP payload analysis: memberId=%s, seq=%u, "
                             "nalType=%u (%s), isFUA=%d, fuaInnerType=%u, size=%zu, isKeyFrame=%s",
                             memberId.c_str(), seq, nalType,
                             (nalType==5?"IDR":nalType==7?"SPS":nalType==8?"PPS":
                              nalType==1?"Non-IDR":nalType==28?"FU-A":"Other"),
                             isFUA, fuaNalType, payloadSize, isKeyFrame?"YES":"NO");*/
        }

        if (frameData.empty()) {
            // FU-A fragment incomplete, wait for subsequent fragments
            // VCS_MEDIA_LOG_TRACE(">>> RTPENGINE: FU-A waiting: memberId=%s, seq=%u, S=%d, E=%d",
            //                   memberId.c_str(), seq, isStart, isEnd);
            return;  // Do not push to queue, wait for reassembly to complete
        }

        // Reassembly complete, build EncodedVideoFrame
        // [Key Fix] frameData is local vector, PushEncodedVideoFrame will copy data
        EncodedVideoFrame frame;
        frame.data = frameData.data();  // Temporary pointer, will copy on Push
        frame.size = frameData.size();
        frame.timestamp = timestamp;
        frame.sequenceNumber = seq;
        frame.ssrc = ssrc;
        frame.isKeyFrame = isKeyFrame;
        frame.isH239 = false;
        frame.codecType = EncodedVideoFrame::CodecType::H264;

        VCS_MEDIA_LOG_TRACE(">>> RTPENGINE: H.264 frame ready: memberId=%s, size=%zu, keyFrame=%s",
                          memberId.c_str(), frameData.size(), isKeyFrame ? "YES" : "NO");

        // [Q5 step 5g] The legacy dev-only H.264 file dump
        // (h264_fp / h264FileMutex / fopen to a hard-coded path under
        // /home/vcs/...) was removed. See Q5-Step5g-migration-comparison.md.
        // The backend (NativeRtpBackend) and MediaStatisticsCollector
        // are the canonical owners of the receive path now.

        // Receive counters: MediaTransportCallback::OnRtpReceived (see the
        // note in the audio branch above).

        // Push to queue (consumed by VideoProcessor)
        // [Key Fix] Keep frameData valid before copying data
        if (!impl_->videoQueuePool) {
            VCS_MEDIA_LOG_ERROR(">>> RTPENGINE: videoQueuePool is NULL, cannot push video frame!");
        } else {
            PushEncodedVideoFrame(memberId, frame);
        }
    } else if (ssrc == ctx.h239Ssrc) {
        // H.239 dual-stream packet  -  callback expects full RTP datagram
        VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Routing DUAL: memberId=%s", memberId.c_str());
        if (impl_->dualCallback) {
            // Same 12-byte header reconstruction as the audio path.
            std::vector<uint8_t> datagram;
            datagram.resize(12 + payloadSize);
            datagram[0] = 0x80;
            datagram[1] = static_cast<uint8_t>(pt & 0x7F);
            datagram[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
            datagram[3] = static_cast<uint8_t>(seq & 0xFF);
            datagram[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
            datagram[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
            datagram[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
            datagram[7] = static_cast<uint8_t>(timestamp & 0xFF);
            datagram[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
            datagram[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
            datagram[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
            datagram[11] = static_cast<uint8_t>(ssrc & 0xFF);
            if (payloadSize > 0 && payload) {
                std::memcpy(datagram.data() + 12, payload, payloadSize);
            }
            impl_->dualCallback(memberId, pt,
                                datagram.data(), datagram.size(),
                                ssrc, timestamp, seq);
        }
    } else {
        // [Key Fix] Auto-learn remote SSRC
        // When receiving from known member but SSRC is unknown, learn based on PT type
        VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Auto-learning SSRC: memberId=%s, ssrc=0x%08x, pt=%u",
                          memberId.c_str(), ssrc, pt);

        // Update member context - add new SSRC mapping
        it->second.remoteSsrc = ssrc;

        // [Q5 step 5g] The legacy impl_->ssrcToMember[ssrc] = memberId
        // double-write was removed. Auto-learning the orchestrator's
        // MemberMediaContext is enough  -  the backend's ssrcToMember_ is
        // updated by the subsequent backend_->UpdateMemberSsrc call
        // below. The "keep old mapping just in case" line was redundant
        // because AddMember already registered the old video SSRC under
        // the same memberId, and overwriting it with the same value is
        // a no-op.
        //
        // Determine audio or video based on PT, and learn corresponding SSRC
        //  Negotiated PT first (per member), then the configured lists.
        if (impl_->ClassifyPayload(it->second, pt) == Impl::PayloadKind::Video) {
            // ============================================================
            // [Design Principle] H.264 FU-A depacketization is completed by RTPEngine
            // ============================================================
            VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Learning VIDEO SSRC: memberId=%s, oldVideoSsrc=0x%08x, newSsrc=0x%08x",
                              memberId.c_str(), it->second.videoSsrc, ssrc);
            it->second.videoSsrc = ssrc;

            // [Q5 step 5g] The legacy "parse FU header and log" diagnostic
            // block was removed. The variables (fuIndicator, fuHeader,
            // nalType, isStart, isEnd) were only used in a TRACE log that
            // duplicated information the backend already reports. The
            // NAL-type breakdown log on the known-videoSsrc path above
            // still runs because it is the production diagnostic.

            // [Q5 step 5f-receive] H.264 FU-A depacketization is now a
            // backend responsibility (see comment in the main video path
            // above).
            auto depackResult = impl_->backend_->DepacketizeH264(
                memberId, payload, payloadSize, seq, timestamp, markerBit);
            bool isKeyFrame = depackResult.isKeyFrame;
            std::vector<uint8_t> frameData = std::move(depackResult.nalu);            

            if (frameData.empty()) {
                // FU-A fragment incomplete, wait for subsequent fragments
                VCS_MEDIA_LOG_TRACE(">>> RTPENGINE: FU-A waiting (learning): memberId=%s, seq=%u, len=%zu",
                                  memberId.c_str(), seq, payloadSize);
                return;  // Do not push to queue, wait for reassembly to complete
            }

            // Reassembly complete, build EncodedVideoFrame
            EncodedVideoFrame frame;
            frame.data = frameData.data();  // Temporary pointer, will copy on Push
            frame.size = frameData.size();
            frame.timestamp = timestamp;
            frame.sequenceNumber = seq;
            frame.ssrc = ssrc;
            frame.isKeyFrame = isKeyFrame;
            frame.isH239 = false;
            frame.codecType = EncodedVideoFrame::CodecType::H264;

            // Push to queue (consumed by VideoProcessor)
            if (!impl_->videoQueuePool) {
                VCS_MEDIA_LOG_ERROR(">>> RTPENGINE: videoQueuePool is NULL, cannot push video frame!");
            } else {
                PushEncodedVideoFrame(memberId, frame);
            }

            // [Q5 step 5g] Legacy h264_fp dev dump removed (see the
            // comment in the known-videoSsrc path above). The backend
            // and MediaStatisticsCollector are the canonical owners of
            // the receive path.

            // Receive counters: MediaTransportCallback::OnRtpReceived.
        } else {
            // Audio packet (default as audio)
            VCS_MEDIA_LOG_INFO(">>> RTPENGINE: Learning AUDIO SSRC: memberId=%s, oldAudioSsrc=0x%08x, newSsrc=0x%08x",
                              memberId.c_str(), it->second.audioSsrc, ssrc);
            it->second.audioSsrc = ssrc;

            // Receive counters: MediaTransportCallback::OnRtpReceived.

            // Call audio callback (reconstruct the 12-byte header for it)
            if (impl_->audioCallback) {
                std::vector<uint8_t> datagram;
                datagram.resize(12 + payloadSize);
                datagram[0] = 0x80;
                datagram[1] = static_cast<uint8_t>(pt & 0x7F);
                datagram[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
                datagram[3] = static_cast<uint8_t>(seq & 0xFF);
                datagram[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
                datagram[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
                datagram[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
                datagram[7] = static_cast<uint8_t>(timestamp & 0xFF);
                datagram[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
                datagram[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
                datagram[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
                datagram[11] = static_cast<uint8_t>(ssrc & 0xFF);
                if (payloadSize > 0 && payload) {
                    std::memcpy(datagram.data() + 12, payload, payloadSize);
                }
                impl_->audioCallback(memberId, pt,
                                    datagram.data(), datagram.size(),
                                    ssrc, timestamp, seq);
            }
        }

        // [Q5 step 5g] Auto-learn path: the orchestrator just discovered
        // a new remote SSRC and updated MemberMediaContext. The backend
        // (NativeRtpBackend) only knows the SSRCs it was handed at
        // AddMember / UpdateMemberSsrc time, so we must propagate the
        // freshly-learned audio/video SSRC into its ssrcToMember_ index.
        // Without this, the next OnRtpReceived for the SAME auto-learned
        // SSRC would re-enter the auto-learn branch (because
        // FindMemberBySsrc on the backend would miss it), and the
        // backend's send path would build RTP headers with the original
        // (stale) AddMember SSRC.
        //
        // We pass both audioSsrc and videoSsrc (not just the one we just
        // learned) because the backend needs the full picture to keep
        // its index consistent  -  the other slot was either set at
        // AddMember or learned on a previous call.
        if (impl_->backend_) {
            impl_->backend_->UpdateMemberSsrc(
                memberId,
                it->second.audioSsrc,
                it->second.videoSsrc,
                it->second.h239Ssrc);
        }
    }
}

void RTPEngine::SendAudioRtp(const std::string& memberId,
                               const uint8_t* data,
                               size_t len,
                               uint32_t ssrc,
                               uint64_t timestamp,
                               uint16_t sequenceNumber) {
    // 1. Look up the audio PT and the (optional) injection-mode callback.
    //    We no longer need to grab the gateway transport here  -  the
    //    backend owns the transport and we let it call SendRtp()
    //    on it. RTPEngine keeps ownership of the SSRC space and the
    //    statistics counters; everything else is delegated.
    uint8_t audioPt = 0;
    MediaSendCallback callback;
    std::shared_ptr<MediaStatisticsCollector::StreamCounter> audioStats;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->members.find(memberId);
        if (it == impl_->members.end()) {
            VCS_MEDIA_LOG_DEBUG("Member not found...");
            return;
        }
        audioPt = it->second.sendAudioPayloadType;
        callback = impl_->mediaSendCallback;
        audioStats = it->second.audioStats;
        // Track the most recent audio RTP timestamp for the periodic
        // RTCP Sender Report. The SR must carry the RTP timestamp that
        // corresponds to the NTP wall-clock at the sampling instant.
        it->second.lastAudioRtpTimestamp = static_cast<uint32_t>(timestamp);
    }

    // 2. Injection mode: caller wants the raw payload, not a real UDP
    //    send. Update stats and return. The backend is NOT consulted in
    //    this mode (the caller is responsible for transporting the bytes).
    if (callback) {
        callback(memberId, audioPt, data, len, ssrc, timestamp, sequenceNumber);
        if (audioStats) {
            audioStats->AddSent(len, 1);
        } else {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->statsCollector->AddSent(memberId, ::vcs::MediaStreamKind::Audio, len, 1);
        }
        return;
    }

    // 3. [WebRTC media engine] Audio egress is driven by PushCapturedAudio
    //    -> ADM -> AudioSendStream, so any stale encoded packet queued by the
    //    legacy encode loop must not be sent a second time.
    if (impl_->backend_ && impl_->backend_->UsesWebRtcMediaEngine()) {
        return;
    }

    // 4. [Q5 step 5d] Delegate the actual RTP send to the injected
    //    backend. The backend builds the 12-byte RTP header and writes
    //    the resulting datagram to the gateway transport. RTPEngine
    //    keeps ownership of the SSRC space (it pre-allocated the SSRC
    //    and hands it to the backend via BackendMemberConfig), the PT
    //    (read from MemberMediaContext, not from the backend), the
    //    statistics counters, and the injection-mode escape hatch.
    //
    //    Sequence number / timestamp / SSRC are passed through
    //    unchanged  -  the backend does not re-roll them.
    if (!impl_->backend_) {
        VCS_MEDIA_LOG_ERROR("SendAudioRtp: backend is NULL, memberId=%s", memberId.c_str());
        return;
    }
    // [V7.0] Pass the audio kind explicitly. The backend looks up the
    // per-kind transport and ships the packet to the AUDIO socket  -  not
    // to whatever transport happens to be registered for the member.
    vcs::ErrorCode rc = impl_->backend_->SendRtp(
        memberId, ::vcs::MediaStreamKind::Audio,
        audioPt, data, len, ssrc, timestamp, sequenceNumber);

    // 4. Update statistics. The handle was resolved at AddMember time, so
    // this no longer takes the statistics store's mutex.
    if (rc == vcs::ErrorCode::Success) {
        if (audioStats) {
            audioStats->AddSent(len, 1);
        } else {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->statsCollector->AddSent(memberId, ::vcs::MediaStreamKind::Audio, len, 1);
        }
        VCS_MEDIA_LOG_DEBUG("Audio RTP sent: memberId=%s, pt=%u, len=%zu",
                        memberId.c_str(), audioPt, len);
    } else {
        // Throttle: a dead transport fails every packet. Log the first
        // failure and then at most once per second, carrying the running
        // total so the log still proves the stream stayed dead.
        const uint64_t now = GetCurrentTimeMs();
        uint64_t total = 0;
        bool logNow = false;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            auto it = impl_->members.find(memberId);
            if (it != impl_->members.end()) {
                auto& m = it->second;
                total = ++m.audioSendFailures;
                if (m.lastAudioSendFailLogMs == 0 ||
                    now - m.lastAudioSendFailLogMs >= 1000) {
                    m.lastAudioSendFailLogMs = now;
                    logNow = true;
                }
            }
        }
        if (logNow) {
            VCS_MEDIA_LOG_ERROR("Audio RTP send failed: memberId=%s, len=%zu, err=%s, "
                              "totalFailures=%llu (throttled to 1/s)",
                              memberId.c_str(), len, ToString(rc),
                              static_cast<unsigned long long>(total));
        }
    }
}

void RTPEngine::SendVideoRtp(const std::string& memberId,
                               const uint8_t* data,
                               size_t len,
                               uint32_t ssrc,
                               uint64_t timestamp,
                               uint16_t sequenceNumber,
                               bool isKeyFrame,
	uint16_t* outLastSeq,
	vcs::MediaStreamKind kind) {
    // [Q5 step 5d-video] Video send now forwards the whole H.264 NALU
    // (or multi-NALU Annex-B byte stream) to the backend. The backend
    // owns the single-NAL vs FU-A fragmentation policy, the per-fragment
    // RTP header construction, and the marker-bit logic. RTPEngine
    // keeps:
    //   - the video PT (SDP-negotiated, lives in MemberMediaContext)
    //   - the SSRC space (allocated by the orchestrator)
    //   - the statistics counters
    //   - the injection-mode escape hatch
    //
    // isKeyFrame is currently unused in the forwarder body but is
    // kept in the signature for ABI compatibility with all existing
    // call sites (VideoProcessor, MediaTick, SendH239Rtp). It
    // will be propagated to the backend in a future step if the
    // fragmentation policy needs to be keyframe-aware (e.g. to bias
    // the start of FU-A fragments to IDR boundaries).
    (void)isKeyFrame;

    // 1. Look up the video PT and the (optional) injection-mode callback.
    uint8_t videoPt = 0;
    MediaSendCallback callback;
    bool logFirstSend = false;
    std::shared_ptr<MediaStatisticsCollector::StreamCounter> videoStats;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->members.find(memberId);
        if (it == impl_->members.end()) {
            VCS_MEDIA_LOG_DEBUG("Member not found...");
            return;
        }
        videoPt = it->second.sendVideoPayloadType;
        callback = impl_->mediaSendCallback;
        videoStats = (kind == ::vcs::MediaStreamKind::SecondaryVideo)
            ? it->second.secondaryVideoStats
            : it->second.videoStats;
        // Only mark it once the send actually succeeds (below): a rejected
        // send must not consume the one-time "first video RTP" evidence.
        logFirstSend = !it->second.firstVideoSendLogged;
    }

    // 2. Injection mode: caller wants the raw NALU, not a real UDP
    //    send. Update stats and return. The backend is NOT consulted.
    if (callback) {
        callback(memberId, videoPt, data, len, ssrc, timestamp, sequenceNumber);
        if (videoStats) {
            videoStats->AddSent(len, 1);
        } else {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->statsCollector->AddSent(memberId, kind, len, 1);
        }
        return;
    }


    // 3. [Q5 step 5d-video] Delegate to the injected backend. The
    //    backend is responsible for the single-NAL vs FU-A decision
    //    and for building each fragment's 12-byte RTP header.
    if (!impl_->backend_) {
        VCS_MEDIA_LOG_ERROR(">>> VIDEO NOT SENT: backend is NULL for memberId=%s", memberId.c_str());
        return;
    }
    // [V7.0] Pass the requested kind explicitly. Hardcoding Video here
    // routed SecondaryVideo (H.239 content) onto the main video transport,
    // which is a different socket/port.
    vcs::ErrorCode rc = impl_->backend_->SendH264Nalu(
        memberId, kind,
        videoPt, data, len, ssrc, timestamp, sequenceNumber, outLastSeq);

    // 4. Update statistics. The handle was resolved at AddMember time, so
    // this no longer takes the statistics store's mutex.
    if (rc == vcs::ErrorCode::Success) {
        if (videoStats) {
            videoStats->AddSent(len, 1);
        } else {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->statsCollector->AddSent(memberId, kind, len, 1);
        }
        if (logFirstSend) {
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                auto it = impl_->members.find(memberId);
                if (it != impl_->members.end()) {
                    it->second.firstVideoSendLogged = true;
                }
            }
            VCS_MEDIA_LOG_INFO("[VideoPT] first video RTP sent: member=%s, pt=%u",
                               memberId.c_str(), (unsigned)videoPt);
        }
    } else {
        // Throttled like the audio path: log the first failure, then at most
        // once per second, with the running total.
        const uint64_t now = GetCurrentTimeMs();
        uint64_t total = 0;
        bool logNow = false;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            auto it = impl_->members.find(memberId);
            if (it != impl_->members.end()) {
                auto& m = it->second;
                total = ++m.videoSendFailures;
                if (m.lastVideoSendFailLogMs == 0 ||
                    now - m.lastVideoSendFailLogMs >= 1000) {
                    m.lastVideoSendFailLogMs = now;
                    logNow = true;
                }
            }
        }
        if (logNow) {
            VCS_MEDIA_LOG_ERROR(">>> VIDEO SEND FAILED: memberId=%s, kind=%s, len=%zu, err=%s, "
                              "totalFailures=%llu (throttled to 1/s)",
                              memberId.c_str(), MediaStreamKindToString(kind), len, ToString(rc),
                              static_cast<unsigned long long>(total));
        }
    }
}

void RTPEngine::SendH239Rtp(const std::string& memberId,
                              const uint8_t* data,
                              size_t len,
                              uint32_t ssrc,
                              uint64_t timestamp,
                              uint16_t sequenceNumber) {
    VCS_MEDIA_LOG_TRACE("H.239 RTP sent: memberId=%s, ssrc=%u, len=%zu",
                       memberId.c_str(), ssrc, len);
    // H.239 uses the same method as video to send
    SendVideoRtp(memberId, data, len, ssrc, timestamp, sequenceNumber, false);
}

void RTPEngine::SendRtcp(const std::string& memberId,
                           const uint8_t* data,
                           size_t len) {
    // [Q5 step 5d] Delegate RTCP send to the injected backend. RTCP is
    // a single, well-bounded interface  -  no SSRC allocation, no
    // fragmentation, no PT lookup  -  so the forwarder is just a one-liner.
    //
    // Note: the member-existence check is now inside the backend. If the
    // backend reports kMemberNotFound, that means the member was never
    // added (or was removed concurrently)  -  log and return. We do NOT
    // also walk impl_->members here; the backend is the single owner
    // of the transport state and we want the rest of the system to
    // converge on a single source of truth.
    if (!impl_->backend_) {
        VCS_MEDIA_LOG_ERROR("SendRtcp: backend is NULL, memberId=%s", memberId.c_str());
        return;
    }

    VCS_MEDIA_LOG_TRACE("RTCP sending: memberId=%s, len=%zu", memberId.c_str(), len);

    // [V7.0] Pass the audio kind. RTCP is normally co-located with
    // RTP, so the audio RTCP goes through the audio transport. If a
    // future backend needs to send video RTCP through a different
    // socket, RTPEngine must grow a SendVideoRtcp() entry point and
    // pass MediaStreamKind::Video here.
    vcs::ErrorCode rc = impl_->backend_->SendRtcp(
        memberId, ::vcs::MediaStreamKind::Audio, data, len);
    if (rc != vcs::ErrorCode::Success) {
        VCS_MEDIA_LOG_WARNING("RTCP send failed: memberId=%s, len=%zu, err=%s",
                            memberId.c_str(), len, ToString(rc));
    }
}

// ============================================================
// [V7.0 Yealink no-video fix] Build and send a RTCP FIR packet.
//
// FIR (Full Intra Request) is the standard RTCP feedback message defined
// in RFC 5104 ��6.3. It is carried inside a "RTPFB" packet:
//
//   PT = 206 (RTPFB)   FMT = 4 (FIR)
//
// On-wire layout (all multi-byte fields are big-endian):
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|  FMT=4  |  PT=206 (RTPFB) |          length=4            |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                  SSRC of packet sender (local)                |  4 bytes
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                  SSRC of media source (remote)                |  4 bytes
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                       FCI (8 bytes)                          |
//  |  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+    |
//  |  | SSRC of the requested layer (== remoteSsrc)  (4 bytes) |    |
//  |  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+    |
//  |  | Seq nr (8b) |S|F|   Reserved (24b)            (4 bytes) |    |
//  |  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+    |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Total: 4 (header) + 4 (sender ssrc) + 4 (media ssrc) + 8 (FCI) = 20 bytes.
// length field: number of 32-bit words minus 1, INCLUDING the header.
//   = (20 / 4) - 1 = 4.
//
// [Note] simple_rtp/SimpleRtpTransport::SendFir in the gateway layer
//   ships a 16-byte variant with length=2  -  that is technically off
//   by one word vs RFC 3550 ��6.1; the spec says the length field must
//   cover the whole packet. The 20-byte form here matches RFC 5104 /
//   3550 verbatim. Most endpoints (Yealink included) accept either
//   shape, but the 20-byte form is the one that strictly conforms.
// ============================================================
void RTPEngine::SendFir(const std::string& memberId,
	uint32_t remoteSsrc,
	uint32_t localSsrc) {
	// ========================================================
	// 【修复】：如果外部传了 0，强行从上下文中提取真实的 SSRC
	// ========================================================
		{
			std::lock_guard<std::mutex> lk(impl_->mutex);
			auto it = impl_->members.find(memberId);
			if (it != impl_->members.end()) {
				if (remoteSsrc == 0) remoteSsrc = it->second.videoSsrc;
				if (localSsrc == 0) localSsrc = it->second.ssrc;
			}
		}

		if (!impl_->backend_) {
			VCS_MEDIA_LOG_ERROR("SendFir: backend is NULL, memberId=%s", memberId.c_str());
			return;
		}
		// remoteSsrc == 0 means the peer's SSRC is not known yet (no inbound
		// packet, no SDP attribute). Send the bootstrap FIR anyway: endpoints
		// that only start streaming after a keyframe request (Yealink) never
		// send the first packet otherwise. Callers throttle these requests.
		if (remoteSsrc == 0) {
			VCS_MEDIA_LOG_INFO("SendFir: remoteSsrc unknown, sending bootstrap FIR: memberId=%s",
				memberId.c_str());
		}

		// 后续构建 RTCP FIR 包的代码保持不变 ...
		uint8_t packet[20] = { 0 };
		int offset = 0;
		packet[offset++] = 0x84;
		packet[offset++] = 206;
		packet[offset++] = 0x00;
		packet[offset++] = 0x04;
		packet[offset++] = (localSsrc >> 24) & 0xFF;
		packet[offset++] = (localSsrc >> 16) & 0xFF;
		packet[offset++] = (localSsrc >> 8) & 0xFF;
		packet[offset++] = localSsrc & 0xFF;
		packet[offset++] = (remoteSsrc >> 24) & 0xFF;
		packet[offset++] = (remoteSsrc >> 16) & 0xFF;
		packet[offset++] = (remoteSsrc >> 8) & 0xFF;
		packet[offset++] = remoteSsrc & 0xFF;
		packet[offset++] = (remoteSsrc >> 24) & 0xFF;
		packet[offset++] = (remoteSsrc >> 16) & 0xFF;
		packet[offset++] = (remoteSsrc >> 8) & 0xFF;
		packet[offset++] = remoteSsrc & 0xFF;

		static std::atomic<uint8_t> s_firSeq{ 0 };
		uint8_t seq = s_firSeq.fetch_add(1, std::memory_order_relaxed);
		packet[offset++] = seq;
		packet[offset++] = 0x00;
		packet[offset++] = 0x00;
		packet[offset++] = 0x00;

		vcs::ErrorCode rc = impl_->backend_->SendRtcp(
			memberId, ::vcs::MediaStreamKind::Video,
			packet, sizeof(packet));

		if (rc == vcs::ErrorCode::Success) {
			VCS_MEDIA_LOG_INFO(
				"[FIR] Sent Full Intra Request: member=%s remoteSsrc=0x%08X localSsrc=0x%08X seq=%u",
				memberId.c_str(), remoteSsrc, localSsrc, seq);
		}
		else {
			VCS_MEDIA_LOG_ERROR(
				"[FIR] Send failed: member=%s remoteSsrc=0x%08X localSsrc=0x%08X seq=%u err=%s",
				memberId.c_str(), remoteSsrc, localSsrc, seq, ToString(rc));
		}
}

bool RTPEngine::HasMember(const std::string& memberId) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->members.find(memberId) != impl_->members.end();
}

MemberMediaContext* RTPEngine::GetMemberContext(const std::string& memberId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->members.find(memberId);
    if (it != impl_->members.end()) {
        return &it->second;
    }
    return nullptr;
}

int RTPEngine::GetMemberCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return static_cast<int>(impl_->members.size());
}

MediaStatisticsCollector* RTPEngine::GetStatisticsCollector() {
    return impl_->statsCollector.get();
}

bool RTPEngine::FindMemberBySsrc(uint32_t ssrc, std::string& outMemberId) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // [Q5 step 5g] The backend is now the single source of truth for
    // SSRC routing. RTPEngine::Impl::ssrcToMember was removed. A
    // custom backend that inherits the default no-op
    // FindMemberBySsrc (e.g. a future WebRTC backend keyed on
    // PeerConnection rather than SSRC) will simply return false
    // here, which is the correct answer for "we don't know which
    // member this SSRC belongs to".
    if (!impl_->backend_) {
        return false;
    }
    return impl_->backend_->FindMemberBySsrc(ssrc, outMemberId);
}

// ============================================================
// Video frame queue interface implementation
// ============================================================

void RTPEngine::SetVideoFrameQueuePool(std::shared_ptr<VideoFrameQueuePool> queuePool) {
    std::lock_guard<std::mutex> lock(impl_->queueMutex_);
    impl_->videoQueuePool = queuePool;
    VCS_MEDIA_LOG_INFO("RTPEngine SetVideoFrameQueuePool: %s",
                      queuePool ? "set" : "nullptr");
}

bool RTPEngine::PushEncodedVideoFrame(const std::string& memberId, const EncodedVideoFrame& frame) {
    // Use independent queueMutex_ to protect videoQueuePool
    // OnRtpReceived holds mutex_, this holds queueMutex_, no deadlock
    std::lock_guard<std::mutex> lock(impl_->queueMutex_);

    if (!impl_->videoQueuePool) {
        VCS_MEDIA_LOG_ERROR("RTPEngine PushEncodedVideoFrame: videoQueuePool is NULL, cannot push frame");
        return false;
    }

    if (!frame.data || frame.size == 0) {
        VCS_MEDIA_LOG_ERROR("RTPEngine PushEncodedVideoFrame: invalid frame data");
        return false;
    }

    // Get or create member's queue
    VideoFrameQueue* queue = impl_->videoQueuePool->GetQueue(memberId);
    if (!queue) {
        // Queue does not exist, try to create
        if (!impl_->videoQueuePool->CreateQueue(memberId)) {
            VCS_MEDIA_LOG_ERROR("RTPEngine PushEncodedVideoFrame: failed to create queue for memberId=%s",
                              memberId.c_str());
            return false;
        }
        queue = impl_->videoQueuePool->GetQueue(memberId);
        if (!queue) {
            VCS_MEDIA_LOG_ERROR("RTPEngine PushEncodedVideoFrame: queue still NULL after create for memberId=%s",
                              memberId.c_str());
            return false;
        }
        VCS_MEDIA_LOG_DEBUG("RTPEngine PushEncodedVideoFrame: created new queue for memberId=%s", memberId.c_str());
    }

    // Push to queue (no runtime allocation, direct memcpy)
    bool pushed = queue->Push(frame);
    if (pushed) {
        // VCS_MEDIA_LOG_TRACE("RTPEngine PushEncodedVideoFrame: pushed frame to memberId=%s, size=%zu, ts=%llu, keyFrame=%s",
        //                   memberId.c_str(), frame.size,
        //                   (unsigned long long)frame.timestamp,
        //                   frame.isKeyFrame ? "YES" : "NO");
    } else {
        VCS_MEDIA_LOG_WARNING("RTPEngine PushEncodedVideoFrame: queue push failed for memberId=%s, size=%zu",
                            memberId.c_str(), frame.size);
    }

    return pushed;
}

uint8_t RTPEngine::GetMemberAudioSendPayloadType(const std::string& memberId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->members.find(memberId);
    if (it != impl_->members.end()) {
        return it->second.sendAudioPayloadType;
    }
    return 8;  // default PCMA
}

// ============================================================
// Periodic RTCP Sender Report
// ============================================================
//
// RFC 3550 Section 6.1: every active sender MUST ship a compound
// RTCP packet (SR + SDES CNAME) at a rate that keeps receivers
// (and MCUs) informed about the sender's SSRC, clock correlation,
// and cumulative octet/packet count.
//
// Without SR the MCU does not recognise this endpoint as an active
// sender and will not include its uplink audio in the conference
// mix. This is the root cause of the "terminal speaks but nobody
// hears" symptom when the internal pjmedia stream is set to
// DIR_NONE (our external-SDP passthrough mode suppresses
// pjproject's built-in RTCP timer).
//
// The timer fires every 3 s (rtcpSrIntervalMs_). Each tick
// snapshots the member map, builds one SR compound per member,
// and ships it through the backend's SendRtcp.

namespace {

// Convert a steady_clock time-point to a 64-bit NTP timestamp.
// NTP epoch is 1900-01-01; Unix epoch is 1970-01-01.
// Offset = 2208988800 seconds.
uint64_t SteadyNowToNtp64() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = duration_cast<seconds>(now.time_since_epoch()).count();
    auto usecs = duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
    uint64_t ntpSecs = static_cast<uint64_t>(secs) + 2208988800ULL;
    uint32_t ntpFrac = static_cast<uint32_t>((usecs * 4294967296ULL) / 1000000ULL);
    return (ntpSecs << 32) | ntpFrac;
}

} // anonymous namespace

void RTPEngine::SendRtcpSenderReport(const std::string& memberId) {
    if (!impl_->backend_) return;

    // [WebRTC media engine] AudioSendStream emits its own SR/RR with the
    // pacer's real counters; a second report would corrupt the timeline.
    if (impl_->backend_->UsesWebRtcMediaEngine()) return;

    // Snapshot the member state under lock.
    uint32_t localSsrc = 0;
    uint32_t lastRtpTs = 0;
    uint32_t packetsSent = 0;
    uint64_t bytesSent = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->members.find(memberId);
        if (it == impl_->members.end()) return;

        localSsrc = it->second.ssrc;
        lastRtpTs = it->second.lastAudioRtpTimestamp;

        // Read cumulative tx counters from the statistics collector.
        vcs::MediaInfoSnapshot snap;
        if (impl_->statsCollector && impl_->statsCollector->GetSnapshot(memberId, snap)) {
            const auto* audio = snap.Find(::vcs::MediaStreamKind::Audio);
            if (audio) {
                packetsSent = audio->packetsSent;
                bytesSent = audio->bytesSent;
            }
        }
    }

    if (localSsrc == 0) {
        // No local SSRC assigned yet; nothing to report.
        return;
    }

    // Build the RTCP SR compound (SR + SDES CNAME).
    IRtpBackend::RtcpReportRequest req;
    req.generateSenderReport = true;
    req.ssrcSender = localSsrc;
    req.ntpTimestamp = SteadyNowToNtp64();
    req.rtpTimestamp = lastRtpTs;
    req.packetCount = packetsSent;
    req.octetCount = static_cast<uint32_t>(bytesSent);
    req.cname = memberId;  // Stable per-session identifier

    auto compound = impl_->backend_->BuildRtcpReport(memberId, req);
    if (compound.empty()) {
        VCS_MEDIA_LOG_TRACE("RTCP SR build returned empty: memberId=%s", memberId.c_str());
        return;
    }

    // Ship it through the audio transport.
    vcs::ErrorCode rc = impl_->backend_->SendRtcp(
        memberId, ::vcs::MediaStreamKind::Audio,
        compound.data(), compound.size());
    if (rc != vcs::ErrorCode::Success) {
        VCS_MEDIA_LOG_WARNING("RTCP SR send failed: memberId=%s, err=%s",
                             memberId.c_str(), ToString(rc));
    } else {
        VCS_MEDIA_LOG_DEBUG("RTCP SR sent: memberId=%s, ssrc=%u, pkts=%u, bytes=%llu, size=%zu",
                           memberId.c_str(), localSsrc, packetsSent,
                           (unsigned long long)bytesSent, compound.size());
    }
}

void RTPEngine::StartRtcpTimer() {
    if (impl_->rtcpTimerThread_.joinable()) return;  // already running

    impl_->rtcpTimerStop_.store(false, std::memory_order_release);
    impl_->rtcpTimerThread_ = std::thread([this]() {
        VCS_MEDIA_LOG_INFO("RTCP SR timer started (interval=%d ms)", Impl::rtcpSrIntervalMs_);
        while (!impl_->rtcpTimerStop_.load(std::memory_order_acquire)) {
            // Sleep in 100 ms increments so we can exit promptly.
            for (int i = 0; i < Impl::rtcpSrIntervalMs_ / 100 && !impl_->rtcpTimerStop_.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (impl_->rtcpTimerStop_.load(std::memory_order_acquire)) break;

            // Snapshot the member list under lock, then send SR for each.
            std::vector<std::string> memberIds;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                memberIds.reserve(impl_->members.size());
                for (const auto& kv : impl_->members) {
                    memberIds.push_back(kv.first);
                }
            }

            for (const auto& id : memberIds) {
                SendRtcpSenderReport(id);
            }
        }
        VCS_MEDIA_LOG_INFO("RTCP SR timer stopped");
    });
}

void RTPEngine::StopRtcpTimer() {
    impl_->rtcpTimerStop_.store(true, std::memory_order_release);
    if (impl_->rtcpTimerThread_.joinable()) {
        impl_->rtcpTimerThread_.join();
    }
}

} // namespace media
} // namespace vcs
