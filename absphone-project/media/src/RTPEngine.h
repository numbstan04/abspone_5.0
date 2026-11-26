#pragma once

#include "media_export.h"
#include "IRtpTransport.h"
#include "IMediaControl.h"   // [V7.0] brings MemberMediaTransports
#include "MediaStatistics.h"
#include "VideoFrameQueue.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace vcs { namespace base {
//class IConfig;
} }

namespace vcs { namespace media {

// [Q5] Forward declaration of the new IRtpBackend interface. The full
// definition lives in IRtpBackend.h and is NOT included here so that
// the rest of RTPEngine can be compiled in isolation. Only the new
// dependency-injecting constructor needs the full type, and that
// constructor is defined out-of-line in RTPEngine.cpp.
class IRtpBackend;

// Member context
struct MemberMediaContext {
    std::string memberId;
    uint32_t ssrc = 0;               // Local SSRC (for sending)
    uint32_t audioSsrc = 0;          // Audio SSRC
    uint32_t videoSsrc = 0;          // Video SSRC
    uint32_t h239Ssrc = 0;           // H.239 dual-stream SSRC
    uint32_t remoteSsrc = 0;         // [Key Fix] Learned remote SSRC
    // [V7.0] Per-kind transports. Replacing the legacy single `transport`
    // pointer. RTPEngine does not call SendRtp directly any more; the
    // backend owns the transports. We keep a copy here for diagnostics and
    // for legacy code paths that still read ctx.transport (which now
    // returns the audio transport, if any).
    MemberMediaTransports transports;
    // Convenience: pointer to the audio transport. nullptr if not set.
    std::shared_ptr<IRtpTransport> transport;  // legacy alias
    bool isConnected = false;

    //  PT value for sending (dynamically determined by SDP negotiation)
    // Default: PCMU=0 (audio), H264=97 (video)
    // Gateway layer updates these values at OnCallMediaReady
    uint8_t sendAudioPayloadType = 0;   // PT for sending audio
    uint8_t sendVideoPayloadType = 97;  // PT for sending video

    //  PT registered from the negotiated SDP, used to classify inbound
    // packets. -1 = not negotiated (fall back to MediaConfig lists); 0 is a
    // valid PT (PCMU) and must not double as "unset".
    int16_t negotiatedAudioPt = -1;
    int16_t negotiatedVideoPt = -1;

    // One-time INFO when the first video RTP leaves, so the on-wire PT can
    // be checked against the SDP without per-packet logging.
    bool firstVideoSendLogged = false;

    // Send-failure throttling. A dead transport rejects every packet, and
    // the audio/video send paths run at 50 / 30+ packets per second; logging
    // every failure would flood the log and hide the first occurrence.
    // These counters are only touched on the failure path, under Impl::mutex:
    // log the first failure, then at most once per second while it persists.
    uint64_t audioSendFailures = 0;
    uint64_t videoSendFailures = 0;
    uint64_t lastAudioSendFailLogMs = 0;
    uint64_t lastVideoSendFailLogMs = 0;

    // Last audio RTP timestamp sent. Updated on every SendAudioRtp call.
    // Used by the periodic RTCP Sender Report to correlate NTP wall-clock
    // with the RTP clock (RFC 3550 Section 6.4.1).
    uint32_t lastAudioRtpTimestamp = 0;

    // Resolved once per member so the per-packet send path updates counters
    // without taking MediaStatisticsCollector's control-plane mutex.
    std::shared_ptr<MediaStatisticsCollector::StreamCounter> audioStats;
    std::shared_ptr<MediaStatisticsCollector::StreamCounter> videoStats;
    std::shared_ptr<MediaStatisticsCollector::StreamCounter> secondaryVideoStats;
};

// RTP packet callback (for dispatching to processors)
using RtpPacketCallback = std::function<void(
    const std::string& memberId,
    uint8_t payloadType,    // Payload type 0=PCMU, 8=PCMA, 96+=dynamic
    const uint8_t* data,
    size_t len,
    uint32_t ssrc,
    uint64_t timestamp,
    uint16_t sequenceNumber)>;

// RTCP callback
using RtcpCallback = std::function<void(
    const std::string& memberId,
    const uint8_t* data,
    size_t len)>;

// Inbound keyframe request (RTCP PSFB PLI / FIR) from a member. MediaControl
// wires this to the video encoder so a peer asking for an IDR gets one.
// kind is the media stream the RTCP arrived on (the callback object is bound
// per MediaStreamKind), not derived from the packet.
using RemoteKeyFrameRequestCallback = std::function<void(
    const std::string& memberId,
    vcs::MediaStreamKind kind)>;

// ============================================================
// Media send callback interface
//
// Injection mode: RTPEngine does not send RTP directly, but notifies upper layer via this callback
// Upper layer (gateway) is responsible for writing data to PJSIP conference
// ============================================================
using MediaSendCallback = std::function<void(
    const std::string& memberId,
    uint8_t payloadType,
    const uint8_t* data,
    size_t len,
    uint32_t ssrc,
    uint64_t timestamp,
    uint16_t sequenceNumber)>;

// Media configuration
struct MediaConfig {
    bool enableAudio = true;
    bool enableVideo = true;
    bool enableRtcp = true;
    uint16_t rtcpPort = 0;           // 0 means same port as RTP
    uint32_t maxBandwidth = 4000000; // bps

