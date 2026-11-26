/**
 * @file WebRtcRtpBackend.h
 * @brief WebRTC RTP backend: full libwebrtc media engine (VCS_WITH_WEBRTC only)
 *
 * [Role]
 * Concrete IRtpBackend for BackendType::kWebRtc. The transport belongs to the
 * gateway (IRtpTransport - ICE/DTLS/SRTP), while all media-engine behaviour
 * comes from libwebrtc through the ABI-neutral IWebRtcMediaEngine layer:
 *
 *   video tx: 5.0 H.264 encoder -> SendEncodedFrame -> VideoSendStream
 *             (pacer, NACK store, RTX, SR/RR) -> IRtpTransport::SendRtp
 *   video rx: IRtpTransport -> OnRtpPacket -> VideoReceiveStream
 *             (jitter buffer, NACK, PLI) -> pass-through decoder
 *             -> encoded-frame sink -> RTPEngine::PushEncodedVideoFrame
 *   audio tx: 5.0 capture PCM -> PushCapturedAudio -> custom ADM
 *             -> AudioProcessing (AEC3/NS/AGC) -> AudioSendStream
 *   audio rx: IRtpTransport -> AudioReceiveStream (NetEq) -> ADM playout
 *             -> PCM playout sink -> 5.0 IAudioSink
 *
 * This mirrors the 4.5 absphone flow (OnEncodedImage on the way out,
 * FakeGenericDecoder::Decode on the way in) while keeping the on-wire
 * transport and the H.264 codecs in 5.0.
 *
 * [Fallback]
 * When the build has no libwebrtc adapter (VCS_WEBRTC_BUILD_LIBWEBRTC=OFF)
 * CreateWebRtcMediaEngine() returns nullptr: every override falls back to
 * NativeRtpBackend / the RTP-codec-only path, so the backend always works.
 *
 * [Build gate]
 * Only compiled into the standard artifact (VCS_WITH_WEBRTC=ON).
 */

#pragma once

#ifdef VCS_WITH_WEBRTC

