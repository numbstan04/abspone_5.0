/**
 * @file LibWebRtcMediaEngine.cpp
 * @brief libwebrtc (m140) media engine: pacer / NACK-ARQ / jitter / PLI / AEC.
 *
 * *** THIS TRANSLATION UNIT IS COMPILED WITH CHROMIUM'S CLANG-CL + LIBC++
 *     TOOLCHAIN ***  libwebrtc m140 is built that way, so its C++ symbols
 *     live in `std::__Cr::` and cannot be linked from MSVC-STL objects.
 *     CMake drives this single file through clang-cl (see
 *     media/src/backend/webrtc/CMakeLists.txt).
 *
 * [What this file provides that the RTP-codec-only path cannot]
 *   - one webrtc::Call per member        -> pacer, BWE, RTCP scheduling
 *   - VideoSendStream (pass-through enc) -> pacing, NACK store, RTX
 *   - VideoReceiveStream (pass-through dec) -> jitter buffer, NACK, PLI
 *   - AudioState + AudioProcessing (AEC3) + AudioSend/ReceiveStream (NetEq)
 *
 * [Pass-through codecs]
 * 5.0 keeps its own H.264 encoder/decoder. We install a VideoEncoder whose
 * Encode() is a no-op (it is never fed raw frames - 5.0 encodes) and whose
 * RegisterEncodeCompleteCallback() hands us the EncodedImageCallback that
 * VideoStreamEncoder installed. SendEncodedFrame() then calls that callback
 * directly with the Annex-B access unit produced by the 5.0 encoder, exactly
 * like 4.5's FakeGenericEncoder::WriteEncodedData().
 *
 * Because m140 defers encoder creation until the first input frame is seen,
 * the engine pushes a single black frame into the VideoSendStream source after
 * creation. That satisfies last_frame_info_, triggers ReconfigureEncoder() and
 * registers our callback; frames sent before that are counted and dropped
 * (same behaviour as 4.5).
 *
 * [Audio]
 * Each member owns a custom AudioDeviceModule bridged to 5.0's PCM pipeline:
 * PushCapturedAudio() resamples/buffers capture PCM into 10 ms frames and feeds
 * AudioTransport::RecordedDataIsAvailable(); the playout thread pulls mixed
 * audio through AudioTransport::NeedMorePlayData() and hands it to
 * IWebRtcAudioSink. AudioProcessing therefore sees both directions and AEC3
 * works exactly as in 4.5.
 */

#include "webrtc/IWebRtcMediaEngine.h"

#ifdef VCS_WITH_WEBRTC

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/audio/audio_device_defines.h"
#include "api/audio/audio_mixer.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/call/transport.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/media_types.h"
#include "api/rtp_parameters.h"
#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "call/audio_receive_stream.h"
#include "call/audio_send_stream.h"
#include "call/audio_state.h"
#include "call/call.h"
#include "call/call_config.h"
#include "call/video_receive_stream.h"
#include "call/video_send_stream.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "common_audio/resampler/push_sinc_resampler.h"
#include "modules/audio_device/include/audio_device_default.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "video/config/video_encoder_config.h"

namespace vcs {
	namespace webrtc_engine {

		namespace {

			constexpr int kAdmSampleRate = 16000;
			constexpr int kAdmChannels = 1;
			constexpr int kAdmFrameMs = 10;
			constexpr int kAdmSamplesPerFrame = kAdmSampleRate * kAdmFrameMs / 1000;

			// ------------------------------------------------------------------
			// SDP helpers
			// ------------------------------------------------------------------

			webrtc::SdpAudioFormat AudioFormatForPayloadType(int pt) {
				switch (pt) {
				case 8:  return webrtc::SdpAudioFormat("PCMA", 8000, 1);
				case 9:  return webrtc::SdpAudioFormat("G722", 8000, 1);
				case 0:
				default: return webrtc::SdpAudioFormat("PCMU", 8000, 1);
				}
			}

			// ------------------------------------------------------------------
			// Pass-through video encoder
			// ------------------------------------------------------------------

			/// Shared between the VideoStreamEncoder-owned VideoEncoder and the
			/// media engine. `callback` is written on the encoder thread and
			/// read on the 5.0 encode thread.
			struct EncoderBridge {
				std::mutex mu;
				webrtc::EncodedImageCallback* callback = nullptr;
				std::atomic<uint64_t> dropped{ 0 };
			};

			class PassThroughVideoEncoder : public webrtc::VideoEncoder {
			public:
				explicit PassThroughVideoEncoder(std::shared_ptr<EncoderBridge> bridge)
					: bridge_(std::move(bridge)) {
				}

				int InitEncode(const webrtc::VideoCodec* /*codec_settings*/,
					const webrtc::VideoEncoder::Settings& /*settings*/) override {
					// 5.0 owns the real encoder; nothing to initialise here.
					return 0;
				}

				int32_t RegisterEncodeCompleteCallback(
					webrtc::EncodedImageCallback* callback) override {
					std::lock_guard<std::mutex> lock(bridge_->mu);
					bridge_->callback = callback;
					return 0;
				}

				int32_t Release() override {
					std::lock_guard<std::mutex> lock(bridge_->mu);
					bridge_->callback = nullptr;
					return 0;
				}

				int32_t Encode(const webrtc::VideoFrame& /*frame*/,
					const std::vector<webrtc::VideoFrameType>* /*frame_types*/) override {
					// Raw frames never enter the engine: 5.0 encodes before
					// calling SendEncodedFrame(). Returning OK keeps the
					// VideoStreamEncoder state machine happy.
					return 0;
				}

				void SetRates(const RateControlParameters& /*parameters*/) override {
				}

				EncoderInfo GetEncoderInfo() const override {
					EncoderInfo info;
					info.implementation_name = "vcs-passthrough";
					// 5.0's encoder does its own rate control; disable the
					// media-optimization frame dropper (we push frames anyway).
					info.has_trusted_rate_controller = true;
					info.is_hardware_accelerated = false;
					info.supports_native_handle = false;
					info.is_qp_trusted = false;
					return info;
				}

			private:
				std::shared_ptr<EncoderBridge> bridge_;
			};

			class PassThroughVideoEncoderFactory : public webrtc::VideoEncoderFactory {
			public:
				explicit PassThroughVideoEncoderFactory(std::shared_ptr<EncoderBridge> bridge)
					: bridge_(std::move(bridge)) {
				}