    //  Video payload type configuration (dynamic configuration, adapt to different devices)
    // Common values:
    //   H.264: 97 (standard), 98, 99, 100+ (dynamic)
    //   H.263: 34 (standard), 96 (dynamic)
    //   VP8/VP9: 100+ (dynamic)
    // [Key Fix] Expand default support range, include common H.264/H.263 PT values
    // Devices like Yealink may use non-standard PT values (e.g., 34), must support
    std::vector<uint8_t> videoPayloadTypes = {97, 34, 98, 99, 100, 101, 102};

    // Audio payload type configuration (dynamic configuration)
    // Common values:
    //   PCMU: 0, PCMA: 8, G722: 9, opus: 100+
    std::vector<uint8_t> audioPayloadTypes = {0, 8, 9, 100, 101, 102};
};

/**
 * @brief RTP transport engine
 *
 * Responsible for all RTP/RTCP packet sending and receiving. The current
 * implementation uses IRtpBackend (Q5) with a software/FFmpeg-based backend;
 * WebRTC integration is a planned future extension but is not active code
 * at this point (Q8 comment fix, 2026-08-07).
 *
 * Functions:
 * 1. Member management - Manage media connections for all participants
 * 2. RTP send/receive - Send/receive RTP packets via IRtpTransport
 * 3. Packet dispatch - Dispatch to corresponding processors by SSRC/payload type
 * 4. Statistics collection - Collect RTP layer statistics
 * 5. RTCP processing - RTCP feedback (SR, RR, REMB, etc.)
 *
 * Design principles:
 * 1. Integrate WebRTC RTP processing capabilities (NACK, FEC, REMB, etc.)
 * 2. Efficient packet dispatch, avoid lock contention
 * 3. Thread-safe
 */
class RTPEngine {
public:
    // [Q5] The default constructor is retained for backward compatibility
    // with every existing call site. It internally constructs a default
    // IRtpBackend (currently NativeRtpBackend) via BackendFactory. New
    // code should prefer the explicit-injection constructor below so the
    // backend choice lives at the top of the dependency graph.
    VCS_MEDIA_API RTPEngine();

    /// [Q5] Dependency-injecting constructor. The caller (typically
    /// MediaControl or a test) owns the choice of backend. See
    /// BackendFactory::Create() for the standard construction paths.
    explicit VCS_MEDIA_API RTPEngine(std::unique_ptr<IRtpBackend> backend);

    VCS_MEDIA_API ~RTPEngine();

    /// [Q5] Returns the backend type string (e.g. "NativeRtp"). Useful
    /// for logs, metrics and crash diagnostics. Returns "none" if no
    /// backend is currently attached (should not happen in normal use).
    VCS_MEDIA_API std::string GetBackendType() const;

    // Initialization
    VCS_MEDIA_API void Initialize(const MediaConfig& config);

    // ============================================================
    // [V7.0+] Dependency-injecting Initialize. Same as the
    // single-argument overload above, but additionally reads the
    // h264-dump debug feature from the supplied IConfig instance:
    //
    //   media.debug.h264_dump.enable  (bool, default false)
    //   media.debug.h264_dump.dir     (string, default "G:/tmp")
    //
    // The h264-dump feature is OFF by default; set the keys in the
    // project config file to enable it for diagnostic sessions.
    // Passing a null IConfig is safe and is equivalent to the
    // single-argument overload (the dump feature stays disabled).
    //
    // Existing test code that only knows about Initialize(MediaConfig)
    // keeps working unchanged.
    // ============================================================
    VCS_MEDIA_API void Initialize(const MediaConfig& config,
                                 const std::shared_ptr<base::IConfig>& debugConfig);

    // [V7.0+] Public read-only accessor for the h264-dump feature
    // switch. Returns false if the feature was never enabled via the
    // two-argument Initialize. Used internally by HandleParsedRtp;
    // exposed publicly for unit tests and runtime diagnostics.
    VCS_MEDIA_API bool IsH264DumpEnabled() const;

