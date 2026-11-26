/**
 * @file IWebRtcEngine.h
 * @brief Version-neutral WebRTC engine abstraction (middle layer).
 *
 * [Why this file exists]
 * The rest of the project (media backend, gateway adapter, future conference
 * code) must never include libwebrtc headers. libwebrtc's C++ API changes
 * between milestones (m140 uses webrtc::Environment and
 * CreateModularPeerConnectionFactory, older versions use raw threads), so all
 * version-specific calls are confined to the implementation translation unit
 * (media/src/backend/webrtc/engine/LibWebRtcEngine.cpp).
 *
 * This header is the contract between the upper layer and that adapter:
 *   upper layer  ->  IWebRtcEngine / IPeerConnection  ->  libwebrtc adapter
 *
 * [Usage]
 *   auto engine = vcs::webrtc_engine::CreateWebRtcEngine();
 *   engine->Initialize({});
 *   auto pc = engine->CreatePeerConnection(config, &observer);
 *   pc->AddSendStream(sendCfg);
 *   pc->AddRecvStream(recvCfg, &videoSink, &audioSink);
 *   pc->CreateOffer();                  // observer gets OnLocalDescription
 *   pc->SetRemoteDescription(...);      // answer
 *   pc->PushVideoFrame(streamId, frame);
 *
 * [Threading]
 * - Add / Remove / CreateOffer / CreateAnswer / SetRemoteDescription /
 *   AddIceCandidate may be called from any application thread; the adapter
 *   uses libwebrtc's thread-safe PeerConnection API (the same calling
 *   convention as peerconnection_client's Conductor).
 * - Observer callbacks and sink callbacks are invoked on libwebrtc threads.
 *   Implementations must be thread-safe.
 *
 * [Build gate]
 * Only compiled into the standard artifact (VCS_WITH_WEBRTC=ON).
 */

#pragma once

