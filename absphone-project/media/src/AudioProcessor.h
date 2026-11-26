#pragma once

#include "media_export.h"
#include "AudioFrame.h"
#include "codec/ICodec.h"
#include "MediaStatistics.h"
#include "ErrorCode.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace vcs {
	namespace media {

		class RTPEngine;
		class IAudioSource;
		class IAudioSink;

		// ============================================================
		// VAD events
		// ============================================================
		enum class VadEvent {
			SpeakingStart,
			SpeakingEnd,
		};
		using VadEventCallback = std::function<void(VadEvent event)>;

		// ============================================================
		// 音频流配置（终端通常只有一条音频流）
		//
		// sourceId / sinkId 独立双向：
		//   - sourceId 非空：发送方向启用，订阅该采集源（麦克风）
		//   - sinkId   非空：接收方向启用，绑定该渲染目标（扬声器）
		// ============================================================
		struct AudioStreamConfig {
			uint32_t remoteSsrc = 0;      // 0 = 接受任意 SSRC
			uint32_t localSsrc = 0;
			uint8_t payloadType = 0;

			codec::AudioCodecType codecType = codec::AudioCodecType::G711U;
			codec::CodecBackend backend = codec::CodecBackend::Software;

			uint32_t sampleRate = 8000;
			uint8_t channels = 1;
			uint32_t frameSizeMs = 20;
			uint32_t bitrate = 64000;

			std::string sourceId;   // 空 = 不发送
			std::string sinkId;     // 空 = 不渲染
		};

		// ============================================================
		// AudioProcessor 配置
		// ============================================================
		struct AudioProcessorConfig {
			uint32_t sampleRate = 8000;
			uint8_t channels = 1;
			uint32_t frameSizeMs = 20;
			codec::AudioCodecType codecType = codec::AudioCodecType::G711U;
			codec::CodecBackend codecBackend = codec::CodecBackend::Software;

			// Send-side mic trim (linear). Applied to captured PCM before the
			// encoder; 1.0 leaves samples untouched. Clamped to int16 range.
			float micGain = 1.0f;

			// VAD
			bool vadEnabled = true;
			float voiceThreshold = 0.1f;
			float noiseThreshold = 0.02f;
			int voiceFramesThreshold = 2;
			int silenceFramesThreshold = 3;
		};

		/**
		 * @brief Audio processor（终端版）
		 *
		 * 路由模型：
		 *   发送：IAudioSource（一对多）──Subscribe──▶ 编码器 ──▶ RTPEngine::SendAudioRtp
		 *   接收：RTPEngine::audioCallback ──▶ 解码 ──▶ Render ──▶ IAudioSink（一对一）
		 *
		 * 线程：
		 *   - 接收方向不单独开线程：由 RTPEngine 的 audioCallback 同步驱动
		 *     （G.711 软解很快；如果将来 codec 变重，可以在 OnRtpPacket 里
		 *     加一个解码队列 + 独立线程，接口不变）
		 *   - 发送方向：一个 encodeThread，消费 encodeQueue
		 *
		 * 生命周期：
		 *   Initialize()  注入 config / codecFactory
		 *   SetRtpEngine() 注入 RTPEngine
		 *   Start()       会话开始，创建 decoder / encoder，启动 captureThread
		 *   Stop()        会话结束，停止 captureThread，销毁 decoder / encoder
		 */
		// ============================================================
		// 单路音频处理快照（供 MediaInfoSnapshot）
		// ============================================================
		struct AudioStreamReport {
			uint64_t framesEncoded = 0;
			uint64_t framesDecoded = 0;
		};

		class AudioProcessor {
		public:
			VCS_MEDIA_API AudioProcessor();
			VCS_MEDIA_API ~AudioProcessor();

			// 配置 + 设备指针（由 MediaControl 解析 sourceId / sinkId 后填入）
			struct ResolvedAudioStream {
				AudioStreamConfig config;
				std::shared_ptr<IAudioSource> source;   // null = 不发送
				std::shared_ptr<IAudioSink>   sink;     // null = 不渲染
			};

			// ============================================================
			// Initialization
			// ============================================================
			VCS_MEDIA_API bool Initialize(
				const AudioProcessorConfig& config,
				std::shared_ptr<codec::ICodecFactory> codecFactory);

			// ============================================================
			// Dependency injection
			// ============================================================
			VCS_MEDIA_API void SetRtpEngine(std::weak_ptr<RTPEngine> rtpEngine);

			// ============================================================
			// Session lifecycle
			//
			// Start：
			//   - 记录 sessionId
			//   - sink 非空：创建 decoder，sink->BindStream(streamId)
			//   - source 非空：创建 encoder，启动 encodeThread，
			//                  source->Subscribe(consumerId, cb)
			//
			// Stop：逆序清理，停线程、Unsubscribe、UnbindStream、销毁 decoder/encoder
			// ============================================================
			VCS_MEDIA_API bool Start(const std::string& sessionId,
				const ResolvedAudioStream& stream);
			VCS_MEDIA_API void Stop();

			// ============================================================
			// Receive path（RTPEngine::audioCallback 调用）
			//
			// encodedData 是 RTP payload（已跳过 12 字节 RTP 头）。
			// ssrc 用于校验当前音频流（remoteSsrc != 0 时不匹配则丢弃）。
			// 解码后直接 sink->Render，不经过 ring buffer / 混音。
			// ============================================================
			VCS_MEDIA_API void OnRtpPacket(uint32_t ssrc,
				const uint8_t* encodedData,
				size_t encodedSize,
				uint64_t timestamp);

			// [WebRTC media engine] NetEq playout PCM (interleaved int16),
			// already AEC/NS/AGC processed. Rendered straight to IAudioSink.
			VCS_MEDIA_API void RenderPcm(const std::string& memberId,
				const int16_t* pcm, size_t samplesPerChannel,
				int sampleRate, int channels);

			// ============================================================
			// Codec（协商后调用）
			// ============================================================
			VCS_MEDIA_API void SetCodec(const std::string& codecName,
				uint8_t payloadType);

			// ============================================================
			// VAD / query / statistics
			// ============================================================
			VCS_MEDIA_API void SetVadCallback(VadEventCallback callback);
			VCS_MEDIA_API bool HasVoice() const;
			VCS_MEDIA_API uint32_t GetAudioSsrc() const;
			// 累计计数；由 MediaControl 的 1 Hz tick 拉取换算成帧率。
			VCS_MEDIA_API AudioStreamReport GetReport() const;

		private:
			struct Impl;
			std::unique_ptr<Impl> impl_;
		};

	} // namespace media
} // namespace vcs
