/**
 * @file LibWebRtcRtpCodec.cpp
 * @brief libwebrtc (m140) adapter for IWebRtcRtpCodec.
 *
 * *** THIS TRANSLATION UNIT IS COMPILED WITH CHROMIUM'S CLANG-CL + LIBC++
 *     TOOLCHAIN ***  libwebrtc m140 itself is built that way, so its C++
 *     symbols live in `std::__Cr::` and cannot be linked from MSVC-STL
 *     objects. CMake drives this single file through clang-cl
 *     (see media/src/backend/webrtc/CMakeLists.txt).
 *
 * The flow copied from libwebrtc's own receive path
 * (video/rtp_video_stream_receiver2.cc):
 *
 *   RtpPacketReceived::Parse
 *     -> VideoRtpDepacketizerH264::Parse          (RFC 6184 -> NAL units)
 *     -> H264SpsPpsTracker::CopyAndFixBitstream   (Annex-B + SPS/PPS fixup)
 *     -> video_coding::PacketBuffer::InsertPacket (reorder / completeness)
 *     -> VideoRtpDepacketizer::AssembleFrame      (access unit bitstream)
 *
 * The egress direction uses RtpPacketizerH264 (the same component the
 * libwebrtc video send stream uses when a frame leaves the encoder).
 */

#include "webrtc/IWebRtcRtpCodec.h"

#ifdef VCS_WITH_WEBRTC

#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "api/video/encoded_image.h"
#include "api/video/video_frame_type.h"
#include "modules/rtp_rtcp/source/rtp_format_h264.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer_h264.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/h264_sps_pps_tracker.h"
#include "modules/video_coding/packet_buffer.h"
#include "rtc_base/numerics/sequence_number_unwrapper.h"

namespace vcs {
	namespace webrtc_engine {

		namespace {

			constexpr size_t kRtpHeaderSize = 12;

			bool HasH264StartCode(const uint8_t* data, size_t size) {
				if (size >= 4 &&
					data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
					return true;
				}
				if (size >= 3 &&
					data[0] == 0 && data[1] == 0 && data[2] == 1) {
					return true;
				}
				return false;
			}

			class LibWebRtcRtpCodec : public IWebRtcRtpCodec {
			public:
				LibWebRtcRtpCodec()
					: depacketizer_(std::make_unique<webrtc::VideoRtpDepacketizerH264>()),
					tracker_(std::make_unique<webrtc::video_coding::H264SpsPpsTracker>()),
					packet_buffer_(std::make_unique<webrtc::video_coding::PacketBuffer>(2048, 2048)) {
				}

				// ---- IWebRtcRtpCodec -------------------------------------------

				void Configure(const RtpCodecConfig& config) override {
					config_ = config;
				}

				bool PacketizeH264(const uint8_t* nalu, size_t naluLen,
					uint32_t timestamp, uint16_t sequenceNumber,
					IWebRtpPacketSink* sink) override {
					if (!nalu || naluLen == 0 || !sink) return false;

					// RtpPacketizerH264 locates NAL units by Annex-B start codes.
					// Terminals normally hand us start-code prefixed buffers, but
					// a bare NAL unit would produce zero packets, so normalise.
					std::vector<uint8_t> withStartCode;
					const uint8_t* data = nalu;
					size_t size = naluLen;
					if (!HasH264StartCode(data, size)) {
						static const uint8_t kStartCode[4] = { 0x00, 0x00, 0x00, 0x01 };
						withStartCode.reserve(size + sizeof(kStartCode));
						withStartCode.insert(withStartCode.end(), kStartCode,
							kStartCode + sizeof(kStartCode));
						withStartCode.insert(withStartCode.end(), nalu, nalu + naluLen);
						data = withStartCode.data();
						size = withStartCode.size();
					}

					webrtc::RtpPacketizer::PayloadSizeLimits limits;
					limits.max_payload_len = config_.maxPayloadSize > 0
						? config_.maxPayloadSize : 1200;

					webrtc::RtpPacketizerH264 packetizer(
						webrtc::ArrayView<const uint8_t>(data, size), limits,
						webrtc::H264PacketizationMode::NonInterleaved);
					if (packetizer.NumPackets() == 0) return false;

					uint16_t seq = sequenceNumber;
					while (packetizer.NumPackets() > 0) {
						webrtc::RtpPacketToSend packet(nullptr);
						if (!packetizer.NextPacket(&packet)) return false;
						packet.SetPayloadType(config_.payloadType);
						packet.SetSequenceNumber(seq++);
						packet.SetTimestamp(timestamp);
						packet.SetSsrc(config_.ssrc);

						webrtc::CopyOnWriteBuffer buffer = packet.Buffer();
						if (!sink->OnRtpPacket(buffer.cdata(), buffer.size())) {
							return false;
						}
					}
					return true;
				}

