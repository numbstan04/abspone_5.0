/**
 * @file AudioProcessor.cpp
 * @brief Audio processor implementation (terminal version)
 *
 * 接收：RTPEngine::audioCallback → OnRtpPacket → decoder->Decode
 *        → sink->Render
 * 发送：source->Subscribe 回调入 encodeQueue → encodeThread
 *        → encoder->Encode → RTPEngine::SendAudioRtp
 *
 * 线程：
 *   - 接收无独立线程：由 RTPEngine 的 audioCallback 同步驱动
 *   - 发送一个 encodeThread：消费 encodeQueue，编码 + 发送
 */

#include "AudioProcessor.h"
#include "RTPEngine.h"
#include "IAudioSource.h"
#include "IAudioSink.h"
#include "PlatformTime.h"
#include "ILogger.h"
#include "IMediaDebug.h"
#include "SpscQueue.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <thread>
#include <chrono>

namespace vcs {
	namespace media {

		namespace {
			constexpr int kEncodePollIntervalMs = 2;
		}

		// ============================================================
		// AudioProcessor::Impl
		// ============================================================
		struct AudioProcessor::Impl {
			struct StreamContext {
				ResolvedAudioStream resolved;
				std::string streamId;
				std::string consumerId;

				// ---- 接收 ----
				std::unique_ptr<codec::IAudioDecoder> decoder;
				// Read by the 1 Hz media-info tick from another thread.
				std::atomic<uint64_t> framesDecoded{ 0 };
				uint64_t framesDropped = 0;

				// ---- 发送 ----
				std::unique_ptr<codec::IAudioEncoder> encoder;
				SpscQueue<AudioFramePtr> encodeQueue;
				std::thread encodeThread;
				std::atomic<bool> encodeStopRequested{ false };
				uint16_t audioSeq = 0;
				uint32_t rtpTimestampCounter = 0;
				std::atomic<uint64_t> framesEncoded{ 0 };

				// ---- VAD ----
				bool hasVoice = false;
				float energy = 0.0f;
				int speakingFrames = 0;
				int silenceFrames = 0;

				// ---- SetCodec 缓存 ----
				std::string currentCodecName;
				codec::AudioCodecType currentCodecType = codec::AudioCodecType::G711U;
				uint8_t currentPayloadType = 0;
				uint32_t currentBitrate = 64000;

				StreamContext() : encodeQueue(64) {}
			};

			AudioProcessorConfig config;
			std::shared_ptr<codec::ICodecFactory> codecFactory;
			std::weak_ptr<RTPEngine> rtpEngine;

			std::string sessionId_;
			std::shared_ptr<StreamContext> stream_;
			mutable std::mutex streamMtx_;

			VadEventCallback vadCallback_;

			// ---- helpers ----
			bool CreateDecoder(StreamContext& ctx);
			bool CreateEncoder(StreamContext& ctx);
			void DestroyStream(StreamContext& ctx);
			void EncodeLoop(std::shared_ptr<StreamContext> ctx);
			bool RecreateAudioCodecs(StreamContext& ctx,
				codec::AudioCodecType newType,
				uint32_t bitrate);
		};

		// ============================================================
		// CreateDecoder / CreateEncoder / DestroyStream
		// ============================================================
		bool AudioProcessor::Impl::CreateDecoder(StreamContext& ctx) {
			if (!codecFactory) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] CreateDecoder: no codecFactory");
				return false;
			}

