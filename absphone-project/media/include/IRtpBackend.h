/**
 * @file IRtpBackend.h
 * @brief RTP transport backend abstraction (Q5)
 *
 * [Design principles]
 * 1. "What", not "how": the interface describes what a backend must do, not
 *    how it does it. Codec logic stays in VideoProcessor / AudioProcessor;
 *    business-member management stays in RTPEngine.
 * 2. One implementation = one transport protocol (UDP, WebRTC, SRT, QUIC...).
 *    A new protocol is added by writing a new class that implements this
 *    interface and injecting it into RTPEngine. RTPEngine itself does not
 *    change.
 * 3. Zero business concepts: there is no ConferenceId, no business member
 *    mapping, no codec negotiation. The interface deals only with
 *    `memberId` (an opaque string owned by the caller) and raw byte
 *    payloads.
 * 4. SSRC is injected from the outside: the backend does NOT allocate
 *    SSRCs. RTPEngine is the single owner of the SSRC space and passes
 *    pre-allocated values through BackendMemberConfig.
 * 5. Callbacks carry only raw data: RTCP parsing, RTCP aggregation and
 *    statistics composition are RTPEngine's responsibility.
 * 6. GetBackendType() is mandatory: used for log routing, metrics tagging
 *    and crash diagnostics so we always know whether a given conference
 *    is using UDP vs WebRTC vs SRT.
 * 7. Thread safety: callbacks are invoked by the backend's internal
 *    threads. RTPEngine must be prepared to receive them concurrently
 *    with its own API calls.
 *
 * [Header policy]
 * - All companion POD types (BackendMemberConfig, BackendMediaConfig,
 *   BackendStatistics) live in this header. The full surface is < 300
 *   lines, which is the design target.
 * - No external header from the gateway / codec modules is included here,
 *   so this header can be built into a shared library independently.
 *
 * [Error code contract]
 * - 2026-08-07: All return values unified to `vcs::ErrorCode`
 *   (absorbed the former media::BackendErrorCode into the common header).
 *   This is the single error vocabulary for the whole project.
 *
 * [References]
 * - Section 20.4  (Q5 overall architecture)
 * - Section 20.5  (Q5 IRtpBackend interface design - this file)
 * - Section 20.6  (Q5 RTPEngine orchestrator)
 * - Section 20.7  (Q5 NativeRtpBackend layout)
 */

#pragma once