				std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
					return { webrtc::SdpVideoFormat("H264") };
				}

				std::unique_ptr<webrtc::VideoEncoder> Create(
					const webrtc::Environment& /*env*/,
					const webrtc::SdpVideoFormat& /*format*/) override {
					return std::make_unique<PassThroughVideoEncoder>(bridge_);
				}

			private:
				std::shared_ptr<EncoderBridge> bridge_;
			};

			// ------------------------------------------------------------------
			// Pass-through video decoder
			// ------------------------------------------------------------------

			class NullVideoRenderer : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
			public:
				void OnFrame(const webrtc::VideoFrame& /*frame*/) override {
				}
			};

			class PassThroughVideoDecoder : public webrtc::VideoDecoder {
			public:
				PassThroughVideoDecoder(std::string memberId, int streamKind,
					IWebRtcEncodedFrameSink* sink, uint32_t fallbackSsrc)
					: memberId_(std::move(memberId)), streamKind_(streamKind),
					sink_(sink), fallbackSsrc_(fallbackSsrc) {
				}

				bool Configure(const Settings& /*settings*/) override {
					return true;
				}

				int32_t Decode(const webrtc::EncodedImage& input,
					int64_t /*render_time_ms*/) override {
					if (!sink_ || !input.data() || input.size() == 0) {
						return 0;
					}
					uint32_t ssrc = fallbackSsrc_;
					const webrtc::RtpPacketInfos& infos = input.PacketInfos();
					if (!infos.empty()) {
						ssrc = infos[0].ssrc();
					}
					sink_->OnEncodedFrame(memberId_.c_str(), streamKind_,
						input.data(), input.size(),
						input.FrameType() == webrtc::VideoFrameType::kVideoFrameKey,
						input.RtpTimestamp(), ssrc);
					// Frames are decoded by 5.0; no Decoded() callback.
					return 0;
				}

				int32_t RegisterDecodeCompleteCallback(
					webrtc::DecodedImageCallback* callback) override {
					decoded_cb_ = callback;
					return 0;
				}

				int32_t Release() override {
					return 0;
				}

				DecoderInfo GetDecoderInfo() const override {
					DecoderInfo info;
					info.implementation_name = "vcs-passthrough";
					info.is_hardware_accelerated = false;
					return info;
				}

			private:
				std::string memberId_;
				int streamKind_;
				IWebRtcEncodedFrameSink* sink_ = nullptr;
				uint32_t fallbackSsrc_ = 0;
				webrtc::DecodedImageCallback* decoded_cb_ = nullptr;
			};

			class PassThroughVideoDecoderFactory : public webrtc::VideoDecoderFactory {
			public:
				PassThroughVideoDecoderFactory(std::string memberId, int streamKind,
					IWebRtcEncodedFrameSink* sink, uint32_t fallbackSsrc)
					: memberId_(std::move(memberId)), streamKind_(streamKind),
					sink_(sink), fallbackSsrc_(fallbackSsrc) {
				}

				std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
					return { webrtc::SdpVideoFormat("H264") };
				}

				std::unique_ptr<webrtc::VideoDecoder> Create(
					const webrtc::Environment& /*env*/,
					const webrtc::SdpVideoFormat& /*format*/) override {
					return std::make_unique<PassThroughVideoDecoder>(
						memberId_, streamKind_, sink_, fallbackSsrc_);
				}

