/**
 * @file IWebRtcMediaEngine.h
 * @brief Version-neutral WebRTC media engine abstraction (middle layer).
 *
 * [Why this file exists]
 * The RTP-codec-only path (IWebRtcRtpCodec) reuses libwebrtc's H.264
 * packetizer / depacketizer but gets none of the media-engine features that
 * SIP terminals actually need in the field:
 *
 *   - smooth sending        : RtpTransportControllerSend / pacer
 *   - NACK / ARQ retransmit : VideoSendStream RTX + VideoReceiveStream NACK
 *   - jitter buffer         : VideoReceiveStream / NetEq
 *   - PLI / SR / RR         : per-stream RTCP modules
 *   - AEC / NS / AGC        : AudioProcessing (AEC3) + AudioState
 *
 * This header exposes the media engine through one *ABI-neutral* interface so
 * the 5.0 pipeline (MSVC STL) can drive libwebrtc (Chromium clang-cl +
 * libc++). Only scalars, plain structs, raw pointers and sink callbacks are
 * allowed in the virtual signatures; nothing from std:: crosses the boundary.
 *
 * [Ownership model]
 *   - One engine per RTP backend (created lazily by CreateWebRtcMediaEngine).
 *   - One webrtc::Call per member, so pacer / BWE / NACK state is isolated.
 *   - All Call objects share one webrtc network/worker thread owned by the
 *     engine; every Call API is executed on that thread.
 *
 * [Data flow]
 *   video tx: 5.0 encoder -> SendEncodedFrame -> VideoSendStream(pacer/NACK/RTX)
 *             -> IWebRtcPacketSink::OnSendRtp -> gateway IRtpTransport
 *   video rx: gateway IRtpTransport -> OnRtpPacket -> VideoReceiveStream
 *             (jitter/NACK/PLI) -> pass-through decoder
 *             -> IWebRtcEncodedFrameSink -> RTPEngine::PushEncodedVideoFrame
 *   audio tx: 5.0 IAudioSource -> PushCapturedAudio -> custom ADM
 *             -> AudioProcessing(AEC3) -> AudioSendStream -> transport
 *   audio rx: transport -> AudioReceiveStream(NetEq) -> ADM playout
 *             -> IWebRtcAudioSink -> 5.0 IAudioSink
 *
 * [Build gate]
 * Only compiled into the standard artifact (VCS_WITH_WEBRTC=ON).
 */

#pragma once

#ifdef VCS_WITH_WEBRTC

#include <cstddef>
#include <cstdint>

namespace vcs {
	namespace webrtc_engine {

		/// Stream selector. Values match MediaStreamKind ordering used by the
		/// gateway, but the engine stays independent of that enum (ABI).
		enum MediaEngineStreamKind : int {
			kMediaStreamAudio = 0,
			kMediaStreamVideo = 1,
			kMediaStreamSecondaryVideo = 2,
		};

		/// Per-member parameters. All SSRC / PT fields are "current best
		/// knowledge"; the engine re-creates streams when they change.
		/// A payload type of -1 means "not negotiated yet".
		struct MediaEngineMemberConfig {
			// ---- video send ------------------------------------------------
			uint32_t videoSendSsrc = 0;
			int32_t  videoSendPayloadType = -1;
			uint32_t videoSendRtxSsrc = 0;
			int32_t  videoSendRtxPayloadType = -1;

			// ---- video receive (0 / -1 = learn from the first packet) ------
			uint32_t videoRecvSsrc = 0;
			int32_t  videoRecvPayloadType = -1;
			uint32_t videoRecvRtxSsrc = 0;
			int32_t  videoRecvRtxPayloadType = -1;

			// ---- shared video ----------------------------------------------
			int32_t  nackHistoryMs = 300;
			int32_t  minBitrateBps = 64000;
			int32_t  startBitrateBps = 512000;
			int32_t  maxBitrateBps = 4000000;
			int32_t  maxPacketSize = 1200;
			uint32_t localBitrateBps = 0;   // 0 = use startBitrateBps

			// ---- audio ------------------------------------------------------
			uint32_t audioSendSsrc = 0;
			int32_t  audioSendPayloadType = 0;   // 0 = PCMU
			uint32_t audioRecvSsrc = 0;
			int32_t  audioRecvPayloadType = 0;   // 0 = PCMU
			int32_t  audioSampleRate = 16000;    // ADM / APM internal rate
			int32_t  audioChannels = 1;

			bool enableVideo = true;
			bool enableAudio = true;
		};

		/// Engine -> gateway egress. Implemented by the RTP backend; the
		/// gateway's IRtpTransport owns the socket (ICE / DTLS / SRTP).
		class IWebRtcPacketSink {
		public:
			virtual ~IWebRtcPacketSink() = default;

			/// `streamKind` is one of MediaEngineStreamKind.
			virtual bool OnSendRtp(const char* memberId, int streamKind,
				const uint8_t* data, size_t size) = 0;
			virtual bool OnSendRtcp(const char* memberId, int streamKind,
				const uint8_t* data, size_t size) = 0;
		};