#include "ErrorCode.h"   // [v1.1] unified error code
#include "media_export.h"
#include "MediaStreamKind.h"
#include "IRtpTransport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vcs {
	namespace media {

		// ============================================================
		// Forward declarations
		// ============================================================

		struct BackendMemberConfig;
		struct BackendMediaConfig;
		struct BackendStatistics;

		// ============================================================
		// Callback type aliases
		// ============================================================

		/// [V7.0] Per-kind transport map; canonical definition in common.
		using vcs::MemberMediaTransports;

		/// Received an RTP packet (called on backend's internal thread).
		using RtpReceiveCallback = std::function<void(
			const std::string& memberId,
			uint8_t payloadType,
			const uint8_t* data,
			size_t len,
			uint32_t ssrc,
			uint64_t timestamp,
			uint16_t sequenceNumber,
			bool markerBit)>;

		/// Received an RTCP packet (raw, not yet parsed).
		using RtcpReceiveCallback = std::function<void(
			const std::string& memberId,
			const uint8_t* data,
			size_t len)>;

		/// Periodic statistics snapshot emitted by the backend.
		// This is the backend-level type. The former RTPEngine-level
		// `StatisticsCallback`/`StatisticsSnapshot` pair was removed: the
		// canonical per-member store is MediaStatisticsCollector, which feeds
		// MediaInfoSnapshot. Do not re-add a second statistics channel here.
		using BackendStatisticsCallback = std::function<void(
			const std::string& memberId,
			const BackendStatistics& stats)>;

		// ============================================================
		// Network event enum
		// ============================================================

		/**
		 * @brief Protocol-agnostic network lifecycle event.
		 *
		 * Concrete backends map their own state machines onto these four buckets:
		 *   - UDP: socket error / interface down / re-bind
		 *   - WebRTC: ICE state (failed / disconnected / checking)
		 *   - SRT: connection broken / reconnecting / connected
		 */
		enum class NetworkEvent : uint8_t {
			kConnected = 0,
			kDisconnected = 1,
			kReconnecting = 2,
			kFailed = 3
		};

		using NetworkEventCallback = std::function<void(
			const std::string& memberId,
			NetworkEvent event)>;

		// ============================================================
		// Companion data structures
		// ============================================================

		/// Per-member configuration handed to the backend at AddMember().
		struct BackendMemberConfig {
			std::string memberId;             // Opaque to the backend
			std::string remoteAddress;        // IP for UDP, peer ID for WebRTC, etc.
			uint16_t    remotePort = 0;     // Ignored for WebRTC
			uint16_t    localPort = 0;     // 0 = OS-assigned
			uint8_t     payloadTypeAudio = 0; // e.g. 0=PCMU, 8=PCMA
			uint8_t     payloadTypeVideo = 97;// e.g. 97=H.264
			bool        enableAudio = true;
			bool        enableVideo = true;
			bool        enableRtcp = true;
			uint32_t    ssrcAudio = 0;    // Allocated by RTPEngine
			uint32_t    ssrcVideo = 0;    // Allocated by RTPEngine
		};

		/// Backend-wide configuration handed at Initialize().
		struct BackendMediaConfig {
			uint32_t    maxBandwidth = 4000000; // bps
			uint16_t    rtcpPort = 0;          // 0 = same port as RTP
			std::string bindAddress;                       // empty = any
			int         statisticsIntervalMs = 1000;       // callback period
		};

		/// Lightweight statistics emitted by the backend. Not wired into the
		/// media-info path: the canonical store is MediaStatisticsCollector,
		/// which is fed by MediaTransportCallback (ingress) and RTPEngine
		/// (egress) and read out as MediaInfoSnapshot.
		struct BackendStatistics {
			uint32_t packetsSent = 0;
			uint32_t packetsReceived = 0;
			uint32_t packetsLost = 0;
			double   fractionLost = 0.0;
			uint32_t jitter = 0;   // ms
			uint32_t roundTripTime = 0;   // ms
			uint64_t bytesSent = 0;
			uint64_t bytesReceived = 0;
		};

		// ============================================================
		// IRtpBackend interface
		// ============================================================

		/**
		 * @brief RTP transport backend abstraction.
		 *
		 * One implementation per transport protocol. RTPEngine holds exactly one
		 * instance of a concrete backend and forwards every RTP/RTCP operation to
		 * it. See Section 20.4 of the design document for the overall picture.
		 */
		class VCS_MEDIA_API IRtpBackend {
		public:
			virtual ~IRtpBackend() = default;

			// ---------------------- Lifecycle ----------------------

			/// Open sockets / ICE agent / SRT socket and prepare for traffic.
			/// Must be called exactly once before any other API. The default
			/// implementation is a no-op (backends whose `Initialize` only
			/// needs to remember a config can ignore the call); backends that
			/// need to do real I/O work (open sockets, allocate an ICE agent,
			/// etc.) override it.
			virtual vcs::ErrorCode Initialize(const BackendMediaConfig& config) = 0;

			/// [Yealink no-video fix] Transition the backend to the "running"
			/// state. A freshly-constructed backend is in a default-deny
			/// posture  -  for example, NativeRtpBackend rejects every
			/// AddMember with `MediaTransportNotReady` because its internal
			/// `running_` flag starts as `false`. Without Start(), the
			/// orchestrator (RTPEngine) silently drops every member, so the
			/// far-end never receives any media and the call ends with a
			/// "video timeout".
			///
			/// Start() MUST be called after Initialize() and before the first
			/// AddMember(). Idempotent: calling it more than once is a no-op
			/// (the first call wins). Symmetric to Shutdown()  -  after
			/// Shutdown() the backend returns to the default-deny posture and
			/// a subsequent Start() re-enables it.
			///
			/// Concrete backends are expected to (1) set their `running_`
			/// flag, and (2) start any background worker threads (e.g. the
			/// native UDP backend's stats emitter) here. They are NOT
			/// expected to re-open sockets; that is Initialize()'s job.
			virtual void Start() = 0;

			/// Symmetric to Initialize(). Must be safe to call from a destructor.
			/// Idempotent: calling twice is a no-op.
			virtual void Shutdown() noexcept = 0;

			// ---------------------- Member management ----------------------

			/// Register a new member. The backend only establishes the transport
			/// channel; it does NOT allocate SSRCs (those were pre-filled in
			/// `config.ssrcAudio / config.ssrcVideo` by RTPEngine).
			virtual vcs::ErrorCode AddMember(
				const std::string& memberId,
				const BackendMemberConfig& config) = 0;

			/// Tear down the transport channel for `memberId`. Safe to call on
			/// an unknown member (no-op).
			virtual void RemoveMember(const std::string& memberId) noexcept = 0;

			/// [Q5 step 5g] Update the SSRC routing for an existing member.
			/// The orchestrator (RTPEngine) calls this after SDP renegotiation
			/// changes audioSsrc / videoSsrc / h239Ssrc. The backend must:
			///   1. Remove the OLD audio/video/h239 SSRCs from its ssrcToMember_
			///      index (they still belong to the same memberId, but the
			///      memberId mapping has changed and the old SSRCs may now be
			///      garbage-collected).
			///   2. Register the NEW audio/video/h239 SSRCs under the same
			///      memberId.
			///   3. Keep the per-member config in sync so a subsequent
			///      SendRtp / FindMemberBySsrc sees the latest SSRCs.
			///
			/// Default implementation: no-op for backends that do not key
			/// routing on SSRC (e.g. a future WebRTC backend keyed on the
			/// PeerConnection ID). The native UDP backend MUST override this.
			///
			/// Idempotent: if `memberId` is unknown the call is a no-op.
			/// Passing all-zero SSRCs is treated as "do not change that slot"
			/// (the legacy RTPEngine behaviour was to skip zero values in
			/// UpdateMemberSsrc, and we preserve that semantic here).
			virtual void UpdateMemberSsrc(
				const std::string& memberId,
				uint32_t audioSsrc,
				uint32_t videoSsrc,
				uint32_t h239Ssrc) noexcept {
				(void)memberId; (void)audioSsrc; (void)videoSsrc; (void)h239Ssrc;
			}

			/// [V7.0] Attach the gateway-owned per-kind transports to the member.
			/// One transport per MediaStreamKind (Audio / Video / SecondaryVideo /
			/// Data). The backend uses the matching transport on egress based on
			/// the `kind` argument passed to SendRtp / SendH264Nalu.
			///
			/// Must be called after AddMember() and before any Send* call. Default
			/// implementation is a no-op for backends (e.g. WebRTC) that own
			/// their own transport internally.
			///
			/// The transports are `std::shared_ptr` to allow the gateway to share
			/// them with other components (e.g. RTCP feedback path). The backend
			/// is NOT required to keep the shared_ptrs alive past the call; the
			/// gateway retains ownership.
			///
			/// The map MAY be empty (called before SDP is fully negotiated). The
			/// backend should tolerate that and not crash.
			virtual void SetTransports(
				const std::string& memberId,
				const MemberMediaTransports& transports) noexcept = 0;

			// ---------------------- Media send ----------------------

			/// [V7.0] Send a fully-built RTP packet (header fields filled by the caller).
			/// `kind` tells the backend which per-kind transport to use. Audio packets
			/// go through the Audio transport, video through the Video transport, etc.
			/// Backends MUST return vcs::ErrorCode::MediaTransportNotReady
			/// if the kind has no transport registered.
			virtual vcs::ErrorCode SendRtp(
				const std::string& memberId,
				vcs::MediaStreamKind kind,
				uint8_t payloadType,
				const uint8_t* data,
				size_t len,
				uint32_t ssrc,
				uint64_t timestamp,
				uint16_t sequenceNumber) = 0;

			/// Send an H.264 NAL unit. The backend is responsible for the
			/// single-NAL vs FU-A fragmentation decision (RFC 6184) and for
			/// building the per-fragment 12-byte RTP header. The orchestrator
			/// just hands over a whole NALU; the backend decides how to ship it.
			///
			/// This is the video counterpart of SendRtp(): audio packets are
			/// small and uniform, so SendRtp() is a single datagram; video
			/// frames can be hundreds of KB, so we need a dedicated entry
			/// point that lets the backend apply its fragmentation policy.
			///
			/// [V7.0] `kind` selects the per-kind transport (typically
			/// MediaStreamKind::Video or MediaStreamKind::SecondaryVideo).
			///
			/// Default implementation: returns kNotImplemented. Backends that
			/// support H.264 (e.g. NativeRtp) override this; backends that do
			/// not (e.g. an audio-only or signaling-only backend) leave the
			/// default and the orchestrator will surface a kNotImplemented
			/// error to the upper layer.
			virtual vcs::ErrorCode SendH264Nalu(
				const std::string& memberId,
				vcs::MediaStreamKind kind,
				uint8_t payloadType,
				const uint8_t* naluData,
				size_t naluLen,
				uint32_t ssrc,
				uint64_t timestamp,
				uint16_t sequenceNumber,
				uint16_t* outLastSeq = nullptr) {
				(void)memberId; (void)kind; (void)payloadType; (void)naluData; (void)naluLen;
				(void)ssrc; (void)timestamp; (void)sequenceNumber; (void)outLastSeq;
				return vcs::ErrorCode::NotImplemented;
			}

			/// [V7.0] Send a raw RTCP compound packet on the transport that
			/// matches `kind` (typically the per-stream RTCP socket).
			virtual vcs::ErrorCode SendRtcp(
				const std::string& memberId,
				vcs::MediaStreamKind kind,
				const uint8_t* data,
				size_t len) = 0;

			/// [Q5 step 5h] Build an RFC 3550 Section 6.1 RTCP compound packet
			/// (SR or RR + SDES CNAME). The orchestrator (RTPEngine) calls
			/// this once per reporting interval to obtain a byte buffer,
			/// then hands the buffer to SendRtcp() to ship it. Splitting
			/// "build" and "send" lets the orchestrator add BYE or APP
			/// packets, set the IPv4/IPv6 DSCP, or throttle the send.
			///
			/// The default implementation returns an empty vector (meaning
			/// "this backend does not generate RTCP reports"). Backends
			/// that do generate reports override this  -  the native UDP
			/// backend uses NativeRtp::Rtcp::BuildCompoundReport to satisfy
			/// the contract.
			///
			/// Thread safety: must be safe to call concurrently with
			/// SendRtp / SendRtcp. The implementation is expected to be
			/// pure (no shared mutable state).
			///
			/// The compound shape:
			///   [SR or RR header + body] [SDES CNAME chunk]
			/// is RFC 3550 Section 6.1-compliant and ready to ship over a single
			/// UDP datagram. The SDES CNAME chunk is mandatory per Section 6.1;
			/// if `req.cname` is empty the function returns an empty
			/// vector.
			struct RtcpReportBlock {
				uint32_t ssrc = 0;   // SSRC of the source
				uint8_t  fractionLost = 0;   // 0..255
				uint32_t cumulativeLost = 0;   // 24-bit on the wire
				uint32_t extendedHighestSeq = 0;   // high 16 = cycles
				uint32_t jitter = 0;   // RTP timestamp units
				uint32_t lastSrNtp = 0;   // middle 32 bits
				uint32_t delaySinceLastSr = 0;   // 1/65536 s units
			};

			struct RtcpReportRequest {
				// ---- SR (or RR) header ----
				// If generateSenderReport is true the backend builds an
				// SR (PT=200); otherwise an RR (PT=201).
				bool        generateSenderReport = true;
				uint32_t    ssrcSender = 0;
				// 64-bit NTP timestamp (RFC 3550 Section 4). 0 = use current
				// wall clock (helpers like UnixSecondsToNtp() in the
				// backend's anonymous namespace make this trivial).
				uint64_t    ntpTimestamp = 0;
				uint32_t    rtpTimestamp = 0;
				uint32_t    packetCount = 0;
				uint32_t    octetCount = 0;
				// 0..31 report blocks describing *other* sources this
				// endpoint is listening to. The native UDP backend
				// typically has 0 or 1 entries (audio + video SSRCs).
				std::vector<RtcpReportBlock> reportBlocks;
				// ---- SDES (CNAME) ----
				// Mandatory per RFC 3550 Section 6.1. The orchestrator is
				// expected to use a stable, per-member identifier (e.g.
				// `<user>@<conference>`) so receivers can correlate
				// reports across SSRC changes.
				std::string cname;
			};

			/// Build a compound RTCP report. Returns the on-the-wire bytes
			/// (network byte order), or an empty vector if the request is
			/// invalid (no SSRC, empty CNAME, or backend does not
			/// implement report generation).
			virtual std::vector<uint8_t> BuildRtcpReport(
				const std::string& memberId,
				const RtcpReportRequest& req) {
				(void)memberId; (void)req;
				return {};
			}

			// ---------------------- Event subscription ----------------------

			/// RTP receive callback. The backend fires this on its receive
			/// thread; the caller is responsible for thread safety.
			virtual void SetRtpReceiveCallback(RtpReceiveCallback cb) = 0;

			/// RTCP receive callback. Raw bytes, not yet parsed.
			virtual void SetRtcpReceiveCallback(RtcpReceiveCallback cb) = 0;

			// ---------------------- External ingress (Q5 step 5d-receive) ----
			//
			// For backends that do NOT own their own receive socket (e.g. the
			// gateway layer's existing `IRtpTransport` is the actual socket
			// owner), the orchestrator (RTPEngine) needs a way to push a
			// freshly received datagram into the backend so it can parse the
			// header and fire the RtpReceiveCallback. This is the receive-side
			// counterpart of SendRtp / SendRtcp.
			//
			// Backends that DO own their own socket (e.g. a future WebRTC
			// backend that reads from an ICE-selected socket) inherit the
			// default no-op and fire RtpReceiveCallback directly from their
			// internal I/O loop, without going through this entry point.
			//
			// [Thread safety] May be called from any thread. The implementation
			// is expected to take a short lock to snapshot the registered
			// callbacks and then return  -  the callbacks themselves run on the
			// caller's thread (this is the "synchronous forward" model used by
			// the existing gateway transport callbacks).
			virtual void OnRtpReceived(const std::string& memberId,
				const uint8_t* data, size_t len) {
				(void)memberId; (void)data; (void)len;
			}

			/// External RTCP ingress. Same contract as OnRtpReceived but for
			/// RTCP compound packets. Default: no-op.
			virtual void OnRtcpReceived(const std::string& memberId,
				const uint8_t* data, size_t len) {
				(void)memberId; (void)data; (void)len;
			}

			/// [Q5 step 5e] Pre-parsed ingress. The caller (e.g. MediaWorker)
			/// has already parsed the RTP header and hands the orchestrator-side
			/// fields directly to the backend. The backend's job is to fire
			/// RtpReceiveCallback without re-parsing. This is the receive-side
			/// counterpart of the parsed `SendRtp` shape.
			///
			/// Default: no-op for backends that do not support the fast path
			/// (e.g. a future WebRTC backend whose own socket is the only ingress
			///  -  it never sees pre-parsed packets because the parsing happens
			/// inside the backend itself). Backends whose ingress is external
			/// (e.g. NativeRtp, which consumes the gateway's pre-parsed
			/// MediaPackets via MediaWorker) override this to fire the
			/// callback directly.
			virtual void OnRtpReceivedParsed(
				const std::string& memberId, uint8_t payloadType,
				const uint8_t* data, size_t len,
				uint32_t ssrc, uint64_t timestamp,
				uint16_t sequenceNumber, bool markerBit) = 0;

			/// Periodic statistics callback.
			virtual void SetStatisticsCallback(BackendStatisticsCallback cb) = 0;

			// ---------------------- WebRTC media-engine integration --------
			//
			// When a backend owns the full libwebrtc media engine (pacer,
			// NACK/ARQ, jitter buffer, PLI, AEC), it delivers already
			// depacketized / jitter-buffered encoded video frames through this
			// callback instead of the generic RtpReceiveCallback path, and it
			// accepts raw PCM for the audio send stream.
			//
			// Backends without a media engine inherit the defaults below, so
			// the orchestrator can query the capability and keep the legacy
			// path unchanged.

			/// Encoded video frame callback (webrtc media-engine mode only).
			using EncodedVideoFrameCallback = std::function<void(
				const std::string& memberId,
				const uint8_t* data, size_t len,
				bool isKeyFrame, uint32_t timestamp, uint32_t ssrc,
				bool isSecondary)>;

			virtual void SetEncodedVideoFrameCallback(EncodedVideoFrameCallback cb) {
				(void)cb;
			}

			/// PCM playout callback (webrtc media-engine mode only).
			using AudioPlayoutCallback = std::function<void(
				const std::string& memberId,
				const int16_t* pcm, size_t samplesPerChannel,
				int sampleRate, int channels)>;

			virtual void SetAudioPlayoutCallback(AudioPlayoutCallback cb) {
				(void)cb;
			}

			/// Push captured PCM into the webrtc audio send path.
			/// Returns false when the backend has no media engine.
			virtual bool PushCapturedAudio(
				const std::string& memberId,
				const int16_t* pcm, size_t samplesPerChannel,
				int sampleRate, int channels) {
				(void)memberId; (void)pcm; (void)samplesPerChannel;
				(void)sampleRate; (void)channels;
				return false;
			}

			/// True when this backend drives the full libwebrtc media engine
			/// (video: pacer/NACK/jitter/PLI, audio: NetEq/AEC). RTPEngine uses
			/// this to bypass its own depacketization, FIR and SR paths.
			virtual bool UsesWebRtcMediaEngine() const {
				return false;
			}

			/// Local (outbound) SSRCs allocated by RTPEngine. Backends that
			/// generate their own RTP (webrtc media engine) need them before
			/// the first Send*/PushCapturedAudio call. Default: no-op.
			virtual void SetMemberLocalSsrc(
				const std::string& memberId,
				uint32_t audioSsrc, uint32_t videoSsrc) {
				(void)memberId; (void)audioSsrc; (void)videoSsrc;
			}

			/// Negotiated payload types for an existing member. Default: no-op.
			virtual void UpdateMemberPayloadTypes(
				const std::string& memberId,
				uint8_t audioPayloadType, uint8_t videoPayloadType) {
				(void)memberId; (void)audioPayloadType; (void)videoPayloadType;
			}

			/// Protocol-agnostic network lifecycle event.
			virtual void SetNetworkEventCallback(NetworkEventCallback cb) = 0;

			// ---------------------- Runtime control ----------------------

			/// Set per-member send bandwidth (bps). 0 = unlimited.
			virtual vcs::ErrorCode SetSendBandwidth(
				const std::string& memberId, uint32_t bps) = 0;

			/// Toggle Forward Error Correction for `memberId`.
			virtual vcs::ErrorCode SetFecEnabled(
				const std::string& memberId, bool enable) = 0;

			// ---------------------- Diagnostics ----------------------

			/// Short, stable identifier of the concrete backend type
			/// (e.g. "NativeRtp", "WebRtc", "Srt"). Used in logs, metrics
			/// and crash dumps.
			virtual std::string GetBackendType() const = 0;

			// ---------------------- H.264 depacketization (Q5 step 5f-receive) ----
			//
			// Backends that own an H.264 RTP receive path (e.g. the native UDP
			// backend, which still has to drive a video frame queue from the
			// gateway transport's pre-parsed packets) override these three methods
			// to maintain per-member FU-A reassembly state.
			//
			// Backends that DO NOT process H.264 (e.g. an audio-only or
			// signaling-only backend) inherit the default no-op implementations,
			// which always return an incomplete / empty result. RTPEngine is
			// expected to call DepacketizeH264() only when it knows the SSRC is
			// carrying H.264 video  -  for non-video packets it can simply skip
			// the depacketization step entirely.

			/// Result of one FU-A / STAP / MTAP depacketization step. Empty
			/// (complete=false) when the depacketizer is still waiting for more
			/// fragments; callers should only push the NAL unit downstream when
			/// complete=true.
			struct H264DepacketizeResult {
				std::vector<uint8_t> nalu;   // Annex-B NAL unit (empty if !complete)
				bool isKeyFrame = false;
				bool complete = false;
			};

			/// Feed one RTP payload into the per-member depacketizer. Returns the
			/// reassembled NAL unit (if any) plus a key-frame hint. Default:
			/// returns {complete=false} for backends that don't support H.264.
			virtual H264DepacketizeResult DepacketizeH264(
				const std::string& memberId,
				const uint8_t* payload, size_t payloadSize,
				uint16_t sequenceNumber, uint64_t timestamp, bool markerBit) = 0;

			/// Drop depacketizer state for a single member. Called by RTPEngine
			/// from RemoveMember(). Default: no-op.
			virtual void ClearH264Depacketizer(const std::string& memberId) {
				(void)memberId;
			}

			/// Drop depacketizer state for all members. Called by RTPEngine from
			/// ClearAllMembers(). Default: no-op.
			virtual void ClearAllH264Depacketizers() {}

			// ---------------------- SSRC routing (Q5 step 5c) -----------------
			//
			// Look up the memberId that owns a given SSRC. Backends that key
			// routing on something other than SSRC (e.g. WebRTC keyed on the
			// PeerConnection ID) inherit the default implementation, which
			// always returns "not found". Backends whose transport is keyed on
			// SSRC (the native UDP backend) override this to consult their
			// internal SSRC->memberId index.
			//
			// Thread safety: must be safe to call concurrently with any other
			// IRtpBackend method.
			virtual bool FindMemberBySsrc(uint32_t ssrc, std::string& outMemberId) const {
				(void)ssrc; (void)outMemberId;
				return false;
			}
		};

	} // namespace media
} // namespace vcs