    // [V7.0] Add a member with the full set of media transports.
    // The map may be empty (e.g. when called from OnMemberCodecNegotiated
    // before the SDP is fully negotiated). The map is forwarded to the
    // backend which is responsible for storing per-kind transports.
    VCS_MEDIA_API void AddMember(const std::string& memberId,
                                  uint32_t ssrc,
                                  const MemberMediaTransports& transports,
                                  uint32_t audioSsrc = 0);
    VCS_MEDIA_API void UpdateMemberSsrc(const std::string& memberId,
                                         uint32_t audioSsrc,
                                         uint32_t videoSsrc,
                                         uint32_t h239Ssrc = 0);
    VCS_MEDIA_API void RemoveMember(const std::string& memberId);
    VCS_MEDIA_API void ClearAllMembers();

    //  Update member send PT value
    // Called by gateway layer at OnCallMediaReady, passing negotiated PT values
    // @param audioPayloadType PT for sending audio (e.g., 0=PCMU, 8=PCMA)
    // @param videoPayloadType PT for sending video (e.g., 97=H264)
    VCS_MEDIA_API void UpdateMemberSendPayloadType(const std::string& memberId,
                                                    uint8_t audioPayloadType,
                                                    uint8_t videoPayloadType);

    //  Update ONLY the audio send PT from the negotiated SDP.
    // Single-purpose on purpose: the audio path has no way to read back the
    // current video PT, so reusing the dual setter would clobber it.
    // The value is also registered as the member's negotiated audio PT and
    // takes precedence over MediaConfig::audioPayloadTypes when classifying
    // inbound packets (SDP assigns one PT per m-line for both directions).
    VCS_MEDIA_API void UpdateMemberAudioSendPayloadType(const std::string& memberId,
                                                        uint8_t audioPayloadType);

    //  Update ONLY the video send PT from the negotiated SDP. Symmetric with
    // the audio setter: send path reads it per packet, receive classification
    // prefers it over MediaConfig::videoPayloadTypes.
    VCS_MEDIA_API void UpdateMemberVideoSendPayloadType(const std::string& memberId,
                                                        uint8_t videoPayloadType);

    // ============================================================
    // [Key Fix 2026-08-11] Explicit "open the receive gate" API.
    //
    // Problem this solves:
    //   Previously, RTPEngine::Initialize() eagerly installed the backend
    //   receive callback (InstallBackendReceiveCallback). From that moment
    //   on, any UDP datagram the backend pulled off the socket would be
    //   routed to HandleParsedRtp - but HandleParsedRtp looks up
    //   members_ to dispatch, and members_ is only populated by AddMember.
    //   Any RTP packet that arrived between Initialize() and the first
    //   AddMember() was silently dropped with "Unknown member". For H.264
    //   video the first packet is almost always a key frame (SPS/PPS/IDR);
    //   losing it makes the decoder fail and the call never recovers.
    //
    // New contract:
    //   1. Initialize() now only stores config; it does NOT install
    //      the receive callback. From Initialize() until BeginReceive(),
    //      any packet the backend receives is dropped at the callback
    //      boundary (the std::function is null) - which is safe because
    //      the caller has not yet promised to deliver any packets.
    //   2. The caller (MediaControl::AddMemberStreams) MUST call
    //      BeginReceive() exactly once, after every AddMember it wants
    //      to be ready to receive for. This is the "open the gate" point.
    //   3. HandleParsedRtp is still defensively guarded against an empty
    //      members_ (logs and drops with diagnostics), but a correctly
    //      written caller will never hit that branch.
    //   4. StopReceive() is the symmetric "close the gate" - used by
    //      RemoveMember/ClearAllMembers and the destructor to detach
    //      before teardown. It is idempotent.
    // ============================================================
    VCS_MEDIA_API void BeginReceive();
    VCS_MEDIA_API void StopReceive();
    VCS_MEDIA_API bool IsReceiving() const;

    // Set processor callbacks (receive data from processors for sending)
    VCS_MEDIA_API void SetAudioOutputCallback(RtpPacketCallback callback);
    VCS_MEDIA_API void SetVideoOutputCallback(RtpPacketCallback callback);
    VCS_MEDIA_API void SetDualOutputCallback(RtpPacketCallback callback);
    VCS_MEDIA_API void SetRtcpCallback(RtcpCallback callback);

    // Called when a member asks us for a keyframe (RTCP PLI / FIR). The
    // handler must be cheap: it runs on the transport's receive thread.
    VCS_MEDIA_API void SetRemoteKeyFrameRequestCallback(RemoteKeyFrameRequestCallback callback);

