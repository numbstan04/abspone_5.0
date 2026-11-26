/**
 * @file NativeRtpBackend.h
 * @brief Default RTP backend implementation (Q5, native UDP)
 *
 * [Role]
 * This is the canonical "boring" backend: plain RFC 3550 RTP/RTCP over
 * UDP. It receives the legacy 1544-line logic that used to live inside
 * RTPEngine.cpp and exposes it through the IRtpBackend interface.
 *
 * [Internal organization]
 * The .cpp file uses four anonymous namespaces to physically isolate
 * the four concerns, so each can be reasoned about (and unit-tested)
 * independently:
 *     namespace NativeRtp::Ingress   -  receive path (RTP, RTCP, demux)
 *     namespace NativeRtp::Egress    -  send path (RTP, RTCP, bandwidth)
 *     namespace NativeRtp::Ssrc      -  SSRC allocation / lookup
 *     namespace NativeRtp::Rtcp      -  RTCP compound packet builder
 *
 * These are NOT separate classes  -  they are translation-unit-local
 * helpers. The single public class is `NativeRtpBackend`.
 *
 * [Thread model]
 * - One io thread (constructed internally) that does blocking recvfrom
 *   and demultiplexes RTP vs RTCP.
 * - All IRtpBackend methods are thread-safe and may be called from any
 *   thread (orchestrator, MediaWorker, RTCP timer, etc.).
 *
 * [References]
 * - Section 20.7 (Q5 NativeRtpBackend layout)
 * - Section 20.4 (Q5 overall architecture)
 */

#pragma once

 // The transport contract (IRtpTransport / RtpPacket / SessionContext) and
 // MediaStreamKind now live in this module, so they are plain includes.
#include "IRtpTransport.h"