#ifdef VCS_WITH_WEBRTC

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vcs {
	namespace webrtc_engine {

		// ============================================================
		// Basic enums / POD types
		// ============================================================

		enum class MediaType {
			kAudio = 0,
			kVideo = 1
		};

		/// Same vocabulary as webrtc::SdpType, minus libwebrtc includes.
		enum class SdpType {
			kOffer = 0,
			kAnswer = 1,
			kPrAnswer = 2,
			kRollback = 3
		};

		enum class IceConnectionState {
			kNew = 0,
			kChecking = 1,
			kConnected = 2,
			kCompleted = 3,
			kFailed = 4,
			kDisconnected = 5,
			kClosed = 6
		};

		enum class PeerConnectionState {
			kNew = 0,
			kConnecting = 1,
			kConnected = 2,
			kDisconnected = 3,
			kFailed = 4,
			kClosed = 5
		};

		struct IceServer {
			std::string uri;        // stun:host:port / turn:host:port
			std::string username;   // TURN only
			std::string password;   // TURN only
		};

		struct EngineConfig {
			bool enableAudio = true;
			bool enableVideo = true;
		};

		struct PeerConfig {
			std::vector<IceServer> iceServers;
			bool disableEncryption = false;
		};

		/**
		 * @brief One local sending stream.
		 *
		 * streamId is the media stream id written into SDP (a=msid). trackId is
		 * the track id; when empty the adapter derives it from streamId. Video
		 * and audio use the same struct; only the fields for their type matter.
		 */
		struct SendStreamConfig {
			MediaType type = MediaType::kVideo;
			std::string streamId;
			std::string trackId;

			// Video only.
			int width = 640;
			int height = 480;
			int fps = 30;

			// Audio only.
			int sampleRate = 48000;
			int channels = 1;
		};

		/**
		 * @brief One remote receiving stream.
		 *
		 * An empty streamId means "accept the first remote stream of this
		 * type". The adapter matches on the stream id carried by the remote
		 * transceiver; sinks are attached when the track arrives in OnTrack().
		 */
		struct RecvStreamConfig {
			MediaType type = MediaType::kVideo;
			std::string streamId;
		};

		/// I420 view. Pointers are valid only for the duration of the callback /
		/// PushVideoFrame call; the adapter copies them into libwebrtc buffers.
		struct VideoFrameView {
			const uint8_t* y = nullptr;
			const uint8_t* u = nullptr;
			const uint8_t* v = nullptr;
			int width = 0;
			int height = 0;
			int strideY = 0;
			int strideU = 0;
			int strideV = 0;
			uint64_t timestampUs = 0;
		};

		/// Interleaved 16-bit PCM view.
		struct AudioFrameView {
			const int16_t* samples = nullptr;
			size_t sampleCount = 0;   // total samples (frames * channels)
			int sampleRate = 0;
			int channels = 0;
			uint64_t timestampUs = 0;
		};

		// ============================================================
		// Sinks (data flows from libwebrtc to the upper layer)
		// ============================================================

		class IVideoSink {
		public:
			virtual ~IVideoSink() = default;
			virtual void OnVideoFrame(const VideoFrameView& frame) = 0;
		};

		class IAudioSink {
		public:
			virtual ~IAudioSink() = default;
			virtual void OnAudioFrame(const AudioFrameView& frame) = 0;
		};

		// ============================================================
		// PeerConnection observer
		// ============================================================

		class IPeerObserver {
		public:
			virtual ~IPeerObserver() = default;

			/// Local offer/answer was created and applied locally.
			virtual void OnLocalDescription(SdpType type,
				const std::string& sdp) = 0;

			/// A local ICE candidate is ready for signaling.
			virtual void OnIceCandidate(const std::string& mid,
				int mlineIndex,
				const std::string& candidate) = 0;

			virtual void OnIceConnectionState(IceConnectionState /*state*/) {}
			virtual void OnConnectionState(PeerConnectionState /*state*/) {}

			/// A remote track arrived. streamId may be empty if the remote SDP
			/// did not carry msid.
			virtual void OnRemoteStreamAdded(const std::string& /*streamId*/,
				const std::string& /*trackId*/,
				MediaType /*type*/) {}
			virtual void OnRemoteStreamRemoved(const std::string& /*streamId*/,
				const std::string& /*trackId*/) {}

			virtual void OnError(const std::string& /*message*/) {}
		};

		// ============================================================
		// PeerConnection
		// ============================================================

		class IPeerConnection {
		public:
			virtual ~IPeerConnection() = default;

			// ---- local sending media ----

			/// Create the track and add it to the PeerConnection. The adapter
			/// creates a push-based source, so the first call also installs the
			/// libwebrtc track/source pair; Push*Frame() then feeds it.
			virtual bool AddSendStream(const SendStreamConfig& config) = 0;

			/// Remove the send track identified by config.streamId.
			virtual void RemoveSendStream(const std::string& streamId) = 0;

			/// Feed one I420 frame into a stream previously registered through
			/// AddSendStream(). Returns false if the stream is unknown, not a
			/// video stream, or the frame is invalid.
			virtual bool PushVideoFrame(const std::string& streamId,
				const VideoFrameView& frame) = 0;

			/// Feed interleaved 16-bit PCM into an audio send stream.
			virtual bool PushAudioFrame(const std::string& streamId,
				const AudioFrameView& frame) = 0;

			// ---- remote receiving media ----

			/// Register sinks for a remote stream. When the matching track
			/// arrives via OnTrack, the adapter attaches these sinks. Passing
			/// nullptr for a sink disables that media type.
			virtual bool AddRecvStream(const RecvStreamConfig& config,
				IVideoSink* videoSink,
				IAudioSink* audioSink) = 0;

			/// Detach the sinks registered for `streamId` (or the wildcard entry
			/// when streamId is empty).
			virtual void RemoveRecvStream(const std::string& streamId) = 0;

			// ---- signaling ----

			virtual void CreateOffer() = 0;
			virtual void CreateAnswer() = 0;

			/// Apply the remote offer/answer SDP.
			virtual bool SetRemoteDescription(SdpType type,
				const std::string& sdp) = 0;

			/// Apply a remote ICE candidate received through signaling.
			virtual bool AddIceCandidate(const std::string& mid,
				int mlineIndex,
				const std::string& candidate) = 0;

			virtual void Close() = 0;
		};

		// ============================================================
		// Engine
		// ============================================================

		class IWebRtcEngine {
		public:
			virtual ~IWebRtcEngine() = default;

			/// Create threads, the PeerConnectionFactory and media engines.
			/// Must be called once before CreatePeerConnection().
			virtual bool Initialize(const EngineConfig& config) = 0;

			/// Create one PeerConnection. `observer` must outlive the returned
			/// connection. Returns nullptr on failure.
			virtual std::unique_ptr<IPeerConnection> CreatePeerConnection(
				const PeerConfig& config,
				IPeerObserver* observer) = 0;

			/// Destroy the factory and worker threads. PeerConnections must be
			/// released before this call.
			virtual void Shutdown() = 0;
		};

		/**
		 * @brief Create the platform WebRTC engine.
		 *
		 * The returned object is the only thing the upper layer knows about.
		 * Returning nullptr means this build has no libwebrtc adapter (the
		 * lite/no-libwebrtc configuration); callers must treat that as
		 * "WebRTC unavailable" rather than crashing.
		 */
		std::unique_ptr<IWebRtcEngine> CreateWebRtcEngine();

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