    // Set media send callback (injection mode)
    // When this callback is set, RTPEngine calls this callback instead of transport->SendRtp()
    // This allows gateway layer to write data to PJSIP conference for encoding and sending
    VCS_MEDIA_API void SetMediaSendCallback(MediaSendCallback callback);

    // Check if using injection mode
    VCS_MEDIA_API bool IsUsingInjectionMode() const;

    // Receive RTP packet (from Gateway/IRtpTransport)
    VCS_MEDIA_API void OnRtpReceived(const std::string& memberId,
                                      const RtpPacket& packet);

    // [Q5 step 5e] Receive an already-parsed RTP packet. Used by
    // MediaWorker to dispatch MediaPackets it drained from the per-
    // conference SPSC queue without re-serialising the RTP header.
    // The backend fires the RtpReceiveCallback with the supplied
    // fields  -  no header re-parse happens on this path.
	VCS_MEDIA_API void OnRtpReceivedParsed(const std::string& memberId,
		uint8_t payloadType,
		const uint8_t* data, size_t len,
		uint32_t ssrc, uint64_t timestamp,
		uint16_t sequenceNumber, bool markerBit);

    // Receive RTCP packet
    VCS_MEDIA_API void OnRtcpReceived(const std::string& memberId,
                                       const RtcpPacket& packet);

    // Entry point for transport-level RTCP (MediaTransportCallback). Parses
    // compound packets and turns PSFB PLI / FIR into a keyframe request on
    // the local encoder; other packet types are forwarded to RtcpCallback.
    // kind is the stream the packet arrived on. Runs on the transport's
    // receive thread - must stay non-blocking.
    VCS_MEDIA_API void HandleInboundRtcp(const std::string& memberId,
                                          vcs::MediaStreamKind kind,
                                          const uint8_t* data, size_t len);

    // Send audio RTP (called by AudioProcessor)
    VCS_MEDIA_API void SendAudioRtp(const std::string& memberId,
                                     const uint8_t* data,
                                     size_t len,
                                     uint32_t ssrc,
                                     uint64_t timestamp,
                                     uint16_t sequenceNumber);

    // Send video RTP (called by VideoProcessor)
    //
    // kind is the MediaStreamKind the encoder produced this access unit for.
    // It is used for statistics attribution only (main vs secondary video);
    // egress routing stays the backend's per-kind transports.
    VCS_MEDIA_API void SendVideoRtp(const std::string& memberId,
                                     const uint8_t* data,
                                     size_t len,
                                     uint32_t ssrc,
                                     uint64_t timestamp,
                                     uint16_t sequenceNumber,
                                     bool isKeyFrame,
		uint16_t* outLastSeq = nullptr,
		vcs::MediaStreamKind kind = vcs::MediaStreamKind::Video);

    // Send H.239 dual-stream RTP (called by DualProcessor)
    VCS_MEDIA_API void SendH239Rtp(const std::string& memberId,
                                     const uint8_t* data,
                                     size_t len,
                                     uint32_t ssrc,
                                     uint64_t timestamp,
                                     uint16_t sequenceNumber);

    // Send RTCP (SR/RR/REMB, etc.)
    VCS_MEDIA_API void SendRtcp(const std::string& memberId,
                                 const uint8_t* data,
                                 size_t len);

    // ============================================================
    // [V7.0 Yealink no-video fix] Send a RTCP FIR (Full Intra Request)
    //   on the *video* RTCP transport of `memberId`. Yealink SIP
    //   endpoints will not push an IDR key-frame on their own; they
    //   wait for the MCU to ask for one. If we never ask, the chain
    //   only sees P-frames, the decoder produces a 0-byte "ghost"
    //   YUV frame, and the encode step fails  -  resulting in the
    //   classic "Yealink joins, no video" symptom.
    //
    //   This wraps the RFC 5104 ��6.3 FIR (PT=206 RTPFB / FMT=4) packet
    //   construction. Internally it routes through the backend with
    //   MediaStreamKind::Video so the RTCP datagram lands on the
    //   video per-kind transport (audio RTCP is currently hard-coded
    //   in SendRtcp() above and is the wrong destination for a FIR).
    //
    //   Thread safety: safe to call from any thread. The backend
    //   takes its own per-member lock; the orchestrator does not
    //   need an additional lock.
    //
    //   @param memberId   Yealink-side member (looked up via backend)
    //   @param remoteSsrc Yealink video SSRC we want to force a key-frame
    //                      from (the SSRC captured from inbound RTP)
    //   @param localSsrc  Our own video SSRC (the FIR packet sender).
    //                      If 0, we look it up from MemberMediaContext.
    //                      0 is also accepted by RFC 5104 (reserved).
    // ============================================================
    VCS_MEDIA_API void SendFir(const std::string& memberId,
                                uint32_t remoteSsrc,
                                uint32_t localSsrc);