		/// Engine -> 5.0 decoder pipeline. Frames are Annex-B access units
		/// produced by the pass-through decoder (webrtc does jitter/NACK
		/// only; H.264 decoding stays in 5.0).
		class IWebRtcEncodedFrameSink {
		public:
			virtual ~IWebRtcEncodedFrameSink() = default;

			virtual void OnEncodedFrame(const char* memberId, int streamKind,
				const uint8_t* data, size_t size, bool isKeyFrame,
				uint32_t rtpTimestamp, uint32_t ssrc) = 0;
		};

		/// Engine -> 5.0 playout (speaker). Called on the engine's audio
		/// playout thread with interleaved int16 PCM.
		class IWebRtcAudioSink {
		public:
			virtual ~IWebRtcAudioSink() = default;

			virtual void OnPlayoutAudio(const char* memberId,
				const int16_t* pcm, size_t samplesPerChannel,
				int sampleRate, int channels) = 0;
		};

		/// Lightweight statistics snapshot (filled by GetStats()).
		struct MediaEngineStats {
			uint32_t packetsSent = 0;
			uint32_t packetsReceived = 0;
			uint32_t packetsLost = 0;
			uint32_t nacksSent = 0;
			uint32_t nacksReceived = 0;
			uint32_t plisSent = 0;
			uint32_t jitterMs = 0;
			uint32_t roundTripMs = 0;
			uint32_t sendBitrateBps = 0;
			int64_t  pacerDelayMs = 0;
			uint32_t framesEncoded = 0;
			uint32_t framesDecoded = 0;
		};

		/**
		 * @brief libwebrtc media engine (video + audio).
		 *
		 * Thread safety: all methods may be called from any thread; the
		 * implementation serialises Call access on its own media thread.
		 */
		class IWebRtcMediaEngine {
		public:
			virtual ~IWebRtcMediaEngine() = default;

			/// Bind the egress / ingress sinks. Must be called once before
			/// AddMember(). Any of the sinks may be null.
			virtual bool Initialize(IWebRtcPacketSink* packetSink,
				IWebRtcEncodedFrameSink* frameSink,
				IWebRtcAudioSink* audioSink) = 0;

			virtual bool AddMember(const char* memberId,
				const MediaEngineMemberConfig& config) = 0;
			virtual void RemoveMember(const char* memberId) = 0;

			/// Update the SSRCs learned from SDP / inbound traffic.
			/// Zero means "keep the current value".
			virtual void UpdateMemberSsrc(const char* memberId,
				uint32_t audioRecvSsrc, uint32_t videoRecvSsrc) = 0;

			/// Update the local (outbound) SSRCs. This is what 5.0 allocated
			/// for its own audio / video streams; the engine needs it before
			/// creating AudioSendStream / VideoSendStream.
			virtual void UpdateMemberSendSsrc(const char* memberId,
				uint32_t audioSendSsrc, uint32_t videoSendSsrc) = 0;

			/// Update the negotiated payload types for audio and video.
			/// -1 means "keep the current value".
			virtual void UpdateMemberPayloadTypes(const char* memberId,
				int32_t audioPayloadType, int32_t videoPayloadType) = 0;

			/// Video egress. `data` is an Annex-B H.264 access unit. The
			/// engine owns packetization, pacing, RTX and RTCP. `rtpTimestamp`
			/// is the 90 kHz RTP timestamp supplied by the 5.0 encoder.
			virtual bool SendEncodedFrame(const char* memberId, int streamKind,
				const uint8_t* data, size_t size, bool isKeyFrame,
				uint32_t rtpTimestamp, uint32_t ssrc,
				int32_t payloadType, int width, int height) = 0;

			/// Raw RTP datagram ingress (header + payload).
			virtual void OnRtpPacket(const char* memberId, int streamKind,
				const uint8_t* data, size_t size) = 0;

			/// Raw RTCP compound packet ingress.
			virtual void OnRtcpPacket(const char* memberId, int streamKind,
				const uint8_t* data, size_t size) = 0;

			/// Audio capture push (interleaved int16, any frame size).
			/// Returns false when the member has no audio path.
			virtual bool PushCapturedAudio(const char* memberId,
				const int16_t* pcm, size_t samplesPerChannel,
				int sampleRate, int channels) = 0;

			/// Update the egress bitrate budget (bps, 0 = unlimited).
			virtual void SetSendBitrate(const char* memberId,
				uint32_t bitrateBps) = 0;

			/// Fill `out` with a statistics snapshot. Returns false when the
			/// member is unknown.
			virtual bool GetStats(const char* memberId,
				MediaEngineStats* out) = 0;
		};

		/// Create the libwebrtc-backed engine. Returns nullptr when this build
		/// has no libwebrtc adapter (VCS_WEBRTC_BUILD_LIBWEBRTC=OFF); callers
		/// must then fall back to the native RTP path. The caller owns the
		/// object and releases it with DestroyWebRtcMediaEngine().
		IWebRtcMediaEngine* CreateWebRtcMediaEngine();

		void DestroyWebRtcMediaEngine(IWebRtcMediaEngine* engine);

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