			const auto& c = ctx.resolved.config;
			auto dec = codecFactory->CreateAudioDecoder(c.codecType, c.backend);
			if (!dec) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] CreateAudioDecoder returned null "
					"(codec=%d)", (int)c.codecType);
				return false;
			}

			codec::AudioCodecConfig dc;
			dc.codecType = c.codecType;
			dc.backend = c.backend;
			dc.sampleRate = c.sampleRate;
			dc.channels = c.channels;
			dc.bitrate = (c.codecType == codec::AudioCodecType::Opus) ? 64000 : 64000;
			if (!dec->Initialize(dc)) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] audio decoder Initialize failed "
					"(codec=%d)", (int)c.codecType);
				return false;
			}

			ctx.decoder = std::move(dec);
			ctx.currentCodecType = c.codecType;
			ctx.currentBitrate = dc.bitrate;
			return true;
		}

		bool AudioProcessor::Impl::CreateEncoder(StreamContext& ctx) {
			if (!codecFactory) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] CreateEncoder: no codecFactory");
				return false;
			}

			const auto& c = ctx.resolved.config;
			auto enc = codecFactory->CreateAudioEncoder(c.codecType, c.backend);
			if (!enc) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] CreateAudioEncoder returned null "
					"(codec=%d)", (int)c.codecType);
				return false;
			}

			codec::AudioCodecConfig ec;
			ec.codecType = c.codecType;
			ec.backend = c.backend;
			ec.sampleRate = c.sampleRate;
			ec.channels = c.channels;
			ec.bitrate = (c.codecType == codec::AudioCodecType::Opus) ? 64000 : 64000;
			if (!enc->Initialize(ec)) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] audio encoder Initialize failed "
					"(codec=%d)", (int)c.codecType);
				return false;
			}

			// G.711 编码表自检：把 8 个已知 PCM 值编码后打进日志，
			// 对端听不到声音时先排除"编码器本身"。
			// 参考值（按 pjproject alaw_ulaw.c 的公式推得）：
			//   A-law: D5 D5 55 FA 7A B6 36 AA
			//   mu-law: FF FF 7F CE 4E 9C 1C 80
			if (c.channels == 1 &&
				(c.codecType == codec::AudioCodecType::G711A ||
				 c.codecType == codec::AudioCodecType::G711U)) {
				int16_t probe[8] = { 0, 1, -1, 1000, -1000, 10000, -10000, 32767 };
				codec::AudioPcmFrame probeFrame;
				probeFrame.data = probe;
				probeFrame.samples = 8;
				probeFrame.sampleRate = c.sampleRate;
				probeFrame.channels = 1;
				probeFrame.timestamp = 0;

				std::vector<uint8_t> probeOut;
				uint64_t probeTs = 0;
				if (enc->Encode(probeFrame, probeOut, probeTs) &&
					probeOut.size() == 8) {
					VCS_MEDIA_LOG_INFO(
						"[AudioProcessor] G.711 self-test (%s): "
						"pcm=[0,1,-1,1000,-1000,10000,-10000,32767] -> "
						"%02X %02X %02X %02X %02X %02X %02X %02X",
						codec::AudioCodecTypeToString(c.codecType),
						probeOut[0], probeOut[1], probeOut[2], probeOut[3],
						probeOut[4], probeOut[5], probeOut[6], probeOut[7]);
				} else {
					VCS_MEDIA_LOG_WARNING(
						"[AudioProcessor] G.711 self-test failed: codec=%s",
						codec::AudioCodecTypeToString(c.codecType));
				}
			}

			ctx.encoder = std::move(enc);
			return true;
		}

		void AudioProcessor::Impl::DestroyStream(StreamContext& ctx) {
			ctx.encoder.reset();
			ctx.decoder.reset();
		}

		// ============================================================
		// RecreateAudioCodecs
		// ============================================================
		bool AudioProcessor::Impl::RecreateAudioCodecs(StreamContext& ctx,
			codec::AudioCodecType newType,
			uint32_t bitrate) {
			if (!codecFactory) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] RecreateAudioCodecs: no codecFactory");
				return false;
			}

			ctx.decoder.reset();
			ctx.encoder.reset();

			ctx.resolved.config.codecType = newType;
			if (bitrate > 0) ctx.resolved.config.bitrate = bitrate;

			if (ctx.resolved.sink && !CreateDecoder(ctx)) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] RecreateAudioCodecs: decoder failed");
				return false;
			}
			if (ctx.resolved.source && !CreateEncoder(ctx)) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] RecreateAudioCodecs: encoder failed");
				return false;
			}

			ctx.currentCodecType = newType;
			if (bitrate > 0) ctx.currentBitrate = bitrate;

			VCS_MEDIA_LOG_INFO("[AudioProcessor] RecreateAudioCodecs OK: codec=%s, bitrate=%u",
				codec::AudioCodecTypeToString(newType), ctx.currentBitrate);
			return true;
		}

		// ============================================================
		// EncodeLoop
		// ============================================================
		void AudioProcessor::Impl::EncodeLoop(std::shared_ptr<StreamContext> ctx) {
			if (!ctx) return;
			VCS_MEDIA_LOG_INFO("[AudioProcessor] EncodeLoop started: session=%s",
				sessionId_.c_str());

			const uint32_t sampleRate = ctx->resolved.config.sampleRate;
			const uint32_t frameSizeMs = ctx->resolved.config.frameSizeMs;
			const uint32_t samplesPerFrame = (sampleRate * frameSizeMs) / 1000;
			const float micGain = config.micGain;
			std::vector<int16_t> gained;

			// ---- 发送链路诊断（1 Hz）----
			// 对端听不到自己的声音时，必须先用数字把问题夹在"采集/编码/发送"
			// 的某一环：peakPreGain 来自采集，peakPostGain 进编码器，
			// d5/ff 是 G.711 的静音码（A-law 0xD5 / mu-law 0xFF）。
			uint64_t diagFrames = 0;
			uint64_t diagZeroInputFrames = 0;
			uint64_t diagBytes = 0;
			uint64_t diagSilenceD5 = 0;
			uint64_t diagSilenceFF = 0;
			int32_t  diagPeakPre = 0;
			int32_t  diagPeakPost = 0;
			uint8_t  diagFirstBytes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
			bool     diagHaveFirst = false;
			int64_t  diagLastLogMs = static_cast<int64_t>(vcs::base::platform::UtcNowEpochMs());

			// 调试旁路：指针进程内稳定；开关关闭时 SaveXxx 只读一个 atomic。
			IMediaDebug* const debug = GetMediaDebug();

			while (!ctx->encodeStopRequested.load(std::memory_order_acquire)) {
				AudioFramePtr frame;
				if (!ctx->encodeQueue.TryPop(frame)) {
					std::this_thread::sleep_for(
						std::chrono::milliseconds(kEncodePollIntervalMs));
					continue;
				}
				if (!frame || !ctx->encoder) continue;
				if (!frame->data || frame->samples == 0) continue;

				const int16_t* src = frame->data->data();
				int32_t peakPre = 0;
				for (size_t i = 0; i < frame->samples; ++i) {
					const int32_t v = src[i] < 0
						? -static_cast<int32_t>(src[i])
						: static_cast<int32_t>(src[i]);
					if (v > peakPre) peakPre = v;
				}
				if (peakPre > diagPeakPre) diagPeakPre = peakPre;
				if (peakPre == 0) ++diagZeroInputFrames;

				int16_t* pcmSamples = frame->data->data();
				int32_t peakPost = peakPre;
				if (micGain != 1.0f) {
					gained.resize(frame->samples);
					peakPost = 0;
					for (size_t i = 0; i < frame->samples; ++i) {
						float v = static_cast<float>(pcmSamples[i]) * micGain;
						if (v > 32767.0f) v = 32767.0f;
						else if (v < -32768.0f) v = -32768.0f;
						gained[i] = static_cast<int16_t>(
							v >= 0.0f ? v + 0.5f : v - 0.5f);
						const int32_t a = gained[i] < 0
							? -static_cast<int32_t>(gained[i])
							: static_cast<int32_t>(gained[i]);
						if (a > peakPost) peakPost = a;
					}
					pcmSamples = gained.data();
				}
				if (peakPost > diagPeakPost) diagPeakPost = peakPost;

				if (debug) {
					debug->SaveAudioEncoded(sessionId_,
						reinterpret_cast<const uint8_t*>(pcmSamples),
						frame->samples * sizeof(int16_t));
				}

				codec::AudioPcmFrame pcm;
				pcm.data = pcmSamples;
				pcm.samples = frame->samples;
				pcm.sampleRate = frame->sampleRate;
				pcm.channels = frame->channels;
				pcm.timestamp = frame->timestamp;

				std::vector<uint8_t> encoded;
				uint64_t outTs = 0;
				if (!ctx->encoder->Encode(pcm, encoded, outTs)) {
					VCS_MEDIA_LOG_WARNING("[AudioProcessor] audio encode failed: samples=%zu",
						frame->samples);
					continue;
				}
				if (encoded.empty()) continue;

				if (debug) {
					debug->SaveAudioOutput(sessionId_, encoded.data(), encoded.size());
				}

				++diagFrames;
				diagBytes += encoded.size();
				for (size_t i = 0; i < encoded.size(); ++i) {
					if (encoded[i] == 0xD5) ++diagSilenceD5;
					else if (encoded[i] == 0xFF) ++diagSilenceFF;
				}
				if (!diagHaveFirst) {
					const size_t n = encoded.size() < 8 ? encoded.size() : 8;
					for (size_t i = 0; i < n; ++i) diagFirstBytes[i] = encoded[i];
					diagHaveFirst = true;
				}

				const int64_t nowMs = static_cast<int64_t>(vcs::base::platform::UtcNowEpochMs());
				if (nowMs - diagLastLogMs >= 1000) {
					diagLastLogMs = nowMs;
					VCS_MEDIA_LOG_INFO(
						"[AudioProcessor] audio TX diag: session=%s, codec=%s, gain=%.2fx, "
						"frames=%llu, zeroInFrames=%llu, peakPreGain=%d, peakPostGain=%d, "
						"bytes=%llu, silenceD5=%llu, silenceFF=%llu, "
						"firstBytes=%02X %02X %02X %02X %02X %02X %02X %02X",
						sessionId_.c_str(),
						codec::AudioCodecTypeToString(ctx->resolved.config.codecType),
						micGain,
						static_cast<unsigned long long>(diagFrames),
						static_cast<unsigned long long>(diagZeroInputFrames),
						diagPeakPre, diagPeakPost,
						static_cast<unsigned long long>(diagBytes),
						static_cast<unsigned long long>(diagSilenceD5),
						static_cast<unsigned long long>(diagSilenceFF),
						diagFirstBytes[0], diagFirstBytes[1], diagFirstBytes[2], diagFirstBytes[3],
						diagFirstBytes[4], diagFirstBytes[5], diagFirstBytes[6], diagFirstBytes[7]);

					if (diagPeakPost == 0 && diagFrames > 0) {
						VCS_MEDIA_LOG_WARNING(
							"[AudioProcessor] audio TX input is SILENT: session=%s, frames=%llu, "
							"gain=%.2fx (capture produced all-zero PCM; see WasapiAudioSource diag)",
							sessionId_.c_str(),
							static_cast<unsigned long long>(diagFrames), micGain);
					}

					diagFrames = 0;
					diagZeroInputFrames = 0;
					diagBytes = 0;
					diagSilenceD5 = 0;
					diagSilenceFF = 0;
					diagPeakPre = 0;
					diagPeakPost = 0;
					diagHaveFirst = false;
				}

				ctx->framesEncoded++;

				auto rtp = rtpEngine.lock();
				if (!rtp) continue;

				// RTP 时间戳按 samplesPerFrame 递增
				ctx->rtpTimestampCounter += samplesPerFrame;
				const uint16_t seq = ++ctx->audioSeq;

				try {
					rtp->SendAudioRtp(sessionId_,
						encoded.data(), encoded.size(),
						ctx->resolved.config.localSsrc,
						ctx->rtpTimestampCounter,
						seq);
				}
				catch (const std::exception& e) {
					VCS_MEDIA_LOG_ERROR("[AudioProcessor] SendAudioRtp threw: %s", e.what());
				}
			}

			VCS_MEDIA_LOG_INFO("[AudioProcessor] EncodeLoop stopped: session=%s",
				sessionId_.c_str());
		}

		// ============================================================
		// AudioProcessor public API
		// ============================================================
		AudioProcessor::AudioProcessor() : impl_(std::make_unique<Impl>()) {
			VCS_MEDIA_LOG_INFO("[AudioProcessor] created");
		}

		AudioProcessor::~AudioProcessor() {
			Stop();
			VCS_MEDIA_LOG_INFO("[AudioProcessor] destroyed");
		}

		bool AudioProcessor::Initialize(
			const AudioProcessorConfig& config,
			std::shared_ptr<codec::ICodecFactory> codecFactory) {

			if (!codecFactory) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] Initialize: codecFactory is null");
				return false;
			}

			impl_->config = config;
			impl_->codecFactory = std::move(codecFactory);

			VCS_MEDIA_LOG_INFO("[AudioProcessor] Initialize: codec=%s, %uHz, %uch, %ums, vad=%d",
				codec::AudioCodecTypeToString(config.codecType),
				config.sampleRate, config.channels,
				config.frameSizeMs, (int)config.vadEnabled);
			return true;
		}

		void AudioProcessor::SetRtpEngine(std::weak_ptr<RTPEngine> rtpEngine) {
			impl_->rtpEngine = std::move(rtpEngine);
		}

		bool AudioProcessor::Start(const std::string& sessionId,
			const ResolvedAudioStream& stream) {
			if (sessionId.empty()) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] Start: empty sessionId");
				return false;
			}
			if (!stream.source && !stream.sink) {
				VCS_MEDIA_LOG_WARNING("[AudioProcessor] Start: neither source nor sink");
				return false;
			}

			std::lock_guard<std::mutex> lock(impl_->streamMtx_);
			if (impl_->stream_) {
				VCS_MEDIA_LOG_WARNING("[AudioProcessor] Start: already started (session=%s)",
					impl_->sessionId_.c_str());
				return false;
			}

			impl_->sessionId_ = sessionId;

			auto ctx = std::make_shared<Impl::StreamContext>();
			ctx->resolved = stream;
			ctx->streamId = sessionId + "_audio";
			ctx->consumerId = ctx->streamId;

			// 接收方向：decoder + sink 绑定
			if (stream.sink) {
				if (!impl_->CreateDecoder(*ctx)) {
					VCS_MEDIA_LOG_ERROR("[AudioProcessor] Start: CreateDecoder failed");
					return false;
				}
				if (!stream.sink->BindStream(ctx->streamId)) {
					VCS_MEDIA_LOG_WARNING("[AudioProcessor] Start: sink %s already bound",
						stream.sink->GetId().c_str());
				}
			}

			// 发送方向：encoder + encodeThread + Subscribe
			if (stream.source) {
				if (!impl_->CreateEncoder(*ctx)) {
					VCS_MEDIA_LOG_ERROR("[AudioProcessor] Start: CreateEncoder failed");
					return false;
				}

				ctx->encodeStopRequested = false;
				std::shared_ptr<Impl::StreamContext> ctxCopy = ctx;
				ctx->encodeThread = std::thread(
					[this, ctxCopy] { impl_->EncodeLoop(ctxCopy); });

				auto ctxRaw = ctx.get();
				const std::string memberId = sessionId;
				std::weak_ptr<RTPEngine> engine = impl_->rtpEngine;
				stream.source->Subscribe(ctx->consumerId,
					[ctxRaw, memberId, engine](AudioFramePtr f) {
					if (!ctxRaw || !f) return;
					if (!f->data || f->samples == 0) return;

					// [WebRTC media engine] PCM goes straight into the ADM
					// (AudioProcessing -> AudioSendStream). Skipping the local
					// encoder is what makes AEC3 see the real capture signal.
					if (auto rtp = engine.lock()) {
						if (rtp->PushCapturedAudio(memberId,
								f->data->data(), f->samples,
								static_cast<int>(f->sampleRate),
								static_cast<int>(f->channels))) {
							return;
						}
					}
					ctxRaw->encodeQueue.TryPush(std::move(f));
				});
			}

			impl_->stream_ = ctx;

			VCS_MEDIA_LOG_INFO("[AudioProcessor] Start: session=%s, source=%s, sink=%s",
				sessionId.c_str(),
				stream.source ? stream.source->GetId().c_str() : "null",
				stream.sink ? stream.sink->GetId().c_str() : "null");
			return true;
		}

		void AudioProcessor::Stop() {
			std::shared_ptr<Impl::StreamContext> ctx;
			{
				std::lock_guard<std::mutex> lock(impl_->streamMtx_);
				if (!impl_->stream_) return;
				ctx = std::move(impl_->stream_);
				impl_->stream_ = nullptr;
				impl_->sessionId_.clear();
			}

			if (!ctx) return;

			// 1. 先标记停止，再 Unsubscribe（保证回调不会再来），最后 join
			ctx->encodeStopRequested = true;

			if (ctx->resolved.source) {
				ctx->resolved.source->Unsubscribe(ctx->consumerId);
			}
			if (ctx->encodeThread.joinable()) {
				ctx->encodeThread.join();
			}
			if (ctx->resolved.sink) {
				ctx->resolved.sink->UnbindStream();
			}

			impl_->DestroyStream(*ctx);
			VCS_MEDIA_LOG_INFO("[AudioProcessor] Stop: done");
		}

		// ============================================================
		// OnRtpPacket（接收方向：同步解码 + 渲染 + VAD）
		// ============================================================
		void AudioProcessor::OnRtpPacket(uint32_t ssrc,
			const uint8_t* encodedData,
			size_t encodedSize,
			uint64_t timestamp) {
			if (!encodedData || encodedSize == 0) return;

			std::shared_ptr<Impl::StreamContext> ctx;
			VadEventCallback vadCb;
			AudioProcessorConfig configCopy;
			{
				std::lock_guard<std::mutex> lock(impl_->streamMtx_);
				if (!impl_->stream_) return;
				ctx = impl_->stream_;
				vadCb = impl_->vadCallback_;
				configCopy = impl_->config;
			}

			// SSRC 校验（remoteSsrc == 0 表示接受任意）
			if (ctx->resolved.config.remoteSsrc != 0 &&
				ssrc != ctx->resolved.config.remoteSsrc) {
				return;
			}

			if (!ctx->decoder || !ctx->resolved.sink) return;

			// 解码
			codec::AudioPcmFrame pcm;
			if (!ctx->decoder->Decode(encodedData, encodedSize, pcm)) {
				ctx->framesDropped++;
				return;
			}
			std::unique_ptr<int16_t[]> guard(pcm.data);
			pcm.data = nullptr;

			if (!guard || pcm.samples == 0) {
				ctx->framesDropped++;
				return;
			}

			ctx->framesDecoded++;

			// 包成 AudioFrame 交给 sink
			auto out = std::make_shared<AudioFrame>();
			out->data = std::make_shared<std::vector<int16_t>>(
				guard.get(), guard.get() + pcm.samples);
			out->samples = pcm.samples;
			out->sampleRate = pcm.sampleRate;
			out->channels = pcm.channels;
			out->timestamp = timestamp;

			ctx->resolved.sink->Render(out);

			// VAD
			if (configCopy.vadEnabled) {
				double sum = 0.0;
				for (size_t i = 0; i < pcm.samples; ++i) {
					double v = static_cast<double>(guard[i]) / 32768.0;
					sum += v * v;
				}
				const float energy = static_cast<float>(
					std::sqrt(sum / static_cast<double>(pcm.samples)));
				ctx->energy = energy;

				if (energy > configCopy.voiceThreshold) {
					ctx->speakingFrames++;
					ctx->silenceFrames = 0;
					if (ctx->speakingFrames >= configCopy.voiceFramesThreshold &&
						!ctx->hasVoice) {
						ctx->hasVoice = true;
						if (vadCb) vadCb(VadEvent::SpeakingStart);
					}
				}
				else if (energy < configCopy.noiseThreshold) {
					ctx->silenceFrames++;
					ctx->speakingFrames = 0;
					if (ctx->silenceFrames >= configCopy.silenceFramesThreshold &&
						ctx->hasVoice) {
						ctx->hasVoice = false;
						if (vadCb) vadCb(VadEvent::SpeakingEnd);
					}
				}
			}
		}

		// ============================================================
		// RenderPcm（WebRTC media engine：NetEq 已解码并做过 AEC）
		// ============================================================
		void AudioProcessor::RenderPcm(const std::string& memberId,
			const int16_t* pcm, size_t samplesPerChannel,
			int sampleRate, int channels) {
			if (!pcm || samplesPerChannel == 0) return;

			std::shared_ptr<Impl::StreamContext> ctx;
			{
				std::lock_guard<std::mutex> lock(impl_->streamMtx_);
				if (!impl_->stream_) return;
				// 只渲染本会话（memberId 即 sessionId）。
				if (!memberId.empty() && memberId != impl_->sessionId_) return;
				ctx = impl_->stream_;
				ctx->framesDecoded++;
			}
			if (!ctx->resolved.sink) return;

			const size_t totalSamples = samplesPerChannel *
				static_cast<size_t>(channels > 0 ? channels : 1);
			auto out = std::make_shared<AudioFrame>();
			out->data = std::make_shared<std::vector<int16_t>>(
				pcm, pcm + totalSamples);
			out->samples = samplesPerChannel;
			out->sampleRate = static_cast<uint32_t>(sampleRate > 0 ? sampleRate : 8000);
			out->channels = static_cast<uint8_t>(channels > 0 ? channels : 1);
			out->timestamp = 0;

			ctx->framesDecoded++;
			ctx->resolved.sink->Render(out);
		}

		// ============================================================
		// SetCodec
		// ============================================================
		void AudioProcessor::SetCodec(const std::string& codecName,
			uint8_t payloadType) {
			std::shared_ptr<Impl::StreamContext> ctx;
			{
				std::lock_guard<std::mutex> lock(impl_->streamMtx_);
				if (!impl_->stream_) {
					VCS_MEDIA_LOG_WARNING("[AudioProcessor] SetCodec: not started");
					return;
				}
				ctx = impl_->stream_;
			}

			codec::AudioCodecType newType = ctx->currentCodecType;
			if (!codec::TryParseAudioCodecType(codecName, newType)) {
				VCS_MEDIA_LOG_WARNING("[AudioProcessor] SetCodec: cannot parse '%s'",
					codecName.c_str());
				return;
			}

			const bool firstInit = ctx->currentCodecName.empty();
			const bool typeChanged = (newType != ctx->currentCodecType);

			VCS_MEDIA_LOG_INFO("[AudioProcessor] SetCodec: name=%s, pt=%u, firstInit=%d, typeChanged=%d",
				codecName.c_str(), payloadType,
				(int)firstInit, (int)typeChanged);

			// 发包 PT 必须跟随协商结果，否则会出现"编码 PCMA、包头 PT=0"。
			auto pushSendPayloadType = [this, payloadType]() {
				auto rtp = impl_->rtpEngine.lock();
				if (!rtp) {
					VCS_MEDIA_LOG_WARNING(
						"[AudioProcessor] SetCodec: rtpEngine gone, audio PT %u not applied",
						(unsigned)payloadType);
					return;
				}
				rtp->UpdateMemberAudioSendPayloadType(impl_->sessionId_, payloadType);
			};

			if (!firstInit && !typeChanged) {
				ctx->currentCodecName = codecName;
				ctx->currentPayloadType = payloadType;
				pushSendPayloadType();
				return;
			}

			// 需要重建：先停 encodeThread，重建 decoder/encoder，再启
			ctx->encodeStopRequested = true;
			if (ctx->encodeThread.joinable()) ctx->encodeThread.join();
			ctx->encodeStopRequested = false;

			bool ok = impl_->RecreateAudioCodecs(*ctx, newType, /*bitrate=*/0);
			if (!ok) {
				VCS_MEDIA_LOG_ERROR("[AudioProcessor] SetCodec recreate failed; keeping old state");
			}
			else {
				ctx->currentCodecName = codecName;
				ctx->currentPayloadType = payloadType;
				pushSendPayloadType();
			}

			if (ctx->resolved.source) {
				std::shared_ptr<Impl::StreamContext> ctxCopy = ctx;
				ctx->encodeThread = std::thread(
					[this, ctxCopy] { impl_->EncodeLoop(ctxCopy); });
			}
		}

		// ============================================================
		// VAD / query / statistics
		// ============================================================
		void AudioProcessor::SetVadCallback(VadEventCallback callback) {
			std::lock_guard<std::mutex> lock(impl_->streamMtx_);
			impl_->vadCallback_ = std::move(callback);
			VCS_MEDIA_LOG_INFO("[AudioProcessor] SetVadCallback: %s",
				impl_->vadCallback_ ? "set" : "cleared");
		}

		bool AudioProcessor::HasVoice() const {
			std::lock_guard<std::mutex> lock(impl_->streamMtx_);
			return impl_->stream_ ? impl_->stream_->hasVoice : false;
		}

		uint32_t AudioProcessor::GetAudioSsrc() const {
			std::lock_guard<std::mutex> lock(impl_->streamMtx_);
			return impl_->stream_ ? impl_->stream_->resolved.config.localSsrc : 0;
		}

		AudioStreamReport AudioProcessor::GetReport() const {
			AudioStreamReport report;
			std::lock_guard<std::mutex> lock(impl_->streamMtx_);
			if (!impl_->stream_) return report;
			report.framesEncoded = impl_->stream_->framesEncoded.load();
			report.framesDecoded = impl_->stream_->framesDecoded.load();
			return report;
		}

	}
} // namespace vcs::media