			private:
				std::string memberId_;
				int streamKind_;
				IWebRtcEncodedFrameSink* sink_ = nullptr;
				uint32_t fallbackSsrc_ = 0;
			};

			// ------------------------------------------------------------------
			// Egress transport (webrtc -> gateway IRtpTransport)
			// ------------------------------------------------------------------

			class CallTransport : public webrtc::Transport {
			public:
				CallTransport(IWebRtcPacketSink* sink, std::string memberId, int streamKind)
					: sink_(sink), memberId_(std::move(memberId)), streamKind_(streamKind) {
				}

				bool SendRtp(webrtc::ArrayView<const uint8_t> packet,
					const webrtc::PacketOptions& /*options*/) override {
					if (!sink_) return false;
					return sink_->OnSendRtp(memberId_.c_str(), streamKind_,
						packet.data(), packet.size());
				}

				bool SendRtcp(webrtc::ArrayView<const uint8_t> packet,
					const webrtc::PacketOptions& /*options*/) override {
					if (!sink_) return false;
					return sink_->OnSendRtcp(memberId_.c_str(), streamKind_,
						packet.data(), packet.size());
				}

			private:
				IWebRtcPacketSink* sink_;
				std::string memberId_;
				int streamKind_;
			};

			// ------------------------------------------------------------------
			// Push video source (only used to bootstrap encoder creation)
			// ------------------------------------------------------------------

			class BootstrapVideoSource
				: public webrtc::VideoSourceInterface<webrtc::VideoFrame> {
			public:
				void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
					const webrtc::VideoSinkWants& /*wants*/) override {
					bool deliver = false;
					int width = 0;
					int height = 0;
					{
						std::lock_guard<std::mutex> lock(mu_);
						sink_ = sink;
						if (sink && pending_) {
							pending_ = false;
							deliver = true;
							width = pending_width_;
							height = pending_height_;
						}
					}
					// SetSource() may install the sink asynchronously; if the
					// bootstrap request arrived first, deliver it now.
					if (deliver) Deliver(sink, width, height);
				}

				void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
					std::lock_guard<std::mutex> lock(mu_);
					if (sink_ == sink) sink_ = nullptr;
				}

				/// Deliver one black I420 frame so VideoStreamEncoder learns the
				/// input resolution and creates the (pass-through) encoder.
				void PushBootstrapFrame(int width, int height) {
					webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink = nullptr;
					{
						std::lock_guard<std::mutex> lock(mu_);
						if (!sink_) {
							pending_ = true;
							pending_width_ = width;
							pending_height_ = height;
							return;
						}
						sink = sink_;
					}
					Deliver(sink, width, height);
				}

			private:
				static void Deliver(
					webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
					int width, int height) {
					if (!sink) return;
					webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
						webrtc::I420Buffer::Create(width, height);
					webrtc::I420Buffer::SetBlack(buffer.get());
					webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
						.set_video_frame_buffer(buffer)
						.set_timestamp_us(webrtc::TimeMicros())
						.build();
					sink->OnFrame(frame);
				}

				std::mutex mu_;
				webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink_ = nullptr;
				bool pending_ = false;
				int pending_width_ = 640;
				int pending_height_ = 360;
			};

			// ------------------------------------------------------------------
			// Bridge AudioDeviceModule (5.0 PCM <-> webrtc ADM)
			// ------------------------------------------------------------------

			class BridgeAudioDeviceModule
				: public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {
			public:
				BridgeAudioDeviceModule(std::string memberId, IWebRtcAudioSink* sink)
					: memberId_(std::move(memberId)), sink_(sink),
					capture_resampler_(nullptr) {
				}

				~BridgeAudioDeviceModule() override {
					Terminate();
				}

				int32_t RegisterAudioCallback(webrtc::AudioTransport* callback) override {
					std::lock_guard<std::mutex> lock(callback_mu_);
					transport_ = callback;
					return 0;
				}

				int32_t Init() override {
					initialized_ = true;
					return 0;
				}

				int32_t Terminate() override {
					StopRecording();
					StopPlayout();
					initialized_ = false;
					return 0;
				}

				bool Initialized() const override { return initialized_; }

				int32_t InitPlayout() override { return 0; }
				bool PlayoutIsInitialized() const override { return true; }

				int32_t StartPlayout() override {
					if (playing_.exchange(true)) return 0;
					playout_thread_ = std::thread([this]() { PlayoutLoop(); });
					return 0;
				}

				int32_t StopPlayout() override {
					if (!playing_.exchange(false)) return 0;
					if (playout_thread_.joinable()) playout_thread_.join();
					return 0;
				}

				bool Playing() const override { return playing_.load(); }

				int32_t InitRecording() override { return 0; }
				bool RecordingIsInitialized() const override { return true; }

				int32_t StartRecording() override {
					if (recording_.exchange(true)) return 0;
					capture_thread_ = std::thread([this]() { CaptureLoop(); });
					return 0;
				}

				int32_t StopRecording() override {
					if (!recording_.exchange(false)) return 0;
					capture_cv_.notify_all();
					if (capture_thread_.joinable()) capture_thread_.join();
					return 0;
				}

				bool Recording() const override { return recording_.load(); }

				int32_t PlayoutDelay(uint16_t* delay_ms) const override {
					if (delay_ms) *delay_ms = kAdmFrameMs;
					return 0;
				}

				/// Called from the 5.0 capture thread. Any rate / channel layout
				/// is accepted; it is sliced into 10 ms mono frames at
				/// kAdmSampleRate and buffered for the capture thread.
				void PushCapture(const int16_t* pcm, size_t samples_per_channel,
					int sample_rate, int channels) {
					if (!pcm || samples_per_channel == 0 || sample_rate <= 0) return;
					const size_t src_per_10ms = static_cast<size_t>(sample_rate / 100);
					if (src_per_10ms == 0) return;

					std::lock_guard<std::mutex> lock(capture_mu_);
					for (size_t offset = 0; offset + src_per_10ms <= samples_per_channel;
						offset += src_per_10ms) {
						AppendResampledLocked(pcm + offset * channels,
							src_per_10ms, sample_rate, channels);
					}
					capture_cv_.notify_all();
				}

			private:
				void AppendResampledLocked(const int16_t* src, size_t src_samples,
					int sample_rate, int channels) {
					if (sample_rate == kAdmSampleRate && channels == 1) {
						capture_ring_.insert(capture_ring_.end(), src, src + src_samples);
						return;
					}
					if (!capture_resampler_ ||
						capture_resampler_rate_ != sample_rate) {
						capture_resampler_ = std::make_unique<webrtc::PushSincResampler>(
							src_samples, static_cast<size_t>(kAdmSamplesPerFrame));
						capture_resampler_rate_ = sample_rate;
					}
					std::vector<int16_t> mono(src_samples);
					for (size_t i = 0; i < src_samples; ++i) {
						mono[i] = src[i * channels];
					}
					int16_t out[kAdmSamplesPerFrame];
					size_t produced = capture_resampler_->Resample(
						mono.data(), mono.size(), out, kAdmSamplesPerFrame);
					capture_ring_.insert(capture_ring_.end(), out, out + produced);
				}

				void CaptureLoop() {
					const auto frame_duration =
						std::chrono::milliseconds(kAdmFrameMs);
					auto next = std::chrono::steady_clock::now();
					int16_t frame[kAdmSamplesPerFrame] = { 0 };
					uint32_t mic_level = 0;
					while (recording_.load()) {
						bool have_frame = false;
						{
							std::unique_lock<std::mutex> lock(capture_mu_);
							capture_cv_.wait_until(lock, next, [this]() {
								return !recording_.load() ||
									capture_ring_.size() >=
									static_cast<size_t>(kAdmSamplesPerFrame);
								});
							if (!recording_.load()) break;
							if (capture_ring_.size() >=
								static_cast<size_t>(kAdmSamplesPerFrame)) {
								std::copy_n(capture_ring_.begin(), kAdmSamplesPerFrame,
									frame);
								capture_ring_.erase(
									capture_ring_.begin(),
									capture_ring_.begin() + kAdmSamplesPerFrame);
								have_frame = true;
							}
						}
						if (!have_frame) {
							std::memset(frame, 0, sizeof(frame));
						}

						webrtc::AudioTransport* transport = nullptr;
						{
							std::lock_guard<std::mutex> lock(callback_mu_);
							transport = transport_;
						}
						uint32_t new_mic_level = mic_level;
						if (transport) {
							transport->RecordedDataIsAvailable(frame, kAdmSamplesPerFrame,
								sizeof(int16_t), kAdmChannels, kAdmSampleRate,
								kAdmFrameMs, 0, mic_level, false, new_mic_level);
							mic_level = new_mic_level;
						}

						next += frame_duration;
						const auto now = std::chrono::steady_clock::now();
						if (next < now) next = now;
					}
				}

				void PlayoutLoop() {
					const auto frame_duration =
						std::chrono::milliseconds(kAdmFrameMs);
					auto next = std::chrono::steady_clock::now();
					std::vector<int16_t> frame(kAdmSamplesPerFrame * kAdmChannels);
					while (playing_.load()) {
						webrtc::AudioTransport* transport = nullptr;
						{
							std::lock_guard<std::mutex> lock(callback_mu_);
							transport = transport_;
						}
						size_t out_samples = 0;
						int64_t elapsed_ms = 0;
						int64_t ntp_ms = 0;
						if (transport) {
							transport->NeedMorePlayData(kAdmSamplesPerFrame,
								sizeof(int16_t), kAdmChannels, kAdmSampleRate,
								frame.data(), out_samples, &elapsed_ms, &ntp_ms);
						}
						if (sink_ && out_samples > 0) {
							sink_->OnPlayoutAudio(memberId_.c_str(), frame.data(),
								out_samples, kAdmSampleRate, kAdmChannels);
						}

						next += frame_duration;
						std::this_thread::sleep_until(next);
						const auto now = std::chrono::steady_clock::now();
						if (next < now) next = now;
					}
				}

				std::string memberId_;
				IWebRtcAudioSink* sink_ = nullptr;

				std::mutex callback_mu_;
				webrtc::AudioTransport* transport_ = nullptr;

				std::atomic<bool> initialized_{ false };
				std::atomic<bool> playing_{ false };
				std::atomic<bool> recording_{ false };

				std::thread playout_thread_;
				std::thread capture_thread_;

				std::mutex capture_mu_;
				std::condition_variable capture_cv_;
				std::deque<int16_t> capture_ring_;
				std::unique_ptr<webrtc::PushSincResampler> capture_resampler_;
				int capture_resampler_rate_ = 0;
			};

			// ------------------------------------------------------------------
			// Per-member engine state
			// ------------------------------------------------------------------

			struct MemberState {
				explicit MemberState(std::string id, const MediaEngineMemberConfig& c)
					: memberId(std::move(id)), cfg(c) {
				}

				~MemberState() {
					// The media thread owns destruction; see RemoveMember().
				}

				std::string memberId;
				MediaEngineMemberConfig cfg;

				IWebRtcPacketSink* packetSink = nullptr;
				IWebRtcEncodedFrameSink* frameSink = nullptr;

				std::unique_ptr<webrtc::Call> call;

				// video
				std::shared_ptr<EncoderBridge> encoderBridge;
				std::unique_ptr<PassThroughVideoEncoderFactory> encoderFactory;
				std::unique_ptr<webrtc::VideoBitrateAllocatorFactory>
					bitrateAllocatorFactory;
				std::unique_ptr<PassThroughVideoDecoderFactory> decoderFactory;
				std::unique_ptr<BootstrapVideoSource> videoSource;
				NullVideoRenderer nullRenderer;
				std::unique_ptr<CallTransport> videoTransport;
				webrtc::VideoSendStream* videoSend = nullptr;
				webrtc::VideoReceiveStreamInterface* videoRecv = nullptr;
				uint32_t videoSendSsrc = 0;
				int videoSendPt = -1;
				uint32_t videoRecvSsrc = 0;
				int videoRecvPt = -1;
				bool videoSendFailed = false;
				bool bootstrapped = false;

				// audio
				webrtc::scoped_refptr<BridgeAudioDeviceModule> adm;
				webrtc::scoped_refptr<webrtc::AudioState> audioState;
				webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
				webrtc::scoped_refptr<webrtc::AudioMixer> mixer;
				webrtc::scoped_refptr<webrtc::AudioEncoderFactory> audioEncoderFactory;
				webrtc::scoped_refptr<webrtc::AudioDecoderFactory> audioDecoderFactory;
				std::unique_ptr<CallTransport> audioTransport;
				webrtc::AudioSendStream* audioSend = nullptr;
				webrtc::AudioReceiveStreamInterface* audioRecv = nullptr;
				uint32_t audioSendSsrc = 0;
				int audioSendPt = -1;
				/// Payload type the running AudioSendStream was built with, so a
				/// renegotiation can rebuild it instead of silently keeping the
				/// old codec.
				int audioSendActivePt = -1;
				uint32_t audioRecvSsrc = 0;
				int audioRecvPt = -1;
				bool audioStarted = false;
			};

			// ------------------------------------------------------------------
			// Engine
			// ------------------------------------------------------------------

			class LibWebRtcMediaEngine : public IWebRtcMediaEngine {
			public:
				LibWebRtcMediaEngine()
					: env_(webrtc::EnvironmentFactory().Create()) {
					media_thread_ = webrtc::Thread::CreateWithSocketServer();
					if (media_thread_) {
						media_thread_->Start();
					}
				}

				~LibWebRtcMediaEngine() override {
					std::vector<std::shared_ptr<MemberState>> members;
					{
						std::lock_guard<std::mutex> lock(members_mu_);
						for (auto& kv : members_) members.push_back(kv.second);
						members_.clear();
					}
					for (auto& m : members) {
						DestroyMemberOnMediaThread(*m);
					}
					if (media_thread_) {
						media_thread_->Stop();
						media_thread_.reset();
					}
				}

				// ---- IWebRtcMediaEngine -----------------------------------------

				bool Initialize(IWebRtcPacketSink* packetSink,
					IWebRtcEncodedFrameSink* frameSink,
					IWebRtcAudioSink* audioSink) override {
					packet_sink_ = packetSink;
					frame_sink_ = frameSink;
					audio_sink_ = audioSink;
					return media_thread_ != nullptr;
				}

				bool AddMember(const char* memberId,
					const MediaEngineMemberConfig& config) override {
					if (!memberId || !*memberId || !media_thread_) return false;
					std::string id(memberId);
					std::shared_ptr<MemberState> member;
					{
						std::lock_guard<std::mutex> lock(members_mu_);
						auto& slot = members_[id];
						if (!slot) {
							slot = std::make_shared<MemberState>(id, config);
						} else {
							slot->cfg = config;
						}
						member = slot;
					}
					// Seed the stream parameters from the initial config. Without
					// this, audioSendPt stays -1 and EnsureAudioSend() always
					// bails out -> no AudioSendStream -> captured PCM never hits
					// the wire.
					if (config.audioSendSsrc) member->audioSendSsrc = config.audioSendSsrc;
					if (config.audioSendPayloadType >= 0) {
						member->audioSendPt = config.audioSendPayloadType;
					}
					if (config.videoSendSsrc) member->videoSendSsrc = config.videoSendSsrc;
					if (config.videoSendPayloadType >= 0) {
						member->videoSendPt = config.videoSendPayloadType;
					}

					member->packetSink = packet_sink_;
					member->frameSink = frame_sink_;
					return RunOnMediaThread([this, member]() -> bool {
						return EnsureCall(*member);
						});
				}

				void RemoveMember(const char* memberId) override {
					if (!memberId) return;
					std::shared_ptr<MemberState> member;
					{
						std::lock_guard<std::mutex> lock(members_mu_);
						auto it = members_.find(memberId);
						if (it == members_.end()) return;
						member = it->second;
						members_.erase(it);
					}
					DestroyMemberOnMediaThread(*member);
				}

				void UpdateMemberSsrc(const char* memberId,
					uint32_t audioRecvSsrc, uint32_t videoRecvSsrc) override {
					auto member = FindMember(memberId);
					if (!member) return;
					if (audioRecvSsrc) member->cfg.audioRecvSsrc = audioRecvSsrc;
					if (videoRecvSsrc) member->cfg.videoRecvSsrc = videoRecvSsrc;
				}

				void UpdateMemberSendSsrc(const char* memberId,
					uint32_t audioSendSsrc, uint32_t videoSendSsrc) override {
					auto member = FindMember(memberId);
					if (!member) return;
					if (audioSendSsrc) member->audioSendSsrc = audioSendSsrc;
					if (videoSendSsrc) member->videoSendSsrc = videoSendSsrc;
					RunOnMediaThreadVoid([this, member]() {
						if (EnsureCall(*member)) EnsureAudioSend(*member);
						});
				}

				void UpdateMemberPayloadTypes(const char* memberId,
					int32_t audioPayloadType, int32_t videoPayloadType) override {
					auto member = FindMember(memberId);
					if (!member) return;
					if (audioPayloadType >= 0) {
						member->cfg.audioSendPayloadType = audioPayloadType;
						member->audioSendPt = audioPayloadType;
					}
					if (videoPayloadType >= 0) {
						member->cfg.videoSendPayloadType = videoPayloadType;
						member->videoSendPt = videoPayloadType;
					}
					// The PT usually arrives after the SSRC, so the earlier
					// EnsureAudioSend() attempt would have been skipped.
					RunOnMediaThreadVoid([this, member]() {
						if (EnsureCall(*member)) EnsureAudioSend(*member);
						});
				}

				bool SendEncodedFrame(const char* memberId, int streamKind,
					const uint8_t* data, size_t size, bool isKeyFrame,
					uint32_t rtpTimestamp, uint32_t ssrc,
					int32_t payloadType, int width, int height) override {
					auto member = FindMember(memberId);
					if (!member) return false;
					if (streamKind != kMediaStreamVideo) {
						// H.239 secondary video stays on the native path.
						return false;
					}
					if (!data || size == 0) return false;

					if (ssrc != 0) member->videoSendSsrc = ssrc;
					if (payloadType >= 0) member->videoSendPt = payloadType;

					if (!member->videoSend) {
						if (!RunOnMediaThread([this, member, width, height]() -> bool {
							return EnsureVideoSend(*member, width, height);
							})) {
							return false;
						}
					}

					webrtc::EncodedImageCallback* callback = nullptr;
					{
						std::lock_guard<std::mutex> lock(member->encoderBridge->mu);
						callback = member->encoderBridge->callback;
					}
					if (!callback && isKeyFrame) {
						// m140 initialises the encoder asynchronously on its own
						// queue. Blocking a key frame for a few milliseconds is
						// much cheaper than waiting for the next IDR interval.
						for (int i = 0; i < 50 && !callback; ++i) {
							std::this_thread::sleep_for(std::chrono::milliseconds(1));
							std::lock_guard<std::mutex> lock(member->encoderBridge->mu);
							callback = member->encoderBridge->callback;
						}
					}
					if (!callback) {
						member->encoderBridge->dropped.fetch_add(1);
						return false;
					}

					webrtc::EncodedImage image;
					image.SetEncodedData(webrtc::EncodedImageBuffer::Create(data, size));
					image.SetRtpTimestamp(rtpTimestamp);
					image._frameType = isKeyFrame
						? webrtc::VideoFrameType::kVideoFrameKey
						: webrtc::VideoFrameType::kVideoFrameDelta;
					image._encodedWidth = width > 0
						? static_cast<uint32_t>(width) : 640u;
					image._encodedHeight = height > 0
						? static_cast<uint32_t>(height) : 360u;
					image.SetSpatialIndex(0);

					webrtc::CodecSpecificInfo codec_info;
					codec_info.codecType = webrtc::kVideoCodecH264;
					codec_info.codecSpecific.H264.packetization_mode =
						webrtc::H264PacketizationMode::NonInterleaved;

					webrtc::EncodedImageCallback::Result result =
						callback->OnEncodedImage(image, &codec_info);
					return result.error == webrtc::EncodedImageCallback::Result::OK;
				}

				void OnRtpPacket(const char* memberId, int streamKind,
					const uint8_t* data, size_t size) override {
					auto member = FindMember(memberId);
					if (!member || !data || size < 12) return;

					// Copy once: the gateway buffer is only valid during the call.
					std::vector<uint8_t> copy(data, data + size);
					RunOnMediaThreadVoid([this, member, streamKind, copy = std::move(copy)]() {
						if (streamKind == kMediaStreamAudio) {
							DeliverAudioRtp(*member, copy.data(), copy.size());
						} else {
							DeliverVideoRtp(*member, copy.data(), copy.size(), streamKind);
						}
						});
				}

				void OnRtcpPacket(const char* memberId, int streamKind,
					const uint8_t* data, size_t size) override {
					auto member = FindMember(memberId);
					if (!member || !data || size < 4) return;
					std::vector<uint8_t> copy(data, data + size);
					RunOnMediaThreadVoid([this, member, copy = std::move(copy)]() {
						if (!member->call) return;
						webrtc::CopyOnWriteBuffer buffer(copy.data(), copy.size());
						member->call->Receiver()->DeliverRtcpPacket(std::move(buffer));
						});
				}

				bool PushCapturedAudio(const char* memberId,
					const int16_t* pcm, size_t samplesPerChannel,
					int sampleRate, int channels) override {
					auto member = FindMember(memberId);
					if (!member) return false;
					RunOnMediaThreadVoid([this, member]() {
						if (EnsureCall(*member)) EnsureAudioSend(*member);
						});
					if (!member->adm) return false;
					member->adm->PushCapture(pcm, samplesPerChannel, sampleRate,
						channels > 0 ? channels : 1);
					return true;
				}

				void SetSendBitrate(const char* memberId,
					uint32_t bitrateBps) override {
					auto member = FindMember(memberId);
					if (!member) return;
					RunOnMediaThreadVoid([this, member, bitrateBps]() {
						if (!member->videoSend || bitrateBps == 0) return;
						webrtc::BitrateSettings settings;
						settings.start_bitrate_bps = static_cast<int>(bitrateBps);
						settings.max_bitrate_bps = static_cast<int>(bitrateBps);
						member->call->SetClientBitratePreferences(settings);
						});
				}

				bool GetStats(const char* memberId, MediaEngineStats* out) override {
					auto member = FindMember(memberId);
					if (!member || !out) return false;
					return RunOnMediaThread([this, member, out]() -> bool {
						if (!member->call) return false;
						webrtc::Call::Stats call_stats = member->call->GetStats();
						out->sendBitrateBps =
							static_cast<uint32_t>(std::max(0, call_stats.send_bandwidth_bps));
						out->pacerDelayMs = call_stats.pacer_delay_ms;
						out->roundTripMs = call_stats.rtt_ms > 0
							? static_cast<uint32_t>(call_stats.rtt_ms) : 0;
						if (member->videoSend) {
							webrtc::VideoSendStream::Stats vs = member->videoSend->GetStats();
							out->framesEncoded = vs.frames_encoded;
							for (const auto& kv : vs.substreams) {
								out->packetsSent += kv.second.rtp_stats.transmitted.packets;
								out->nacksReceived += kv.second.rtcp_packet_type_counts.nack_packets;
							}
						}
						if (member->videoRecv) {
							webrtc::VideoReceiveStreamInterface::Stats rs =
								member->videoRecv->GetStats();
							out->packetsReceived = rs.rtp_stats.packet_counter.packets;
							out->packetsLost = rs.rtp_stats.packets_lost > 0
								? static_cast<uint32_t>(rs.rtp_stats.packets_lost) : 0;
							out->jitterMs = static_cast<uint32_t>(rs.jitter_buffer_ms);
							out->framesDecoded = rs.frames_decoded;
							out->nacksSent = rs.rtcp_packet_type_counts.nack_packets;
							out->plisSent = rs.rtcp_packet_type_counts.pli_packets;
						}
						return true;
						});
				}

			private:
				// ---- threading ---------------------------------------------------

				template <typename F>
				bool RunOnMediaThread(F&& fn) {
					if (!media_thread_) return false;
					if (media_thread_->IsCurrent()) {
						return static_cast<bool>(fn());
					}
					bool ok = false;
					media_thread_->BlockingCall([&]() { ok = static_cast<bool>(fn()); });
					return ok;
				}

				template <typename F>
				void RunOnMediaThreadVoid(F&& fn) {
					if (!media_thread_) return;
					if (media_thread_->IsCurrent()) {
						fn();
						return;
					}
					media_thread_->BlockingCall([&]() { fn(); });
				}

				std::shared_ptr<MemberState> FindMember(const char* memberId) {
					if (!memberId) return nullptr;
					std::lock_guard<std::mutex> lock(members_mu_);
					auto it = members_.find(memberId);
					return it == members_.end() ? nullptr : it->second;
				}

				// ---- member lifecycle -------------------------------------------

				bool EnsureCall(MemberState& m) {
					if (m.call) return true;

					webrtc::CallConfig config(env_, media_thread_.get());
					config.bitrate_config.min_bitrate_bps = m.cfg.minBitrateBps;
					config.bitrate_config.start_bitrate_bps =
						m.cfg.localBitrateBps > 0
						? static_cast<int>(m.cfg.localBitrateBps)
						: m.cfg.startBitrateBps;
					config.bitrate_config.max_bitrate_bps = m.cfg.maxBitrateBps;

					webrtc::scoped_refptr<webrtc::AudioMixer> mixer;
					webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
					webrtc::scoped_refptr<BridgeAudioDeviceModule> adm;
					webrtc::scoped_refptr<webrtc::AudioState> audio_state;

					if (m.cfg.enableAudio) {
						webrtc::AudioProcessing::Config apm_config;
						apm_config.echo_canceller.enabled = true;
						apm_config.echo_canceller.mobile_mode = false;
						apm_config.echo_canceller.enforce_high_pass_filtering = true;
						apm_config.noise_suppression.enabled = true;
						apm_config.gain_controller1.enabled = true;
						apm_config.gain_controller1.mode =
							webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
						apm = webrtc::BuiltinAudioProcessingBuilder(apm_config).Build(env_);
						mixer = webrtc::AudioMixerImpl::Create();
						if (apm && mixer) {
							adm = webrtc::make_ref_counted<BridgeAudioDeviceModule>(
								m.memberId, audio_sink_);
							adm->Init();
							webrtc::AudioState::Config state_config;
							state_config.audio_mixer = mixer;
							state_config.audio_processing = apm;
							state_config.audio_device_module = adm;
							audio_state = webrtc::AudioState::Create(state_config);
							if (audio_state) {
								adm->RegisterAudioCallback(audio_state->audio_transport());
							}
						}
					}

					if (audio_state) {
						config.audio_state = audio_state;
						config.audio_processing = apm.get();
					}

					m.mixer = mixer;
					m.apm = apm;
					m.adm = adm;
					m.audioState = audio_state;

					m.call = webrtc::Call::Create(std::move(config));
					if (!m.call) {
						if (m.adm) {
							m.adm->Terminate();
							m.adm = nullptr;
						}
						m.audioState = nullptr;
						return false;
					}
					m.call->SignalChannelNetworkState(webrtc::MediaType::AUDIO,
						webrtc::kNetworkUp);
					m.call->SignalChannelNetworkState(webrtc::MediaType::VIDEO,
						webrtc::kNetworkUp);
					return true;
				}

				bool EnsureVideoSend(MemberState& m, int width, int height) {
					if (m.videoSend) return true;
					if (m.videoSendFailed) return false;
					if (!EnsureCall(m)) return false;
					if (m.videoSendSsrc == 0 || m.videoSendPt < 0) return false;

					// The send and receive streams share one CallTransport, and the
					// receive stream keeps a raw pointer to it inside webrtc's
					// VideoReceiveStream2::transport_adapter_. Replacing it here
					// would free the object the receive stream still points at, so
					// only create it when it does not exist yet (EnsureVideoRecv
					// uses the same guard).
					if (!m.videoTransport) {
						m.videoTransport = std::make_unique<CallTransport>(
							m.packetSink, m.memberId, kMediaStreamVideo);
					}
					m.encoderBridge = std::make_shared<EncoderBridge>();
					m.encoderFactory =
						std::make_unique<PassThroughVideoEncoderFactory>(m.encoderBridge);
					// VideoStreamEncoder::ReconfigureEncoder() dereferences this.
					m.bitrateAllocatorFactory =
						webrtc::CreateBuiltinVideoBitrateAllocatorFactory();
					m.videoSource = std::make_unique<BootstrapVideoSource>();

					webrtc::VideoSendStream::Config send_config(m.videoTransport.get());
					send_config.rtp.ssrcs = { m.videoSendSsrc };
					send_config.rtp.payload_name = "H264";
					send_config.rtp.payload_type = m.videoSendPt;
					send_config.rtp.rtcp_mode = webrtc::RtcpMode::kCompound;
					send_config.rtp.nack.rtp_history_ms = m.cfg.nackHistoryMs;
					send_config.rtp.max_packet_size =
						static_cast<size_t>(m.cfg.maxPacketSize > 0
							? m.cfg.maxPacketSize : 1200);
					if (m.cfg.videoSendRtxPayloadType >= 0 &&
						m.cfg.videoSendRtxSsrc != 0) {
						send_config.rtp.rtx.ssrcs = { m.cfg.videoSendRtxSsrc };
						send_config.rtp.rtx.payload_type =
							m.cfg.videoSendRtxPayloadType;
					}
					send_config.encoder_settings.encoder_factory =
						m.encoderFactory.get();
					send_config.encoder_settings.bitrate_allocator_factory =
						m.bitrateAllocatorFactory.get();
					send_config.encoder_settings.capabilities =
						webrtc::VideoEncoder::Capabilities(false);
					send_config.send_transport = m.videoTransport.get();
					send_config.rtcp_report_interval_ms = 1000;

					const int enc_width = width > 0 ? width : 640;
					const int enc_height = height > 0 ? height : 360;

					webrtc::VideoEncoderConfig encoder_config;
					encoder_config.codec_type = webrtc::kVideoCodecH264;
					encoder_config.video_format = webrtc::SdpVideoFormat("H264");
					encoder_config.max_bitrate_bps = m.cfg.maxBitrateBps;
					encoder_config.min_transmit_bitrate_bps = 0;
					encoder_config.number_of_streams = 1;
					encoder_config.content_type =
						webrtc::VideoEncoderConfig::ContentType::kRealtimeVideo;
					encoder_config.simulcast_layers.resize(1);
					encoder_config.simulcast_layers[0].width =
						static_cast<size_t>(std::max(16, enc_width));
					encoder_config.simulcast_layers[0].height =
						static_cast<size_t>(std::max(16, enc_height));
					encoder_config.simulcast_layers[0].max_framerate = 30;
					encoder_config.simulcast_layers[0].min_bitrate_bps =
						m.cfg.minBitrateBps;
					encoder_config.simulcast_layers[0].target_bitrate_bps =
						m.cfg.startBitrateBps;
					encoder_config.simulcast_layers[0].max_bitrate_bps =
						m.cfg.maxBitrateBps;
					encoder_config.simulcast_layers[0].active = true;

					m.videoSend = m.call->CreateVideoSendStream(
						std::move(send_config), std::move(encoder_config));
					if (!m.videoSend) {
						m.videoSendFailed = true;
						return false;
					}
					m.videoSend->SetSource(m.videoSource.get(),
						webrtc::DegradationPreference::DISABLED);
					m.videoSend->Start();

					// m140 defers encoder creation until the first input frame
					// arrives; push one black frame to bootstrap it.
					if (!m.bootstrapped) {
						m.videoSource->PushBootstrapFrame(
							std::max(16, enc_width), std::max(16, enc_height));
						m.bootstrapped = true;
					}
					return true;
				}

				bool EnsureVideoRecv(MemberState& m, uint32_t remoteSsrc, int pt) {
					if (m.videoRecv) {
						return m.videoRecvSsrc == remoteSsrc;
					}
					if (!EnsureCall(m)) return false;

					if (!m.videoTransport) {
						m.videoTransport = std::make_unique<CallTransport>(
							m.packetSink, m.memberId, kMediaStreamVideo);
					}
					if (!m.decoderFactory) {
						m.decoderFactory = std::make_unique<PassThroughVideoDecoderFactory>(
							m.memberId, kMediaStreamVideo, m.frameSink, remoteSsrc);
					}

					webrtc::VideoReceiveStreamInterface::Config recv_config(
						m.videoTransport.get(), m.decoderFactory.get());
					recv_config.rtp.remote_ssrc = remoteSsrc;
					recv_config.rtp.local_ssrc = m.videoSendSsrc;
					recv_config.rtp.rtcp_mode = webrtc::RtcpMode::kCompound;
					recv_config.rtp.nack.rtp_history_ms = m.cfg.nackHistoryMs;
					recv_config.rtp.keyframe_method = webrtc::KeyFrameReqMethod::kPliRtcp;
					if (m.cfg.videoRecvRtxPayloadType >= 0 &&
						m.cfg.videoRecvRtxSsrc != 0) {
						recv_config.rtp.rtx_ssrc = m.cfg.videoRecvRtxSsrc;
						recv_config.rtp.rtx_associated_payload_types[
							m.cfg.videoRecvRtxPayloadType] = pt;
					}
					recv_config.decoders.push_back(
						webrtc::VideoReceiveStreamInterface::Decoder(
							webrtc::SdpVideoFormat("H264"), pt));
					recv_config.decoder_factory = m.decoderFactory.get();
					recv_config.renderer = &m.nullRenderer;
					recv_config.rtcp_send_transport = m.videoTransport.get();
					recv_config.enable_prerenderer_smoothing = false;

					m.videoRecv = m.call->CreateVideoReceiveStream(std::move(recv_config));
					if (!m.videoRecv) return false;
					m.videoRecv->Start();
					m.videoRecvSsrc = remoteSsrc;
					m.videoRecvPt = pt;
					return true;
				}

				void EnsureAudioSend(MemberState& m) {
					if (!m.audioState || !m.call) return;
					if (m.audioSendSsrc == 0 || m.audioSendPt < 0) return;

					if (m.audioSend) {
						if (m.audioSendActivePt == m.audioSendPt) return;
						// Renegotiated codec: rebuild the stream so the new PT /
						// encoder takes effect.
						m.call->DestroyAudioSendStream(m.audioSend);
						m.audioSend = nullptr;
						m.audioSendActivePt = -1;
					}

					if (!m.audioTransport) {
						m.audioTransport = std::make_unique<CallTransport>(
							m.packetSink, m.memberId, kMediaStreamAudio);
					}
					if (!m.audioEncoderFactory) {
						m.audioEncoderFactory =
							webrtc::CreateBuiltinAudioEncoderFactory();
					}

					webrtc::AudioSendStream::Config send_config(
						m.audioTransport.get());
					send_config.rtp.ssrc = m.audioSendSsrc;
					send_config.rtp.rtcp_mode = webrtc::RtcpMode::kCompound;
					send_config.rtp.c_name = m.memberId;
					send_config.rtcp_report_interval_ms = 5000;
					send_config.send_transport = m.audioTransport.get();
					send_config.encoder_factory = m.audioEncoderFactory;
					send_config.send_codec_spec =
						webrtc::AudioSendStream::Config::SendCodecSpec(
							m.audioSendPt, AudioFormatForPayloadType(m.audioSendPt));

					m.audioSend = m.call->CreateAudioSendStream(send_config);
					if (m.audioSend) {
						m.audioSend->Start();
						m.audioSendActivePt = m.audioSendPt;
					}
				}

				void EnsureAudioRecv(MemberState& m, uint32_t remoteSsrc) {
					if (m.audioRecv || !m.call) return;
					if (!m.audioTransport) {
						m.audioTransport = std::make_unique<CallTransport>(
							m.packetSink, m.memberId, kMediaStreamAudio);
					}
					if (!m.audioDecoderFactory) {
						m.audioDecoderFactory =
							webrtc::CreateBuiltinAudioDecoderFactory();
					}

					webrtc::AudioReceiveStreamInterface::Config recv_config;
					recv_config.rtp.remote_ssrc = remoteSsrc;
					recv_config.rtp.local_ssrc = m.audioSendSsrc;
					recv_config.rtp.rtcp_mode = webrtc::RtcpMode::kCompound;
					recv_config.rtp.nack.rtp_history_ms = m.cfg.nackHistoryMs;
					recv_config.rtcp_send_transport = m.audioTransport.get();
					recv_config.decoder_factory = m.audioDecoderFactory;
					const int pt = m.audioRecvPt >= 0
						? m.audioRecvPt : m.cfg.audioRecvPayloadType;
					recv_config.decoder_map.emplace(pt, AudioFormatForPayloadType(pt));
					recv_config.jitter_buffer_max_packets = 200;

					m.audioRecv = m.call->CreateAudioReceiveStream(recv_config);
					if (m.audioRecv) {
						m.audioRecv->Start();
						m.audioRecvSsrc = remoteSsrc;
					}
				}

				// ---- ingress ------------------------------------------------------

				void DeliverVideoRtp(MemberState& m, const uint8_t* data, size_t size,
					int streamKind) {
					webrtc::RtpPacketReceived packet;
					if (!packet.Parse(data, size)) return;
					// Call::DeliverRtpPacket() requires a finite arrival time;
					// the gateway hands us the datagram without one.
					packet.set_arrival_time(webrtc::Timestamp::Micros(webrtc::TimeMicros()));
					const uint32_t ssrc = packet.Ssrc();
					const int pt = packet.PayloadType();
					if (!EnsureVideoRecv(m, ssrc, pt)) return;
					if (!m.call) return;
					m.call->Receiver()->DeliverRtpPacket(
						webrtc::MediaType::VIDEO, std::move(packet), nullptr);
				}

				void DeliverAudioRtp(MemberState& m, const uint8_t* data, size_t size) {
					if (!EnsureCall(m)) return;
					webrtc::RtpPacketReceived packet;
					if (!packet.Parse(data, size)) return;
					// Call::DeliverRtpPacket() requires a finite arrival time;
					// the gateway hands us the datagram without one.
					packet.set_arrival_time(webrtc::Timestamp::Micros(webrtc::TimeMicros()));
					const uint32_t ssrc = packet.Ssrc();
					if (!m.audioRecv) {
						m.audioRecvPt = packet.PayloadType();
						EnsureAudioRecv(m, ssrc);
					}
					if (!m.audioRecv) return;
					m.call->Receiver()->DeliverRtpPacket(
						webrtc::MediaType::AUDIO, std::move(packet), nullptr);
				}

				// ---- teardown -----------------------------------------------------

				void DestroyMemberOnMediaThread(MemberState& m) {
					RunOnMediaThreadVoid([this, &m]() {
						if (m.call) {
							if (m.videoSend) {
								m.call->DestroyVideoSendStream(m.videoSend);
								m.videoSend = nullptr;
							}
							if (m.videoRecv) {
								m.call->DestroyVideoReceiveStream(m.videoRecv);
								m.videoRecv = nullptr;
							}
							if (m.audioSend) {
								m.call->DestroyAudioSendStream(m.audioSend);
								m.audioSend = nullptr;
							}
							if (m.audioRecv) {
								m.call->DestroyAudioReceiveStream(m.audioRecv);
								m.audioRecv = nullptr;
							}
						}
						m.audioState = nullptr;
						if (m.adm) {
							m.adm->Terminate();
							m.adm = nullptr;
						}
						m.apm = nullptr;
						m.mixer = nullptr;
						m.call.reset();
						m.encoderBridge.reset();
						m.encoderFactory.reset();
						m.bitrateAllocatorFactory.reset();
						m.decoderFactory.reset();
						m.videoSource.reset();
						m.videoTransport.reset();
						m.audioTransport.reset();
						});
				}

				// ---- state --------------------------------------------------------

				webrtc::Environment env_;
				std::unique_ptr<webrtc::Thread> media_thread_;

				std::mutex members_mu_;
				std::map<std::string, std::shared_ptr<MemberState>> members_;

				IWebRtcPacketSink* packet_sink_ = nullptr;
				IWebRtcEncodedFrameSink* frame_sink_ = nullptr;
				IWebRtcAudioSink* audio_sink_ = nullptr;
			};

		} // namespace

		IWebRtcMediaEngine* CreateWebRtcMediaEngine() {
			return new LibWebRtcMediaEngine();
		}

		void DestroyWebRtcMediaEngine(IWebRtcMediaEngine* engine) {
			delete engine;
		}

	} // namespace webrtc_engine
} // namespace vcs

#endif // VCS_WITH_WEBRTC
