/**
 * @file IWebRtcRtpCodec.h
 * @brief Version-neutral WebRTC RTP codec abstraction (middle layer).
 *
 * [Why this file exists]
 * When BackendType::kWebRtc is selected, H.264 RTP packetization and
 * depacketization are performed by libwebrtc, while the actual on-wire
 * transport stays with the gateway (the SIP library's IRtpTransport; ICE /
 * DTLS / SRTP is the gateway's business).
 *
 * This mirrors the 4.5 absphone flow:
 *   4.5: encoder -> callback_->OnEncodedImage() -> libwebrtc RTP packetizer
 *        raw RTP  -> cricket::VideoChannel::OnPacketReceived()
 *                 -> libwebrtc depacketizer/frame assembler
 *                 -> FakeGenericDecoder::Decode(EncodedImage)
 *
 * [ABI note]
 * The libwebrtc adapter (engine/LibWebRtcRtpCodec.cpp) is compiled with the
 * Chromium clang-cl + libc++ toolchain that produced libwebrtc itself, while
 * the backend is compiled with the project's MSVC toolchain. Therefore this
 * header must not expose STL types across the boundary: every virtual method
 * takes only scalars / raw pointers / plain structs, and bulk output is
 * delivered through the two sink interfaces below.
 *
 * [Threading]
 * One codec instance belongs to one member (one video stream). It is not
 * thread-safe; the backend serialises calls with its own mutex.
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

		/// Per-stream parameters needed to build the RTP header.
		struct RtpCodecConfig {
			uint8_t  payloadType = 96;      // negotiated H.264 payload type
			uint32_t ssrc = 0;              // local sending SSRC
			int      maxPayloadSize = 1200; // RTP payload budget per packet
		};

		/// Receives complete RTP datagrams from PacketizeH264() (12-byte
		/// header + payload already built by libwebrtc).
		class IWebRtpPacketSink {
		public:
			virtual ~IWebRtpPacketSink() = default;

			/// Return false to abort the rest of the packetization.
			virtual bool OnRtpPacket(const uint8_t* data, size_t size) = 0;
		};

		/// Receives a complete Annex-B access unit from Depacketize().
		class IWebRtpFrameSink {
		public:
			virtual ~IWebRtpFrameSink() = default;

			/// `data` is only valid for the duration of the call.
			virtual void OnAccessUnit(const uint8_t* data, size_t size,
				bool isKeyFrame) = 0;
		};

		/**
		 * @brief H.264 RTP packetizer / depacketizer backed by libwebrtc.
		 */
		class IWebRtcRtpCodec {
		public:
			virtual ~IWebRtcRtpCodec() = default;

			/// Update the header fields used for egress. Cheap and idempotent.
			virtual void Configure(const RtpCodecConfig& config) = 0;

			/// Packetize one Annex-B H.264 buffer (single NAL, or several NAL
			/// units separated by start codes). Each produced datagram uses
			/// consecutive sequence numbers starting at `sequenceNumber`.
			/// Returns false when the input cannot be packetized.
			virtual bool PacketizeH264(const uint8_t* nalu, size_t naluLen,
				uint32_t timestamp, uint16_t sequenceNumber,
				IWebRtpPacketSink* sink) = 0;

			/// Feed one RTP payload into libwebrtc's receive path. The sink is
			/// invoked when a complete Annex-B access unit is assembled.
			virtual void Depacketize(const uint8_t* payload, size_t payloadSize,
				uint16_t sequenceNumber, uint32_t timestamp, bool markerBit,
				IWebRtpFrameSink* sink) = 0;

			/// Drop all partial state (call on SSRC change / renegotiation).
			virtual void Reset() = 0;
		};

		/**
		 * @brief Create the libwebrtc-backed codec.
		 *
		 * Returns nullptr when this build has no libwebrtc adapter
		 * (VCS_WEBRTC_BUILD_LIBWEBRTC=OFF); callers must fall back to the
		 * native RTP implementation instead of crashing. The caller owns the
		 * object and must release it with DestroyWebRtcRtpCodec().
		 */
		IWebRtcRtpCodec* CreateWebRtcRtpCodec();

		void DestroyWebRtcRtpCodec(IWebRtcRtpCodec* codec);

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