#include "NativeRtpBackend.h"
#include "media_export.h"
#include "webrtc/IWebRtcMediaEngine.h"
#include "webrtc/IWebRtcRtpCodec.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vcs {
	namespace media {

		/**
		 * @brief WebRTC-flavoured IRtpBackend.
		 *
		 * Inherits the complete NativeRtpBackend implementation (member
		 * bookkeeping, transports, RTCP reporting) and replaces the media
		 * data path with the libwebrtc media engine when available.
		 */
		class VCS_MEDIA_API WebRtcRtpBackend : public NativeRtpBackend,
			public webrtc_engine::IWebRtcPacketSink,
			public webrtc_engine::IWebRtcEncodedFrameSink,
			public webrtc_engine::IWebRtcAudioSink {
		public:
			WebRtcRtpBackend();
			~WebRtcRtpBackend() override;

			// ---- IRtpBackend: member management -------------------------------

			vcs::ErrorCode AddMember(
				const std::string& memberId,
				const BackendMemberConfig& config) override;
			void RemoveMember(const std::string& memberId) noexcept override;
			void UpdateMemberSsrc(
				const std::string& memberId,
				uint32_t audioSsrc,
				uint32_t videoSsrc,
				uint32_t h239Ssrc) noexcept override;
			void SetMemberLocalSsrc(
				const std::string& memberId,
				uint32_t audioSsrc, uint32_t videoSsrc) override;
			void UpdateMemberPayloadTypes(
				const std::string& memberId,
				uint8_t audioPayloadType, uint8_t videoPayloadType) override;

			// ---- IRtpBackend: media send ---------------------------------------

			/// H.264 egress. When the media engine is available the Annex-B
			/// access unit is handed to VideoSendStream (packetization, pacing,
			/// NACK/RTX and RTCP are then owned by libwebrtc). `outLastSeq` keeps
			/// the legacy contract: with the media engine the real RTP sequence
			/// numbers are generated inside libwebrtc, so the caller-provided
			/// sequence number is returned unchanged.
			vcs::ErrorCode SendH264Nalu(
				const std::string& memberId,
				vcs::MediaStreamKind kind,
				uint8_t payloadType,
				const uint8_t* naluData,
				size_t naluLen,
				uint32_t ssrc,
				uint64_t timestamp,
				uint16_t sequenceNumber,
				uint16_t* outLastSeq = nullptr) override;

			// ---- IRtpBackend: H.264 ingress ------------------------------------

			/// Legacy depacketizer, still used when the media engine is absent.
			H264DepacketizeResult DepacketizeH264(
				const std::string& memberId,
				const uint8_t* payload, size_t payloadSize,
				uint16_t sequenceNumber, uint64_t timestamp, bool markerBit) override;

			// ---- IRtpBackend: external ingress ---------------------------------

			void OnRtpReceived(const std::string& memberId,
				const uint8_t* data, size_t len) override;
			void OnRtpReceivedParsed(
				const std::string& memberId, uint8_t payloadType,
				const uint8_t* data, size_t len,
				uint32_t ssrc, uint64_t timestamp,
				uint16_t sequenceNumber, bool markerBit) override;
			void OnRtcpReceived(const std::string& memberId,
				const uint8_t* data, size_t len) override;

			// ---- IRtpBackend: runtime control ----------------------------------

			vcs::ErrorCode SetSendBandwidth(
				const std::string& memberId, uint32_t bps) override;

			// ---- IRtpBackend: media-engine integration -------------------------

			void SetEncodedVideoFrameCallback(
				EncodedVideoFrameCallback cb) override;
			void SetAudioPlayoutCallback(
				AudioPlayoutCallback cb) override;
			bool PushCapturedAudio(
				const std::string& memberId,
				const int16_t* pcm, size_t samplesPerChannel,
				int sampleRate, int channels) override;
			bool UsesWebRtcMediaEngine() const override;

			// ---- IRtpBackend: diagnostics -------------------------------------

			/// Returns "WebRtc". Must match BackendType::kWebRtc's name in
			/// BackendFactory so that Create(GetBackendType()) round-trips.
			std::string GetBackendType() const override;

			// ---- webrtc_engine sinks ------------------------------------------

			bool OnSendRtp(const char* memberId, int streamKind,
				const uint8_t* data, size_t size) override;
			bool OnSendRtcp(const char* memberId, int streamKind,
				const uint8_t* data, size_t size) override;
			void OnEncodedFrame(const char* memberId, int streamKind,
				const uint8_t* data, size_t size, bool isKeyFrame,
				uint32_t rtpTimestamp, uint32_t ssrc) override;
			void OnPlayoutAudio(const char* memberId,
				const int16_t* pcm, size_t samplesPerChannel,
				int sampleRate, int channels) override;

		private:
			/// Per-member libwebrtc codec (legacy RTP-only fallback path).
			struct MemberCodec;
			/// Per-member routing hints used to split video / audio ingress.
			struct MemberRoute;

			std::shared_ptr<MemberCodec> GetOrCreateCodec(const std::string& memberId);
			MemberRoute GetRoute(const std::string& memberId);

			/// True when the RTP payload type looks like a video payload.
			bool IsVideoPayload(const MemberRoute& route, uint8_t payloadType) const;

			/// Feeds one raw RTP datagram into the media engine (if active) and
			/// decides whether the legacy path must also see it.
			void RouteRtpToEngine(const std::string& memberId,
				const uint8_t* data, size_t len);

			std::unique_ptr<webrtc_engine::IWebRtcMediaEngine> engine_;

			std::mutex codec_mu_;
			std::unordered_map<std::string, std::shared_ptr<MemberCodec>> codecs_;

			std::mutex route_mu_;
			std::unordered_map<std::string, MemberRoute> routes_;

			std::mutex sink_mu_;
			EncodedVideoFrameCallback encodedFrameCallback_;
			AudioPlayoutCallback audioPlayoutCallback_;
		};

	} // namespace media
} // namespace vcs

#endif // VCS_WITH_WEBRTC