    // Get member information
    VCS_MEDIA_API bool HasMember(const std::string& memberId) const;
    VCS_MEDIA_API MemberMediaContext* GetMemberContext(const std::string& memberId);
    VCS_MEDIA_API int GetMemberCount() const;

    // Statistics query
    VCS_MEDIA_API MediaStatisticsCollector* GetStatisticsCollector();

    // Find member by SSRC (return result via reference parameter)
    VCS_MEDIA_API bool FindMemberBySsrc(uint32_t ssrc, std::string& outMemberId) const;

    // ============================================================
    // Video frame queue interface
    // RTPEngine as producer, pushes video frames to VideoFrameQueue
    // VideoProcessor as consumer, fetches frames from queue for decoding and composition
    // ============================================================

    // Set video frame queue pool
    VCS_MEDIA_API void SetVideoFrameQueuePool(std::shared_ptr<VideoFrameQueuePool> queuePool);

    // Push encoded frame to corresponding member's queue (called by RTPEngine as producer)
    VCS_MEDIA_API bool PushEncodedVideoFrame(const std::string& memberId, const EncodedVideoFrame& frame);

	VCS_MEDIA_API uint8_t GetMemberAudioSendPayloadType(const std::string& memberId);

    // ============================================================
    // [WebRTC media engine] Audio bridge
    // ============================================================
    //
    // When the backend owns the libwebrtc media engine, the audio path is
    // driven by PCM instead of encoded RTP:
    //   capture : AudioProcessor -> PushCapturedAudio -> ADM -> AudioSendStream
    //   playout : NetEq -> ADM -> SetAudioPlayoutPcmCallback -> AudioProcessor
    VCS_MEDIA_API void SetAudioPlayoutPcmCallback(
        std::function<void(const std::string&,
            const int16_t*, size_t, int, int)> callback);

    VCS_MEDIA_API bool PushCapturedAudio(const std::string& memberId,
        const int16_t* pcm, size_t samplesPerChannel,
        int sampleRate, int channels);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // ----------------------------------------------------------------
    // Receive-side data plane (Q5 step 5d-receive)
    //
    // OnRtpReceived (the public, gateway-facing entry point) is a
    // thin forwarder to backend_->OnRtpReceived. The backend parses
    // the RTP header and then calls back into this method with the
    // already-parsed (pt, ssrc, ts, seq, payload, len) tuple.
    //
    // HandleParsedRtp is the legacy body that used to live in
    // OnRtpReceived: SSRC routing, auto-learn, H.264 FU-A
    // depacketization, and dispatch to audio/video/dual callbacks
    // (which in turn feed AudioProcessor / VideoProcessor).
    //
    // Splitting the public entry point from the depacketization body
    // lets us install HandleParsedRtp as the backend's
    // RtpReceiveCallback at Initialize() time, while the public
    // OnRtpReceived API stays stable for every existing call site
    // (MediaWorker, MediaTransportCallback, ...).
    // ----------------------------------------------------------------
	void HandleParsedRtp(const std::string& memberId,
		uint8_t  pt,
		const uint8_t* payload, size_t   payloadLen,
		uint32_t ssrc, uint64_t timestamp,
		uint16_t sequenceNumber, bool markerBit);

    // [Q5 step 5d-receive] RTCP counterpart of HandleParsedRtp. The
    // backend already demuxed the datagram before calling us, so we
    // just forward raw bytes to the registered RTCP callback (e.g. for
    // RTCP XR / statistics).
    void HandleParsedRtcp(const std::string& memberId,
                          const uint8_t* data, size_t len);

    /// Install / clear the backend receive callback. Called from
    /// Initialize() (install) and ~RTPEngine() (clear).
    void InstallBackendReceiveCallback();
    void ClearBackendReceiveCallback();

    // Build and send an RTCP Sender Report (SR + SDES CNAME compound)
    // for the audio stream of `memberId`. Called periodically by the
    // RTCP timer thread. The MCU uses RTCP SR to detect active senders
    // and include their uplink audio in the conference mix.
    void SendRtcpSenderReport(const std::string& memberId);

    // Start / stop the periodic RTCP SR timer. StartRtcpTimer() is
    // called from Initialize(); StopRtcpTimer() from the destructor.
    void StartRtcpTimer();
    void StopRtcpTimer();
};

} // namespace media
} // namespace vcs
