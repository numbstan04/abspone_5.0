/**
 * @file LibWebRtcEngine.cpp
 * @brief libwebrtc (m140) adapter for IWebRtcEngine.
 *
 * *** THIS IS THE ONLY TRANSLATION UNIT THAT INCLUDES LIBWEBRTC HEADERS. ***
 *
 * When the libwebrtc milestone changes, the compiler errors point here and
 * the rest of the project (IWebRtcEngine.h consumers) stays untouched.
 *
 * The implementation follows examples/peerconnection/client/conductor.cc:
 *   - EnvironmentFactory -> Environment
 *   - Thread::CreateWithSocketServer() -> signaling thread
 *   - PeerConnectionFactoryDependencies + EnableMedia()
 *   - CreateModularPeerConnectionFactory()
 *   - CreatePeerConnectionOrError()
 *   - CreateVideoTrack / CreateAudioTrack + AddTrack()
 *   - OnTrack() -> attach VideoSinkInterface / AudioTrackSinkInterface
 */

#include "webrtc/IWebRtcEngine.h"

#ifdef VCS_WITH_WEBRTC

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "media/base/adapted_video_track_source.h"
#include "rtc_base/checks.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

namespace vcs {
	namespace webrtc_engine {

		namespace {

			// ============================================================
			// SSL lifetime (process wide, reference counted)
			// ============================================================

			std::mutex g_ssl_mutex;
			int g_ssl_refs = 0;

			void AddSslRef() {
				std::lock_guard<std::mutex> lk(g_ssl_mutex);
				if (g_ssl_refs++ == 0) {
					webrtc::InitializeSSL();
				}
			}

			void ReleaseSslRef() {
				std::lock_guard<std::mutex> lk(g_ssl_mutex);
				if (--g_ssl_refs == 0) {
					webrtc::CleanupSSL();
				}
			}

			// ============================================================
			// Engine context: threads + factory shared by all PeerConnections
			// ============================================================

			struct EngineContext {
				explicit EngineContext(const webrtc::Environment& e) : env(e) {
					AddSslRef();
				}

				~EngineContext() {
					factory = nullptr;
					if (signaling_thread) {
						signaling_thread->Stop();
						signaling_thread.reset();
					}
					ReleaseSslRef();
				}

				webrtc::Environment env;
				std::unique_ptr<webrtc::Thread> signaling_thread;
				webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
			};

			// ============================================================
			// Push-based sources
			// ============================================================

			/// I420 frame source fed by PushVideoFrame(). AdaptedVideoTrackSource
			/// implements the sink plumbing and (de)serialisation on the worker
			/// thread; OnFrame() may be called from any thread.
			class PushVideoSource : public webrtc::AdaptedVideoTrackSource {
			public:
				PushVideoSource() = default;

				webrtc::MediaSourceInterface::SourceState state() const override {
					return webrtc::MediaSourceInterface::kLive;
				}
				bool remote() const override { return false; }
				bool is_screencast() const override { return false; }
				std::optional<bool> needs_denoising() const override {
					return std::nullopt;
				}

				void Push(const VideoFrameView& view) {
					if (!view.y || !view.u || !view.v ||
						view.width <= 0 || view.height <= 0) {
						return;
					}
					webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
						webrtc::I420Buffer::Copy(
							view.width, view.height,
							view.y, view.strideY,
							view.u, view.strideU,
							view.v, view.strideV);
					webrtc::VideoFrame frame =
						webrtc::VideoFrame::Builder()
						.set_video_frame_buffer(buffer)
						.set_timestamp_us(view.timestampUs)
						.build();
					OnFrame(frame);
				}
			};

			/// 16-bit PCM source fed by PushAudioFrame(). The audio track
			/// registers itself as an AudioTrackSinkInterface, so broadcasting
			/// OnData() here is enough to feed the libwebrtc audio pipeline.
			class PushAudioSource : public webrtc::AudioSourceInterface {
			public:
				webrtc::MediaSourceInterface::SourceState state() const override {
					return webrtc::MediaSourceInterface::kLive;
				}
				bool remote() const override { return false; }

				// NotifierInterface. The terminal never observes source state.
				void RegisterObserver(webrtc::ObserverInterface*) override {}
				void UnregisterObserver(webrtc::ObserverInterface*) override {}

