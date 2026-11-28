/**
 * @file WebRtcRtpBackend.cpp
 * @brief WebRTC RTP backend implementation (VCS_WITH_WEBRTC only)
 *
 * Media data path:
 *   - video tx : 5.0 encoder -> IWebRtcMediaEngine::SendEncodedFrame
 *                -> VideoSendStream (pacer / NACK / RTX / SR)
 *   - video rx : IRtpTransport -> OnRtpReceived -> engine OnRtpPacket
 *                -> VideoReceiveStream (jitter / NACK / PLI)
 *                -> encoded-frame sink -> RTPEngine queue
 *   - audio    : PCM push / playout through the engine's ADM (AEC3 + NetEq)
 *
 * The gateway keeps ownership of the socket (IRtpTransport); libwebrtc only
 * produces/consumes datagrams. Without libwebrtc the class falls back to
 * NativeRtpBackend plus the RTP-codec-only path.
 */

#include "WebRtcRtpBackend.h"

#ifdef VCS_WITH_WEBRTC

#include "WebRtcLogBridge.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace vcs {
	namespace media {

		namespace {

			/// Best-effort key-frame detection over an Annex-B access unit.
			bool LooksLikeKeyFrame(const uint8_t* data, size_t size) {
				if (!data || size < 1) return false;
				size_t i = 0;
				bool sawStartCode = false;
				while (i + 4 <= size) {
					if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
						sawStartCode = true;
						if (i + 3 < size) {
							const uint8_t nalType = data[i + 3] & 0x1F;
							if (nalType == 5 || nalType == 7) return true;
						}
						i += 3;
					} else if (data[i] == 0 && data[i + 1] == 0 &&
						data[i + 2] == 0 && data[i + 3] == 1) {
						sawStartCode = true;
						if (i + 4 < size) {
							const uint8_t nalType = data[i + 4] & 0x1F;
							if (nalType == 5 || nalType == 7) return true;
						}
						i += 4;
					} else {
						++i;
					}
				}
				if (!sawStartCode) {
					const uint8_t nalType = data[0] & 0x1F;
					return nalType == 5 || nalType == 7;
				}
				return false;
			}

			bool ParseRtpDatagramHeader(const uint8_t* data, size_t len,
				uint8_t& outPayloadType, uint32_t& outSsrc) {
				if (!data || len < 12) return false;
				if ((data[0] >> 6) != 2) return false;
				outPayloadType = static_cast<uint8_t>(data[1] & 0x7F);
				outSsrc = (static_cast<uint32_t>(data[8]) << 24) |
					(static_cast<uint32_t>(data[9]) << 16) |
					(static_cast<uint32_t>(data[10]) << 8) |
					static_cast<uint32_t>(data[11]);
				return true;
			}

			/// Static (RFC 3551) audio payload types.
			///
			/// Inbound routing cannot rely on the negotiated *send* payload type:
			/// the peer may answer with a different codec (e.g. it sends PCMA/8
			/// while we send PCMU/0). A PT that is unambiguously audio must never
			/// be handed to the video engine - doing so used to poison
			/// VideoReceiveStream (created with the audio SSRC/PT) and stall video
			/// forever.
			bool IsStaticAudioPayloadType(uint8_t pt) {
				switch (pt) {
				case 0:   // PCMU
				case 8:   // PCMA
				case 9:   // G722
				case 13:  // comfort noise
				case 18:  // G.729
					return true;
				default:
					return false;
				}
			}

			int ToEngineKind(vcs::MediaStreamKind kind) {
				switch (kind) {
				case vcs::MediaStreamKind::Audio:
					return webrtc_engine::kMediaStreamAudio;
				case vcs::MediaStreamKind::SecondaryVideo:
					return webrtc_engine::kMediaStreamSecondaryVideo;
				case vcs::MediaStreamKind::Video:
				default:
					return webrtc_engine::kMediaStreamVideo;
				}
			}

			vcs::MediaStreamKind FromEngineKind(int kind) {
				switch (kind) {
				case webrtc_engine::kMediaStreamAudio:
					return vcs::MediaStreamKind::Audio;
				case webrtc_engine::kMediaStreamSecondaryVideo:
					return vcs::MediaStreamKind::SecondaryVideo;
				default:
					return vcs::MediaStreamKind::Video;
				}
			}

			/// Hands each libwebrtc-produced RTP datagram to the gateway
			/// transport (RTP-codec-only fallback path).
			class TransportPacketSink : public webrtc_engine::IWebRtpPacketSink {
			public:
				explicit TransportPacketSink(std::shared_ptr< ::vcs::IRtpTransport> transport)
					: transport_(std::move(transport)) {
				}

				bool OnRtpPacket(const uint8_t* data, size_t size) override {
					::vcs::RtpPacket packet;
					packet.data = data;
					packet.length = size;
					try {
						if (!transport_->SendRtp(packet)) return false;
					}
					catch (...) {
						return false;
					}
					++sent_;
					return true;
				}

				size_t sent() const { return sent_; }

			private:
				std::shared_ptr< ::vcs::IRtpTransport> transport_;
				size_t sent_ = 0;
			};

		} // namespace

		// ------------------------------------------------------------------
		// Per-member state
		// ------------------------------------------------------------------

		struct WebRtcRtpBackend::MemberCodec {
			~MemberCodec() {
				if (codec) {
					webrtc_engine::DestroyWebRtcRtpCodec(codec);
				}
			}

			std::mutex mu;
			webrtc_engine::IWebRtcRtpCodec* codec = nullptr;
		};

		struct WebRtcRtpBackend::MemberRoute {
			bool useEngine = false;
			uint8_t audioPt = 0;
			uint8_t videoPt = 97;
			uint32_t videoRecvSsrc = 0;
		};

		// ------------------------------------------------------------------
		// Lifecycle
		// ------------------------------------------------------------------

		WebRtcRtpBackend::WebRtcRtpBackend() {
			// One webrtc.log per process, next to the other module logs.
			StartWebRtcLogging();
			engine_.reset(webrtc_engine::CreateWebRtcMediaEngine());
			if (engine_) {
				if (!engine_->Initialize(this, this, this)) {
					engine_.reset();
				}
			}
		}

		WebRtcRtpBackend::~WebRtcRtpBackend() = default;

		std::string WebRtcRtpBackend::GetBackendType() const {
			return "WebRtc";
		}

		bool WebRtcRtpBackend::UsesWebRtcMediaEngine() const {
			return engine_ != nullptr;
		}

		// ------------------------------------------------------------------
		// Member management
		// ------------------------------------------------------------------

		vcs::ErrorCode WebRtcRtpBackend::AddMember(
			const std::string& memberId,
			const BackendMemberConfig& config) {
			const vcs::ErrorCode rc = NativeRtpBackend::AddMember(memberId, config);
			if (rc != vcs::ErrorCode::Success) {
				return rc;
			}

			MemberRoute route;
			route.audioPt = config.payloadTypeAudio;
			route.videoPt = config.payloadTypeVideo;
			route.videoRecvSsrc = config.ssrcVideo;

			if (engine_) {
				webrtc_engine::MediaEngineMemberConfig engineConfig;
				engineConfig.enableAudio = config.enableAudio;
				engineConfig.enableVideo = config.enableVideo;
				engineConfig.audioSendSsrc = config.ssrcAudio;
				engineConfig.audioSendPayloadType = config.payloadTypeAudio;
				engineConfig.audioRecvPayloadType = config.payloadTypeAudio;
				engineConfig.videoSendSsrc = config.ssrcVideo;
				engineConfig.videoSendPayloadType = config.payloadTypeVideo;
				engineConfig.videoRecvSsrc = config.ssrcVideo;
				engineConfig.videoRecvPayloadType = config.payloadTypeVideo;
				route.useEngine =
					engine_->AddMember(memberId.c_str(), engineConfig);
			}

			std::lock_guard<std::mutex> lock(route_mu_);
			routes_[memberId] = route;
			return rc;
		}

		void WebRtcRtpBackend::RemoveMember(const std::string& memberId) noexcept {
			NativeRtpBackend::RemoveMember(memberId);
			try {
				if (engine_) {
					engine_->RemoveMember(memberId.c_str());
				}
				{
					std::lock_guard<std::mutex> lock(codec_mu_);
					codecs_.erase(memberId);
				}
				{
					std::lock_guard<std::mutex> lock(route_mu_);
					routes_.erase(memberId);
				}
			}
			catch (...) {
				// noexcept: cached state may leak, which is safe.
			}
		}

		void WebRtcRtpBackend::UpdateMemberSsrc(
			const std::string& memberId,
			uint32_t audioSsrc,
			uint32_t videoSsrc,
			uint32_t h239Ssrc) noexcept {
			NativeRtpBackend::UpdateMemberSsrc(memberId, audioSsrc, videoSsrc, h239Ssrc);
			try {
				if (engine_) {
					engine_->UpdateMemberSsrc(memberId.c_str(), audioSsrc, videoSsrc);
				}
				std::lock_guard<std::mutex> lock(route_mu_);
				auto it = routes_.find(memberId);
				if (it != routes_.end() && videoSsrc != 0) {
					it->second.videoRecvSsrc = videoSsrc;
				}
			}
			catch (...) {
			}
		}

		void WebRtcRtpBackend::SetMemberLocalSsrc(
			const std::string& memberId,
			uint32_t audioSsrc, uint32_t videoSsrc) {
			if (engine_) {
				engine_->UpdateMemberSendSsrc(memberId.c_str(), audioSsrc, videoSsrc);
			}
		}

		void WebRtcRtpBackend::UpdateMemberPayloadTypes(
			const std::string& memberId,
			uint8_t audioPayloadType, uint8_t videoPayloadType) {
			{
				std::lock_guard<std::mutex> lock(route_mu_);
				auto it = routes_.find(memberId);
				if (it != routes_.end()) {
					it->second.audioPt = audioPayloadType;
					it->second.videoPt = videoPayloadType;
				}
			}
			if (engine_) {
				engine_->UpdateMemberPayloadTypes(memberId.c_str(),
					static_cast<int32_t>(audioPayloadType),
					static_cast<int32_t>(videoPayloadType));
			}
		}

		std::shared_ptr<WebRtcRtpBackend::MemberCodec>
		WebRtcRtpBackend::GetOrCreateCodec(const std::string& memberId) {
			std::lock_guard<std::mutex> lock(codec_mu_);
			std::shared_ptr<MemberCodec>& slot = codecs_[memberId];
			if (!slot) {
				slot = std::make_shared<MemberCodec>();
				slot->codec = webrtc_engine::CreateWebRtcRtpCodec();
			}
			return slot;
		}

		WebRtcRtpBackend::MemberRoute
		WebRtcRtpBackend::GetRoute(const std::string& memberId) {
			std::lock_guard<std::mutex> lock(route_mu_);
			auto it = routes_.find(memberId);
			if (it == routes_.end()) return MemberRoute();
			return it->second;
		}

		bool WebRtcRtpBackend::IsVideoPayload(
			const MemberRoute& route, uint8_t payloadType) const {
			if (route.audioPt == route.videoPt) return false;
			return payloadType == route.videoPt;
		}

		// ------------------------------------------------------------------
		// Media send
		// ------------------------------------------------------------------

		vcs::ErrorCode WebRtcRtpBackend::SendH264Nalu(
			const std::string& memberId,
			::vcs::MediaStreamKind kind,
			uint8_t payloadType,
			const uint8_t* naluData,
			size_t naluLen,
			uint32_t ssrc,
			uint64_t timestamp,
			uint16_t sequenceNumber,
			uint16_t* outLastSeq) {
			const MemberRoute route = GetRoute(memberId);

			// The media engine owns primary-video packetization / pacing.
			if (engine_ && route.useEngine &&
				kind == ::vcs::MediaStreamKind::Video) {
				if (!naluData || naluLen == 0) {
					return vcs::ErrorCode::InvalidParams;
				}
				const bool isKeyFrame = LooksLikeKeyFrame(naluData, naluLen);
				const bool sent = engine_->SendEncodedFrame(
					memberId.c_str(),
					webrtc_engine::kMediaStreamVideo,
					naluData, naluLen, isKeyFrame,
					static_cast<uint32_t>(timestamp), ssrc,
					static_cast<int32_t>(payloadType), 0, 0);
				if (outLastSeq) {
					// libwebrtc generates the real sequence numbers; the value
					// here only feeds legacy diagnostics.
					*outLastSeq = sequenceNumber;
				}
				return sent ? vcs::ErrorCode::Success
					: vcs::ErrorCode::MediaSendFailed;
			}

			// Secondary video / no engine: keep the RTP-codec-only path.
			std::shared_ptr<MemberCodec> state = GetOrCreateCodec(memberId);
			if (!state || !state->codec) {
				return NativeRtpBackend::SendH264Nalu(memberId, kind, payloadType,
					naluData, naluLen, ssrc, timestamp, sequenceNumber, outLastSeq);
			}
			if (!naluData || naluLen == 0) {
				return vcs::ErrorCode::InvalidParams;
			}

			std::shared_ptr< ::vcs::IRtpTransport> transport = GetTransport(memberId, kind);
			if (!transport) {
				return vcs::ErrorCode::MediaTransportNotReady;
			}

			TransportPacketSink sink(transport);
			bool packetized = false;
			{
				std::lock_guard<std::mutex> lock(state->mu);
				webrtc_engine::RtpCodecConfig config;
				config.payloadType = payloadType;
				config.ssrc = ssrc;
				state->codec->Configure(config);

				packetized = state->codec->PacketizeH264(
					naluData, naluLen, static_cast<uint32_t>(timestamp),
					sequenceNumber, &sink);
			}

			const size_t sent = sink.sent();
			if (outLastSeq) {
				*outLastSeq = sent == 0
					? static_cast<uint16_t>(sequenceNumber - 1)
					: static_cast<uint16_t>(sequenceNumber + sent - 1);
			}
			if (!packetized) {
				return sent == 0 ? vcs::ErrorCode::InvalidParams
					: vcs::ErrorCode::MediaSendFailed;
			}
			return vcs::ErrorCode::Success;
		}

		vcs::ErrorCode WebRtcRtpBackend::SetSendBandwidth(
			const std::string& memberId, uint32_t bps) {
			const vcs::ErrorCode rc = NativeRtpBackend::SetSendBandwidth(memberId, bps);
			if (engine_) {
				engine_->SetSendBitrate(memberId.c_str(), bps);
			}
			return rc;
		}

		// ------------------------------------------------------------------
		// Ingress
		// ------------------------------------------------------------------

		void WebRtcRtpBackend::RouteRtpToEngine(const std::string& memberId,
			const uint8_t* data, size_t len) {
			uint8_t pt = 0;
			uint32_t ssrc = 0;
			if (!ParseRtpDatagramHeader(data, len, pt, ssrc)) {
				NativeRtpBackend::OnRtpReceived(memberId, data, len);
				return;
			}

			const MemberRoute route = GetRoute(memberId);
			if (!engine_ || !route.useEngine) {
				NativeRtpBackend::OnRtpReceived(memberId, data, len);
				return;
			}

			if (pt == route.audioPt && route.audioPt != route.videoPt) {
				// Audio is fully owned by the engine (NetEq + AEC): do not let
				// the legacy decoder see the packet a second time.
				engine_->OnRtpPacket(memberId.c_str(),
					webrtc_engine::kMediaStreamAudio, data, len);
				return;
			}

			// The peer may pick a different audio codec than we send, so the
			// route's "audio" PT is not authoritative for ingress.
			if (IsStaticAudioPayloadType(pt) && pt != route.videoPt) {
				engine_->OnRtpPacket(memberId.c_str(),
					webrtc_engine::kMediaStreamAudio, data, len);
				return;
			}

			const bool videoByPt = IsVideoPayload(route, pt);
			const bool videoBySsrc = route.videoRecvSsrc != 0 &&
				ssrc == route.videoRecvSsrc;
			if (videoByPt || videoBySsrc) {
				engine_->OnRtpPacket(memberId.c_str(),
					webrtc_engine::kMediaStreamVideo, data, len);
				// Also let RTPEngine learn the SSRC (it skips depacketization
				// when UsesWebRtcMediaEngine() is true) and keep its counters.
				NativeRtpBackend::OnRtpReceived(memberId, data, len);
				return;
			}

			// Unknown payload type (unnegotiated RTX, FEC, telephone-event...).
			// Do NOT hand it to the video engine: the receive stream would be
			// created from this packet's SSRC/PT and never decode again.
			// Dropping is safe; RTX/FEC need explicit config to be useful.
		}

		void WebRtcRtpBackend::OnRtpReceived(const std::string& memberId,
			const uint8_t* data, size_t len) {
			RouteRtpToEngine(memberId, data, len);
		}

		void WebRtcRtpBackend::OnRtpReceivedParsed(
			const std::string& memberId, uint8_t payloadType,
			const uint8_t* data, size_t len,
			uint32_t ssrc, uint64_t timestamp,
			uint16_t sequenceNumber, bool markerBit) {
			if (!data || len == 0) return;

			// Rebuild a minimal RTP header so the engine sees a datagram it
			// can demux. Header extensions are not present on this path.
			std::vector<uint8_t> datagram(12 + len);
			datagram[0] = 0x80;
			datagram[1] = static_cast<uint8_t>(
				(markerBit ? 0x80 : 0x00) | (payloadType & 0x7F));
			datagram[2] = static_cast<uint8_t>((sequenceNumber >> 8) & 0xFF);
			datagram[3] = static_cast<uint8_t>(sequenceNumber & 0xFF);
			datagram[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
			datagram[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
			datagram[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
			datagram[7] = static_cast<uint8_t>(timestamp & 0xFF);
			datagram[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
			datagram[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
			datagram[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
			datagram[11] = static_cast<uint8_t>(ssrc & 0xFF);
			std::memcpy(datagram.data() + 12, data, len);

			RouteRtpToEngine(memberId, datagram.data(), datagram.size());
		}

		void WebRtcRtpBackend::OnRtcpReceived(const std::string& memberId,
			const uint8_t* data, size_t len) {
			// Keep the legacy statistics callback alive...
			NativeRtpBackend::OnRtcpReceived(memberId, data, len);
			// ...and let the engine's RTCP modules (NACK/PLI/SR) react to it.
			if (engine_ && data && len >= 4) {
				const MemberRoute route = GetRoute(memberId);
				if (route.useEngine) {
					engine_->OnRtcpPacket(memberId.c_str(),
						webrtc_engine::kMediaStreamVideo, data, len);
				}
			}
		}

		IRtpBackend::H264DepacketizeResult WebRtcRtpBackend::DepacketizeH264(
			const std::string& memberId,
			const uint8_t* payload, size_t payloadSize,
			uint16_t sequenceNumber, uint64_t timestamp, bool markerBit) {
			const MemberRoute route = GetRoute(memberId);
			if (engine_ && route.useEngine) {
				// The media engine already emitted a jitter-buffered frame via
				// OnEncodedFrame(); RTPEngine must not depacketize twice.
				return IRtpBackend::H264DepacketizeResult{};
			}

			std::shared_ptr<MemberCodec> state = GetOrCreateCodec(memberId);
			if (!state || !state->codec) {
				return NativeRtpBackend::DepacketizeH264(memberId, payload, payloadSize,
					sequenceNumber, timestamp, markerBit);
			}

			class ResultFrameSink : public webrtc_engine::IWebRtpFrameSink {
			public:
				void OnAccessUnit(const uint8_t* data, size_t size,
					bool isKeyFrame) override {
					if (result.complete || !data || size == 0) return;
					result.nalu.assign(data, data + size);
					result.isKeyFrame = isKeyFrame;
					result.complete = true;
				}

				IRtpBackend::H264DepacketizeResult result;
			};

			ResultFrameSink sink;
			{
				std::lock_guard<std::mutex> lock(state->mu);
				state->codec->Depacketize(payload, payloadSize, sequenceNumber,
					static_cast<uint32_t>(timestamp), markerBit, &sink);
			}
			return std::move(sink.result);
		}

		// ------------------------------------------------------------------
		// Media-engine sinks
		// ------------------------------------------------------------------

		bool WebRtcRtpBackend::OnSendRtp(const char* memberId, int streamKind,
			const uint8_t* data, size_t size) {
			if (!memberId || !data || size == 0) return false;
			std::shared_ptr< ::vcs::IRtpTransport> transport =
				GetTransport(memberId, FromEngineKind(streamKind));
			if (!transport) return false;

			::vcs::RtpPacket packet;
			packet.data = data;
			packet.length = size;
			try {
				return transport->SendRtp(packet);
			}
			catch (...) {
				return false;
			}
		}

		bool WebRtcRtpBackend::OnSendRtcp(const char* memberId, int streamKind,
			const uint8_t* data, size_t size) {
			if (!memberId || !data || size == 0) return false;
			std::shared_ptr< ::vcs::IRtpTransport> transport =
				GetTransport(memberId, FromEngineKind(streamKind));
			if (!transport) return false;

			::vcs::RtcpPacket packet;
			packet.data = data;
			packet.length = size;
			try {
				return transport->SendRtcp(packet);
			}
			catch (...) {
				return false;
			}
		}

		void WebRtcRtpBackend::OnEncodedFrame(const char* memberId, int streamKind,
			const uint8_t* data, size_t size, bool isKeyFrame,
			uint32_t rtpTimestamp, uint32_t ssrc) {
			if (!memberId || !data || size == 0) return;
			EncodedVideoFrameCallback cb;
			{
				std::lock_guard<std::mutex> lock(sink_mu_);
				cb = encodedFrameCallback_;
			}
			if (cb) {
				cb(memberId, data, size, isKeyFrame, rtpTimestamp, ssrc,
					streamKind == webrtc_engine::kMediaStreamSecondaryVideo);
			}
		}

		void WebRtcRtpBackend::OnPlayoutAudio(const char* memberId,
			const int16_t* pcm, size_t samplesPerChannel,
			int sampleRate, int channels) {
			if (!memberId) return;
			AudioPlayoutCallback cb;
			{
				std::lock_guard<std::mutex> lock(sink_mu_);
				cb = audioPlayoutCallback_;
			}
			if (cb) {
				cb(memberId, pcm, samplesPerChannel, sampleRate, channels);
			}
		}

		void WebRtcRtpBackend::SetEncodedVideoFrameCallback(
			EncodedVideoFrameCallback cb) {
			std::lock_guard<std::mutex> lock(sink_mu_);
			encodedFrameCallback_ = std::move(cb);
		}

		void WebRtcRtpBackend::SetAudioPlayoutCallback(
			AudioPlayoutCallback cb) {
			std::lock_guard<std::mutex> lock(sink_mu_);
			audioPlayoutCallback_ = std::move(cb);
		}

		bool WebRtcRtpBackend::PushCapturedAudio(
			const std::string& memberId,
			const int16_t* pcm, size_t samplesPerChannel,
			int sampleRate, int channels) {
			if (!engine_) return false;
			const MemberRoute route = GetRoute(memberId);
			if (!route.useEngine) return false;
			return engine_->PushCapturedAudio(memberId.c_str(), pcm,
				samplesPerChannel, sampleRate, channels);
		}

	} // namespace media
} // namespace vcs

#endif // VCS_WITH_WEBRTC