#include "IRtpBackend.h"
#include "media_export.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vcs {
	namespace media {

		/**
		 * @brief Native UDP-based IRtpBackend implementation.
		 *
		 * Created by `BackendFactory::CreateDefaultBackend()` and injected into
		 * RTPEngine at construction time. `BackendFactory.cpp` is the only
		 * supported construction site; direct `new NativeRtpBackend()`
		 * anywhere else is a contract violation (docs/12 §15 P0-5).
		 */
		class VCS_MEDIA_API NativeRtpBackend : public IRtpBackend {
		public:
			NativeRtpBackend();
			~NativeRtpBackend() override;

			// ---- IRtpBackend: lifecycle ---------------------------------------

			vcs::ErrorCode Initialize(const BackendMediaConfig& config) override;
			// [Yealink no-video fix] See IRtpBackend.h for the contract.
			// Separated from Initialize() so the default-deny posture (i.e.
			// the `running_` flag stays false) is preserved through
			// construction + configuration, and is only flipped when the
			// orchestrator (RTPEngine) is ready to accept traffic.
			void Start() override;
			void Shutdown() noexcept override;

			// ---- IRtpBackend: member management -------------------------------

			vcs::ErrorCode AddMember(
				const std::string& memberId,
				const BackendMemberConfig& config) override;

			void RemoveMember(const std::string& memberId) noexcept override;

			// [Q5 step 5g] See IRtpBackend.h for the contract. Updates the
			// SSRC routing index after SDP renegotiation. Audio/video/h239
			// SSRCs are independently replaceable; zero is treated as "skip".
			void UpdateMemberSsrc(
				const std::string& memberId,
				uint32_t audioSsrc,
				uint32_t videoSsrc,
				uint32_t h239Ssrc) noexcept override;

			// [V7.0] Attach per-kind gateway transports to the member. The
			// backend stores the entire map and uses the transport matching
			// `kind` on egress. Replacing this implementation removed the
			// legacy single-transport model (where a single transport carried
			// audio + video + RTCP and routing was done by payload-type lookup,
			// which is what caused the audio/video cross-port bug).
			void SetTransports(
				const std::string& memberId,
				const MemberMediaTransports& transports) noexcept override;

			// ---- IRtpBackend: media send --------------------------------------

			// [V7.0] `kind` selects the per-kind transport for egress. The
			// backend rejects the call with vcs::ErrorCode::MediaTransportNotReady
			// if no transport is registered for the requested kind. This is the
			// single piece of information that distinguishes audio traffic
			// from video traffic on the wire, and is the fix for the legacy
			// "audio RTP shipped to video port" bug.
			vcs::ErrorCode SendRtp(
				const std::string& memberId,
				::vcs::MediaStreamKind kind,
				uint8_t payloadType,
				const uint8_t* data,
				size_t len,
				uint32_t ssrc,
				uint64_t timestamp,
				uint16_t sequenceNumber) override;

			// [Q5 step 5d-video] H.264 NALU entry point. Performs single-NAL vs
			// FU-A fragmentation (RFC 6184) and ships each fragment via the
			// per-kind gateway transport. Migrated from
			// RTPEngine::SendH264WithFragmentation  -  see
			// NativeRtp::Egress::FragmentAndSendH264 in NativeRtpBackend.cpp for
			// the actual fragmentation logic.
			vcs::ErrorCode SendH264Nalu(
				const std::string& memberId,
				::vcs::MediaStreamKind kind,
				uint8_t payloadType,
				const uint8_t* naluData,
				size_t naluLen,
				uint32_t ssrc,
				uint64_t timestamp,
				uint16_t sequenceNumber,
				uint16_t* outLastSeq = nullptr) override;

			// [V7.0] `kind` selects the per-kind transport for RTCP egress
			// (typically the per-stream RTCP socket). Most stacks colocate
			// RTP and RTCP on the same transport, but the API mirrors
			// SendRtp so backends that use distinct sockets can route
			// correctly.
			vcs::ErrorCode SendRtcp(
				const std::string& memberId,
				::vcs::MediaStreamKind kind,
				const uint8_t* data,
				size_t len) override;

			// [Q5 step 5h] See IRtpBackend::BuildRtcpReport for the contract.
			// The native backend delegates to NativeRtp::Rtcp::BuildCompoundReport
			// (defined in the .cpp), which is a pure function and therefore
			// safe to call concurrently with any other API. The `memberId`
			// argument is currently unused (the report is built from `req`
			// alone) but is kept in the signature so a future variant that
			// needs per-member state (e.g. cached CNAME) can be added without
			// changing the interface.
			std::vector<uint8_t> BuildRtcpReport(
				const std::string& memberId,
				const RtcpReportRequest& req) override;

			// ---- IRtpBackend: event subscription ------------------------------

			void SetRtpReceiveCallback(RtpReceiveCallback cb) override;
			void SetRtcpReceiveCallback(RtcpReceiveCallback cb) override;
			void SetStatisticsCallback(BackendStatisticsCallback cb) override;
			void SetNetworkEventCallback(NetworkEventCallback cb) override;

			// ---- IRtpBackend: external ingress (Q5 step 5d-receive) -----------
			//
			// The native UDP backend does NOT own its own receive socket  -  the
			// gateway transport is the actual UDP socket owner (see
			// IRtpTransport in gateway/include/IRtpTransport.h). The
			// orchestrator (RTPEngine) therefore routes the raw bytes the
			// gateway thread just received through these two entry points. The
			// backend parses the RTP header (or detects RTCP) and then fires
			// the registered RtpReceiveCallback / RtcpReceiveCallback.
			//
			// This is the receive-side counterpart of SendRtp / SendRtcp.
			void OnRtpReceived(const std::string& memberId,
				const uint8_t* data, size_t len) override;
			void OnRtcpReceived(const std::string& memberId,
				const uint8_t* data, size_t len) override;

			// ---- IRtpBackend: pre-parsed ingress (Q5 step 5e) ------------------
			//
			// The "fast path" for callers that have already parsed the RTP
			// header (e.g. MediaWorker that drained a MediaPacket from the
			// SPSC queue). Skips the re-serialize / re-parse cycle and fires
			// the RtpReceiveCallback directly with the supplied fields.
			void OnRtpReceivedParsed(
				const std::string& memberId,
				uint8_t payloadType,
				const uint8_t* data, size_t len,
				uint32_t ssrc,
				uint64_t timestamp,
				uint16_t sequenceNumber,
				bool markerBit) override;

			// ---- IRtpBackend: runtime control ---------------------------------

			vcs::ErrorCode SetSendBandwidth(
				const std::string& memberId, uint32_t bps) override;

			vcs::ErrorCode SetFecEnabled(
				const std::string& memberId, bool enable) override;

			// ---- IRtpBackend: diagnostics -------------------------------------

			std::string GetBackendType() const override { return "NativeRtp"; }

			// ---- Non-interface helpers used by RTPEngine during migration ----
			//
			// These are public to keep the migration mechanical: RTPEngine can
			// call them directly during the transition, while still funnelling
			// traffic through IRtpBackend. They will be made private once
			// RTPEngine is fully refactored (see Section 20.6 for the end state).

			/// Returns the gateway transport for `(memberId, kind)`, or nullptr
			/// if the member is unknown or the requested kind has not been
			/// registered. Mirrors the old single-transport GetTransport() but
			/// with an explicit kind selector.
			std::shared_ptr< ::vcs::IRtpTransport> GetTransport(
				const std::string& memberId,
				::vcs::MediaStreamKind kind);

			// ---- IRtpBackend: SSRC routing (Q5 step 5c) ------------------------
			//
			// Override of IRtpBackend::FindMemberBySsrc. Returns true and writes
			// the memberId on hit. The implementation queries the
			// `ssrcToMember_` index, which is kept in sync with AddMember /
			// RemoveMember.
			bool FindMemberBySsrc(uint32_t ssrc, std::string& outMemberId) const override;

			// ---- H.264 FU-A depacketization (Q5 step 5f-receive) --------------
			//
			// Migrated from RTPEngine::H264FuADepacketizer. The depacketizer
			// reassembles FU-A fragments (RFC 6184) into complete NAL units in
			// Annex-B format. STAP-A/B, MTAP16/24, and FU-B are also handled
			// here  -  the depacketizer is the RFC 6184 ground truth.
			//
			// The H264DepacketizeResult type lives in IRtpBackend (the interface
			// owner). NativeRtpBackend reuses it as-is so the override has the
			// same return type as the virtual declaration (no covariant return
			// gymnastics) and callers can refer to either name.
			using H264DepacketizeResult = IRtpBackend::H264DepacketizeResult;

			H264DepacketizeResult DepacketizeH264(
				const std::string& memberId,
				const uint8_t* payload, size_t payloadSize,
				uint16_t sequenceNumber, uint64_t timestamp,
				bool markerBit) override;

			/// Drop the depacketizer state for one member. Called by RTPEngine
			/// on RemoveMember.
			void ClearH264Depacketizer(const std::string& memberId) override;

			/// Drop depacketizer state for all members. Called by RTPEngine on
			/// ClearAllMembers.
			void ClearAllH264Depacketizers() override;

		private:
			// -----------------------------------------------------------------
			// Internals
			// -----------------------------------------------------------------

			struct MemberEntry;

			// The four concern namespaces from the .cpp are friend-like helpers;
			// we keep them out of the class so the .cpp can stay organized into
			// four physical sections. The public class owns the shared state
			// (members_, callbacks_, running_) below.

			/// Per-member state. Owned by NativeRtpBackend::members_.
			//
			// [V7.0] The single `transport` shared_ptr was replaced with the
			// `MemberMediaTransports` map (one transport per MediaStreamKind).
			// The backend's egress path (SendRtp / SendH264Nalu / SendRtcp) now
			// uses the `kind` argument to look up the right transport, which is
			// the structural fix for the audio/video cross-port bug.
			struct MemberEntry {
				BackendMemberConfig config;
				MemberMediaTransports transports;
				bool registered = false;
			};

			// ---- internal helpers (defined in .cpp) ---------------------------
			bool ParseRtpHeader(const uint8_t* data, size_t len,
				uint8_t& outPt, uint32_t& outSsrc,
				uint64_t& outTimestamp, uint16_t& outSeq,
				bool& outMarkerBit) const;
			bool IsRtcp(const uint8_t* data, size_t len) const;

			// ---- state --------------------------------------------------------

			mutable std::mutex members_mu_;
			std::unordered_map<std::string, std::unique_ptr<MemberEntry>> members_;

			// [Q5 step 5f-receive] H.264 FU-A depacketizer state. The actual
			// FragmentBuffer type lives inside the anonymous namespace in the
			// .cpp (NativeRtp::Ingress::H264::FragmentBuffer) so it is not
			// visible to header consumers. We hold it via a pimpl here so the
			// header doesn't need to know the depacketizer's internal layout.
			struct H264State;
			std::unique_ptr<H264State> h264_state_;

			// [Q5 step 5c] SSRC -> memberId index. Populated from the SSRCs
			// handed in at AddMember() time; cleared on RemoveMember(). The
			// orchestrator (RTPEngine) and the data-plane ingress path both
			// query this when they need to identify the source of a packet
			// whose SSRC is the only available key (e.g. RTCP receiver reports
			// keyed by the sender's SSRC). Mirrors the `ssrcToMember` map that
			// currently lives in RTPEngine::Impl; once the data plane is
			// forwarded (Q5 step 5d) the orchestrator's map is the one that
			// gets removed.
			std::unordered_map<uint32_t, std::string> ssrcToMember_;

			std::atomic<bool> running_{ false };
			std::thread io_thread_;

			// Callbacks
			std::mutex cb_mu_;
			RtpReceiveCallback     on_rtp_;
			RtcpReceiveCallback    on_rtcp_;
			BackendStatisticsCallback on_stats_;
			NetworkEventCallback   on_net_event_;

			// Backend-wide config (set at Initialize())
			BackendMediaConfig cfg_;

			// Statistics emission cadence. Native backend owns its own timer.
			std::thread stats_thread_;
			void StatsLoop(std::atomic<bool>& running);
		};

	} // namespace media
} // namespace vcs