				void AddSink(webrtc::AudioTrackSinkInterface* sink) override {
					if (!sink) return;
					std::lock_guard<std::mutex> lk(mu_);
					if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end()) {
						sinks_.push_back(sink);
					}
				}

				void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override {
					std::lock_guard<std::mutex> lk(mu_);
					sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink),
						sinks_.end());
				}

				void Push(const AudioFrameView& view) {
					if (!view.samples || view.sampleRate <= 0 || view.channels <= 0) {
						return;
					}
					std::vector<webrtc::AudioTrackSinkInterface*> sinks;
					{
						std::lock_guard<std::mutex> lk(mu_);
						sinks = sinks_;
					}
					const size_t frames = view.sampleCount /
						static_cast<size_t>(view.channels);
					for (webrtc::AudioTrackSinkInterface* sink : sinks) {
						sink->OnData(view.samples, 16, view.sampleRate,
							static_cast<size_t>(view.channels), frames);
					}
				}

			private:
				std::mutex mu_;
				std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
			};

			// ============================================================
			// Receive-side sink adapters (libwebrtc -> neutral interfaces)
			// ============================================================

			class VideoSinkAdapter : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
			public:
				explicit VideoSinkAdapter(IVideoSink* sink) : sink_(sink) {}

				void OnFrame(const webrtc::VideoFrame& frame) override {
					if (!sink_) return;
					webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
						frame.video_frame_buffer()->ToI420();
					if (!i420) return;

					VideoFrameView view;
					view.y = i420->DataY();
					view.u = i420->DataU();
					view.v = i420->DataV();
					view.width = i420->width();
					view.height = i420->height();
					view.strideY = i420->StrideY();
					view.strideU = i420->StrideU();
					view.strideV = i420->StrideV();
					view.timestampUs = frame.timestamp_us();
					sink_->OnVideoFrame(view);
				}

			private:
				IVideoSink* sink_ = nullptr;
			};

			class AudioSinkAdapter : public webrtc::AudioTrackSinkInterface {
			public:
				explicit AudioSinkAdapter(IAudioSink* sink) : sink_(sink) {}

				void OnData(const void* audioData,
					int bitsPerSample,
					int sampleRate,
					size_t numberOfChannels,
					size_t numberOfFrames) override {
					if (!sink_ || !audioData || bitsPerSample != 16 ||
						numberOfChannels == 0) {
						return;
					}
					AudioFrameView view;
					view.samples = static_cast<const int16_t*>(audioData);
					view.sampleCount = numberOfFrames * numberOfChannels;
					view.sampleRate = sampleRate;
					view.channels = static_cast<int>(numberOfChannels);
					sink_->OnAudioFrame(view);
				}

			private:
				IAudioSink* sink_ = nullptr;
			};

			// ============================================================
			// Session-description observers
			// ============================================================

			class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
			public:
				using SuccessFn = std::function<void(webrtc::SessionDescriptionInterface*)>;
				using FailureFn = std::function<void(const std::string&)>;

				CreateSdpObserver(SuccessFn onSuccess, FailureFn onFailure)
					: onSuccess_(std::move(onSuccess)), onFailure_(std::move(onFailure)) {}

				void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
					if (onSuccess_) onSuccess_(desc);
					else delete desc;
				}

				void OnFailure(webrtc::RTCError error) override {
					if (onFailure_) onFailure_(error.message());
				}

			private:
				SuccessFn onSuccess_;
				FailureFn onFailure_;
			};

			class SetLocalObserver : public webrtc::SetSessionDescriptionObserver {
			public:
				using FailureFn = std::function<void(const std::string&)>;
				explicit SetLocalObserver(FailureFn onFailure)
					: onFailure_(std::move(onFailure)) {}

				void OnSuccess() override {}
				void OnFailure(webrtc::RTCError error) override {
					if (onFailure_) onFailure_(error.message());
				}

			private:
				FailureFn onFailure_;
			};

			class SetRemoteObserver
				: public webrtc::SetRemoteDescriptionObserverInterface {
			public:
				using FailureFn = std::function<void(const std::string&)>;
				explicit SetRemoteObserver(FailureFn onFailure)
					: onFailure_(std::move(onFailure)) {}

				void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
					if (!error.ok() && onFailure_) onFailure_(error.message());
				}

			private:
				FailureFn onFailure_;
			};

			// ============================================================
			// Enum translation
			// ============================================================

			SdpType ToSdpType(webrtc::SdpType type) {
				switch (type) {
				case webrtc::SdpType::kOffer:    return SdpType::kOffer;
				case webrtc::SdpType::kAnswer:   return SdpType::kAnswer;
				case webrtc::SdpType::kPrAnswer: return SdpType::kPrAnswer;
				case webrtc::SdpType::kRollback: return SdpType::kRollback;
				}
				return SdpType::kOffer;
			}

			webrtc::SdpType ToLibSdpType(SdpType type) {
				switch (type) {
				case SdpType::kOffer:    return webrtc::SdpType::kOffer;
				case SdpType::kAnswer:   return webrtc::SdpType::kAnswer;
				case SdpType::kPrAnswer: return webrtc::SdpType::kPrAnswer;
				case SdpType::kRollback: return webrtc::SdpType::kRollback;
				}
				return webrtc::SdpType::kOffer;
			}

			IceConnectionState ToIceState(
				webrtc::PeerConnectionInterface::IceConnectionState s) {
				using S = webrtc::PeerConnectionInterface::IceConnectionState;
				switch (s) {
				case S::kIceConnectionNew:          return IceConnectionState::kNew;
				case S::kIceConnectionChecking:     return IceConnectionState::kChecking;
				case S::kIceConnectionConnected:    return IceConnectionState::kConnected;
				case S::kIceConnectionCompleted:    return IceConnectionState::kCompleted;
				case S::kIceConnectionFailed:       return IceConnectionState::kFailed;
				case S::kIceConnectionDisconnected: return IceConnectionState::kDisconnected;
				case S::kIceConnectionClosed:       return IceConnectionState::kClosed;
				}
				return IceConnectionState::kNew;
			}

			PeerConnectionState ToPeerState(
				webrtc::PeerConnectionInterface::PeerConnectionState s) {
				using S = webrtc::PeerConnectionInterface::PeerConnectionState;
				switch (s) {
				case S::kNew:          return PeerConnectionState::kNew;
				case S::kConnecting:   return PeerConnectionState::kConnecting;
				case S::kConnected:    return PeerConnectionState::kConnected;
				case S::kDisconnected: return PeerConnectionState::kDisconnected;
				case S::kFailed:       return PeerConnectionState::kFailed;
				case S::kClosed:       return PeerConnectionState::kClosed;
				}
				return PeerConnectionState::kNew;
			}

			class LibPeerConnection;

			// ============================================================
			// PeerConnectionObserver adapter
			// ============================================================

			class PeerObserver : public webrtc::PeerConnectionObserver {
			public:
				explicit PeerObserver(LibPeerConnection* owner) : owner_(owner) {}

				void OnSignalingChange(
					webrtc::PeerConnectionInterface::SignalingState) override {}
				void OnIceConnectionChange(
					webrtc::PeerConnectionInterface::IceConnectionState newState) override;
				void OnConnectionChange(
					webrtc::PeerConnectionInterface::PeerConnectionState newState) override;
				void OnIceGatheringChange(
					webrtc::PeerConnectionInterface::IceGatheringState) override {}
				void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
				void OnIceConnectionReceivingChange(bool) override {}
				void OnTrack(
					webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
				void OnRemoveTrack(
					webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
				void OnDataChannel(
					webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {}
				void OnRenegotiationNeeded() override {}

			private:
				LibPeerConnection* owner_;
			};

			// ============================================================
			// LibPeerConnection
			// ============================================================

			class LibPeerConnection : public IPeerConnection {
			public:
				LibPeerConnection(std::shared_ptr<EngineContext> ctx,
					const PeerConfig& config,
					IPeerObserver* observer);
				~LibPeerConnection() override;

				bool AddSendStream(const SendStreamConfig& config) override;
				void RemoveSendStream(const std::string& streamId) override;
				bool PushVideoFrame(const std::string& streamId,
					const VideoFrameView& frame) override;
				bool PushAudioFrame(const std::string& streamId,
					const AudioFrameView& frame) override;

				bool AddRecvStream(const RecvStreamConfig& config,
					IVideoSink* videoSink,
					IAudioSink* audioSink) override;
				void RemoveRecvStream(const std::string& streamId) override;

				void CreateOffer() override;
				void CreateAnswer() override;
				bool SetRemoteDescription(SdpType type, const std::string& sdp) override;
				bool AddIceCandidate(const std::string& mid,
					int mlineIndex,
					const std::string& candidate) override;
				void Close() override;

				// PeerObserver callbacks (called on libwebrtc threads).
				void HandleIceState(IceConnectionState state);
				void HandlePeerState(PeerConnectionState state);
				void HandleIceCandidate(const webrtc::IceCandidate& candidate);
				void HandleRemoteTrack(
					webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver);

			private:
				struct SendEntry {
					MediaType type = MediaType::kVideo;
					webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
					webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
					webrtc::scoped_refptr<PushVideoSource> videoSource;
					webrtc::scoped_refptr<PushAudioSource> audioSource;
				};

				struct RecvEntry {
					MediaType type = MediaType::kVideo;
					std::shared_ptr<VideoSinkAdapter> videoSink;
					std::shared_ptr<AudioSinkAdapter> audioSink;
					webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
				};

				void DetachRecv(RecvEntry& entry);

				std::shared_ptr<EngineContext> ctx_;
				IPeerObserver* observer_ = nullptr;
				// Guards asynchronous SDP callbacks against destruction of this
				// connection before libwebrtc finishes the operation.
				std::shared_ptr<std::atomic<bool>> alive_;

				// Declared before pc_ so the observer outlives the connection.
				std::unique_ptr<PeerObserver> pcObserver_;
				webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;

				std::mutex mu_;
				std::unordered_map<std::string, SendEntry> send_;
				std::unordered_map<std::string, RecvEntry> recv_;
			};

			// ---- PeerObserver out-of-line bodies ----------------------

			void PeerObserver::OnIceConnectionChange(
				webrtc::PeerConnectionInterface::IceConnectionState newState) {
				if (owner_) owner_->HandleIceState(ToIceState(newState));
			}

			void PeerObserver::OnConnectionChange(
				webrtc::PeerConnectionInterface::PeerConnectionState newState) {
				if (owner_) owner_->HandlePeerState(ToPeerState(newState));
			}

			void PeerObserver::OnIceCandidate(const webrtc::IceCandidate* candidate) {
				if (owner_ && candidate) owner_->HandleIceCandidate(*candidate);
			}

			void PeerObserver::OnTrack(
				webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
				if (owner_) owner_->HandleRemoteTrack(std::move(transceiver));
			}

			void PeerObserver::OnRemoveTrack(
				webrtc::scoped_refptr<webrtc::RtpReceiverInterface>) {
				// Remote track removal is reported through OnConnectionState in
				// this adapter; per-track removal is not used by the terminal.
			}

			// ---- LibPeerConnection ------------------------------------

			LibPeerConnection::LibPeerConnection(
				std::shared_ptr<EngineContext> ctx,
				const PeerConfig& config,
				IPeerObserver* observer)
				: ctx_(std::move(ctx)), observer_(observer),
				  alive_(std::make_shared<std::atomic<bool>>(true)) {
				if (!ctx_ || !ctx_->factory || !observer_) {
					return;
				}

				webrtc::PeerConnectionInterface::RTCConfiguration rtcConfig;
				rtcConfig.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
				for (const IceServer& ice : config.iceServers) {
					webrtc::PeerConnectionInterface::IceServer server;
					server.uri = ice.uri;
					server.username = ice.username;
					server.password = ice.password;
					rtcConfig.servers.push_back(std::move(server));
				}

				pcObserver_.reset(new PeerObserver(this));
				webrtc::PeerConnectionDependencies deps(pcObserver_.get());
				webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::PeerConnectionInterface>>
					result = ctx_->factory->CreatePeerConnectionOrError(
						rtcConfig, std::move(deps));
				if (!result.ok()) {
					observer_->OnError(result.error().message());
					return;
				}
				pc_ = std::move(result.value());
			}

			LibPeerConnection::~LibPeerConnection() {
				if (alive_) alive_->store(false);
				Close();
			}

			bool LibPeerConnection::AddSendStream(const SendStreamConfig& config) {
				if (!pc_ || config.streamId.empty()) return false;

				const std::string trackId = config.trackId.empty()
					? config.streamId : config.trackId;

				SendEntry entry;
				entry.type = config.type;

				webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
				if (config.type == MediaType::kVideo) {
					entry.videoSource = webrtc::make_ref_counted<PushVideoSource>();
					track = ctx_->factory->CreateVideoTrack(
						entry.videoSource, trackId);
				} else {
					entry.audioSource = webrtc::make_ref_counted<PushAudioSource>();
					track = ctx_->factory->CreateAudioTrack(
						trackId, entry.audioSource.get());
				}
				if (!track) {
					if (observer_) observer_->OnError("CreateTrack failed");
					return false;
				}

				webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpSenderInterface>>
					added = pc_->AddTrack(track, { config.streamId });
				if (!added.ok()) {
					if (observer_) observer_->OnError(added.error().message());
					return false;
				}

				entry.track = track;
				entry.sender = std::move(added.value());
				{
					std::lock_guard<std::mutex> lk(mu_);
					send_[config.streamId] = std::move(entry);
				}
				return true;
			}

			void LibPeerConnection::RemoveSendStream(const std::string& streamId) {
				SendEntry entry;
				{
					std::lock_guard<std::mutex> lk(mu_);
					auto it = send_.find(streamId);
					if (it == send_.end()) return;
					entry = std::move(it->second);
					send_.erase(it);
				}
				if (pc_ && entry.sender) {
					pc_->RemoveTrackOrError(entry.sender);
				}
			}

			bool LibPeerConnection::PushVideoFrame(const std::string& streamId,
				const VideoFrameView& frame) {
				webrtc::scoped_refptr<PushVideoSource> source;
				{
					std::lock_guard<std::mutex> lk(mu_);
					auto it = send_.find(streamId);
					if (it == send_.end() ||
						it->second.type != MediaType::kVideo) {
						return false;
					}
					source = it->second.videoSource;
				}
				if (!source) return false;
				source->Push(frame);
				return true;
			}

			bool LibPeerConnection::PushAudioFrame(const std::string& streamId,
				const AudioFrameView& frame) {
				webrtc::scoped_refptr<PushAudioSource> source;
				{
					std::lock_guard<std::mutex> lk(mu_);
					auto it = send_.find(streamId);
					if (it == send_.end() ||
						it->second.type != MediaType::kAudio) {
						return false;
					}
					source = it->second.audioSource;
				}
				if (!source) return false;
				source->Push(frame);
				return true;
			}

			bool LibPeerConnection::AddRecvStream(const RecvStreamConfig& config,
				IVideoSink* videoSink,
				IAudioSink* audioSink) {
				RecvEntry entry;
				entry.type = config.type;
				if (config.type == MediaType::kVideo) {
					if (!videoSink) return false;
					entry.videoSink = std::make_shared<VideoSinkAdapter>(videoSink);
				} else {
					if (!audioSink) return false;
					entry.audioSink = std::make_shared<AudioSinkAdapter>(audioSink);
				}
				std::lock_guard<std::mutex> lk(mu_);
				recv_[config.streamId] = std::move(entry);
				return true;
			}

			void LibPeerConnection::DetachRecv(RecvEntry& entry) {
				if (!entry.track) return;
				if (entry.type == MediaType::kVideo && entry.videoSink) {
					webrtc::VideoTrackInterface* track =
						static_cast<webrtc::VideoTrackInterface*>(entry.track.get());
					track->RemoveSink(entry.videoSink.get());
				} else if (entry.type == MediaType::kAudio && entry.audioSink) {
					webrtc::AudioTrackInterface* track =
						static_cast<webrtc::AudioTrackInterface*>(entry.track.get());
					track->RemoveSink(entry.audioSink.get());
				}
				entry.track = nullptr;
			}

			void LibPeerConnection::RemoveRecvStream(const std::string& streamId) {
				RecvEntry entry;
				{
					std::lock_guard<std::mutex> lk(mu_);
					auto it = recv_.find(streamId);
					if (it == recv_.end()) return;
					entry = std::move(it->second);
					recv_.erase(it);
				}
				DetachRecv(entry);
			}

			void LibPeerConnection::CreateOffer() {
				webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = pc_;
				if (!pc) return;
				std::shared_ptr<std::atomic<bool>> alive = alive_;
				auto observer = webrtc::make_ref_counted<CreateSdpObserver>(
					[this, pc, alive](webrtc::SessionDescriptionInterface* desc) {
						if (!alive->load()) {
							delete desc;
							return;
						}
						std::string sdp;
						desc->ToString(&sdp);
						auto setLocal = webrtc::make_ref_counted<SetLocalObserver>(
							[this, alive](const std::string& e) {
								if (alive->load() && observer_) observer_->OnError(e);
							});
						pc->SetLocalDescription(setLocal.get(), desc);
						if (observer_) {
							observer_->OnLocalDescription(ToSdpType(desc->GetType()), sdp);
						}
					},
					[this, alive](const std::string& e) {
						if (alive->load() && observer_) observer_->OnError(e);
					});
				pc->CreateOffer(observer.get(),
					webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
			}

			void LibPeerConnection::CreateAnswer() {
				webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = pc_;
				if (!pc) return;
				std::shared_ptr<std::atomic<bool>> alive = alive_;
				auto observer = webrtc::make_ref_counted<CreateSdpObserver>(
					[this, pc, alive](webrtc::SessionDescriptionInterface* desc) {
						if (!alive->load()) {
							delete desc;
							return;
						}
						std::string sdp;
						desc->ToString(&sdp);
						auto setLocal = webrtc::make_ref_counted<SetLocalObserver>(
							[this, alive](const std::string& e) {
								if (alive->load() && observer_) observer_->OnError(e);
							});
						pc->SetLocalDescription(setLocal.get(), desc);
						if (observer_) {
							observer_->OnLocalDescription(ToSdpType(desc->GetType()), sdp);
						}
					},
					[this, alive](const std::string& e) {
						if (alive->load() && observer_) observer_->OnError(e);
					});
				pc->CreateAnswer(observer.get(),
					webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
			}

			bool LibPeerConnection::SetRemoteDescription(SdpType type,
				const std::string& sdp) {
				webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = pc_;
				if (!pc || sdp.empty()) return false;

				webrtc::SdpParseError error;
				std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
					webrtc::CreateSessionDescription(ToLibSdpType(type), sdp, &error);
				if (!desc) {
					if (observer_) observer_->OnError(error.description);
					return false;
				}

				auto setRemote = webrtc::make_ref_counted<SetRemoteObserver>(
					[this](const std::string& e) {
						if (observer_) observer_->OnError(e);
					});
				pc->SetRemoteDescription(std::move(desc), setRemote);
				return true;
			}

			bool LibPeerConnection::AddIceCandidate(const std::string& mid,
				int mlineIndex,
				const std::string& candidate) {
				webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = pc_;
				if (!pc || mid.empty() || candidate.empty()) return false;

				webrtc::SdpParseError error;
				std::unique_ptr<webrtc::IceCandidate> ice(
					webrtc::CreateIceCandidate(mid, mlineIndex, candidate, &error));
				if (!ice) {
					if (observer_) observer_->OnError(error.description);
					return false;
				}
				if (!pc->AddIceCandidate(ice.get())) {
					if (observer_) observer_->OnError("AddIceCandidate failed");
					return false;
				}
				return true;
			}

			void LibPeerConnection::Close() {
				std::unordered_map<std::string, RecvEntry> recv;
				{
					std::lock_guard<std::mutex> lk(mu_);
					recv.swap(recv_);
					send_.clear();
				}
				for (auto& kv : recv) {
					DetachRecv(kv.second);
				}

				webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = pc_;
				pc_ = nullptr;
				if (pc) {
					pc->Close();
				}
			}

			// ---- PeerObserver -> LibPeerConnection --------------------

			void LibPeerConnection::HandleIceState(IceConnectionState state) {
				if (observer_) observer_->OnIceConnectionState(state);
			}

			void LibPeerConnection::HandlePeerState(PeerConnectionState state) {
				if (observer_) observer_->OnConnectionState(state);
			}

			void LibPeerConnection::HandleIceCandidate(
				const webrtc::IceCandidate& candidate) {
				if (!observer_) return;
				observer_->OnIceCandidate(candidate.sdp_mid(),
					candidate.sdp_mline_index(),
					candidate.ToString());
			}

			void LibPeerConnection::HandleRemoteTrack(
				webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
				if (!transceiver || !transceiver->receiver()) return;
				webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver =
					transceiver->receiver();
				webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track =
					receiver->track();
				if (!track) return;

				const bool isVideo =
					track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind;
				const MediaType type = isVideo ? MediaType::kVideo : MediaType::kAudio;

				std::string streamId;
				for (const auto& stream : receiver->streams()) {
					if (stream) {
						streamId = stream->id();
						break;
					}
				}

				RecvEntry entry;
				bool found = false;
				{
					std::lock_guard<std::mutex> lk(mu_);
					auto it = recv_.find(streamId);
					if (it == recv_.end()) it = recv_.find(std::string());
					if (it != recv_.end() && it->second.type == type) {
						entry = it->second;
						found = true;
					}
				}
				if (!found) return;

				if (isVideo && entry.videoSink) {
					webrtc::VideoTrackInterface* videoTrack =
						static_cast<webrtc::VideoTrackInterface*>(track.get());
					videoTrack->AddOrUpdateSink(entry.videoSink.get(),
						webrtc::VideoSinkWants());
				} else if (!isVideo && entry.audioSink) {
					webrtc::AudioTrackInterface* audioTrack =
						static_cast<webrtc::AudioTrackInterface*>(track.get());
					audioTrack->AddSink(entry.audioSink.get());
				}
				entry.track = track;
				{
					std::lock_guard<std::mutex> lk(mu_);
					auto it = recv_.find(streamId);
					if (it == recv_.end()) it = recv_.find(std::string());
					if (it != recv_.end() && it->second.type == type) {
						it->second.track = track;
					}
				}

				if (observer_) {
					observer_->OnRemoteStreamAdded(streamId, track->id(), type);
				}
			}

			// ============================================================
			// LibWebRtcEngine
			// ============================================================

			class LibWebRtcEngine : public IWebRtcEngine {
			public:
				~LibWebRtcEngine() override { Shutdown(); }

				bool Initialize(const EngineConfig& config) override {
					if (ctx_) return true;

					webrtc::Environment env =
						webrtc::EnvironmentFactory().Create();
					auto ctx = std::make_shared<EngineContext>(env);

					ctx->signaling_thread =
						webrtc::Thread::CreateWithSocketServer();
					if (!ctx->signaling_thread) {
						return false;
					}
					ctx->signaling_thread->Start();

					webrtc::PeerConnectionFactoryDependencies deps;
					deps.signaling_thread = ctx->signaling_thread.get();
					deps.env = ctx->env;
					if (config.enableAudio) {
						deps.audio_encoder_factory =
							webrtc::CreateBuiltinAudioEncoderFactory();
						deps.audio_decoder_factory =
							webrtc::CreateBuiltinAudioDecoderFactory();
					}
					if (config.enableVideo) {
						deps.video_encoder_factory =
							webrtc::CreateBuiltinVideoEncoderFactory();
						deps.video_decoder_factory =
							webrtc::CreateBuiltinVideoDecoderFactory();
					}
					webrtc::EnableMedia(deps);
					ctx->factory =
						webrtc::CreateModularPeerConnectionFactory(std::move(deps));
					if (!ctx->factory) {
						return false;
					}

					ctx_ = std::move(ctx);
					return true;
				}

				std::unique_ptr<IPeerConnection> CreatePeerConnection(
					const PeerConfig& config,
					IPeerObserver* observer) override {
					if (!ctx_ || !ctx_->factory || !observer) {
						return nullptr;
					}
					return std::unique_ptr<IPeerConnection>(
						new LibPeerConnection(ctx_, config, observer));
				}

				void Shutdown() override {
					ctx_.reset();
				}

			private:
				std::shared_ptr<EngineContext> ctx_;
			};

		} // namespace

		std::unique_ptr<IWebRtcEngine> CreateWebRtcEngine() {
			return std::unique_ptr<IWebRtcEngine>(new LibWebRtcEngine());
		}

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