				void Depacketize(const uint8_t* payload, size_t payloadSize,
					uint16_t sequenceNumber, uint32_t timestamp, bool markerBit,
					IWebRtpFrameSink* sink) override {
					if (!payload || payloadSize == 0 || !sink) return;

					// Rewrap the payload into a minimal RTP datagram so the
					// libwebrtc receive path sees the same header fields it
					// would see on the wire (marker / seq / timestamp).
					std::vector<uint8_t> raw(kRtpHeaderSize + payloadSize);
					raw[0] = 0x80;   // V=2, P=0, X=0, CC=0
					raw[1] = static_cast<uint8_t>(
						(markerBit ? 0x80 : 0x00) | (config_.payloadType & 0x7F));
					raw[2] = static_cast<uint8_t>((sequenceNumber >> 8) & 0xFF);
					raw[3] = static_cast<uint8_t>(sequenceNumber & 0xFF);
					raw[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
					raw[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
					raw[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
					raw[7] = static_cast<uint8_t>(timestamp & 0xFF);
					raw[8] = static_cast<uint8_t>((config_.ssrc >> 24) & 0xFF);
					raw[9] = static_cast<uint8_t>((config_.ssrc >> 16) & 0xFF);
					raw[10] = static_cast<uint8_t>((config_.ssrc >> 8) & 0xFF);
					raw[11] = static_cast<uint8_t>(config_.ssrc & 0xFF);
					std::memcpy(raw.data() + kRtpHeaderSize, payload, payloadSize);

					webrtc::RtpPacketReceived packet;
					if (!packet.Parse(raw.data(), raw.size())) return;

					std::optional<webrtc::VideoRtpDepacketizer::ParsedRtpPayload>
						parsed = depacketizer_->Parse(packet.PayloadBuffer());
					if (!parsed) return;

					// Annex-B conversion + in-band SPS/PPS bookkeeping, same as
					// RtpVideoStreamReceiver2. kDrop (malformed STAP-A) and
					// kRequestKeyframe (IDR seen before SPS/PPS) both mean
					// "nothing to hand downstream yet".
					webrtc::video_coding::H264SpsPpsTracker::FixedBitstream fixed =
						tracker_->CopyAndFixBitstream(
							webrtc::MakeArrayView(parsed->video_payload.cdata(),
								parsed->video_payload.size()),
							&parsed->video_header);
					if (fixed.action != webrtc::video_coding::H264SpsPpsTracker::kInsert)
						return;
					parsed->video_payload = std::move(fixed.bitstream);

					parsed->video_header.is_last_packet_in_frame |= packet.Marker();

					auto cached = std::make_unique<webrtc::video_coding::PacketBuffer::Packet>(
						packet, sequence_unwrapper_.Unwrap(packet.SequenceNumber()),
						parsed->video_header);
					cached->video_payload = std::move(parsed->video_payload);
					cached->marker_bit = packet.Marker();

					webrtc::video_coding::PacketBuffer::InsertResult inserted =
						packet_buffer_->InsertPacket(std::move(cached));

					EmitAssembledFrames(inserted, sink);
				}

				void Reset() override {
					depacketizer_ = std::make_unique<webrtc::VideoRtpDepacketizerH264>();
					tracker_ = std::make_unique<webrtc::video_coding::H264SpsPpsTracker>();
					packet_buffer_ =
						std::make_unique<webrtc::video_coding::PacketBuffer>(2048, 2048);
					sequence_unwrapper_.Reset();
				}

			private:
				// Same grouping logic as RtpVideoStreamReceiver2::OnInsertedPacket,
				// minus the frame-reference bookkeeping (not needed here: the
				// access unit is handed straight to the decoder pipeline).
				void EmitAssembledFrames(
					webrtc::video_coding::PacketBuffer::InsertResult& inserted,
					IWebRtpFrameSink* sink) {
					webrtc::video_coding::PacketBuffer::Packet* first_packet = nullptr;
					std::vector<webrtc::ArrayView<const uint8_t>> payloads;

					for (auto& packet : inserted.packets) {
						if (packet->is_first_packet_in_frame()) {
							first_packet = packet.get();
							payloads.clear();
						}
						if (!first_packet) continue;

						payloads.emplace_back(packet->video_payload);
						if (!packet->is_last_packet_in_frame()) continue;

						webrtc::scoped_refptr<webrtc::EncodedImageBuffer> bitstream =
							depacketizer_->AssembleFrame(payloads);
						if (bitstream && bitstream->size() > 0) {
							sink->OnAccessUnit(bitstream->data(), bitstream->size(),
								first_packet->video_header.frame_type ==
									webrtc::VideoFrameType::kVideoFrameKey);
						}
						first_packet = nullptr;
						payloads.clear();
					}
				}

				RtpCodecConfig config_;
				std::unique_ptr<webrtc::VideoRtpDepacketizerH264> depacketizer_;
				std::unique_ptr<webrtc::video_coding::H264SpsPpsTracker> tracker_;
				std::unique_ptr<webrtc::video_coding::PacketBuffer> packet_buffer_;
				webrtc::SeqNumUnwrapper<uint16_t> sequence_unwrapper_;
			};

		} // namespace

		IWebRtcRtpCodec* CreateWebRtcRtpCodec() {
			return new LibWebRtcRtpCodec();
		}

		void DestroyWebRtcRtpCodec(IWebRtcRtpCodec* codec) {
			delete codec;
		}

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
