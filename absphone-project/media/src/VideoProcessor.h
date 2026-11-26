#pragma once

#include "media_export.h"
#include "VideoFrameQueue.h"
#include "VideoFrame.h"
#include "codec/ICodec.h"
#include "ErrorCode.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vcs {
	namespace media {

		class RTPEngine;
		class IVideoSource;
		class IVideoSink;

		// ============================================================
		// 视频流种类
		//
		// 一个对端最多两条视频流：
		//   Main      - 主流（camera 采集 / 主窗口渲染）
		//   Secondary - 辅流（screen 采集 / 辅窗口渲染，如内容共享）
		//
		// 与 MediaStreamKind 的对应：
		//   Main      <-> MediaStreamKind::Video
		//   Secondary <-> MediaStreamKind::SecondaryVideo
		// ============================================================
		enum class VideoStreamKind : uint8_t {
			Main = 0,
			Secondary = 1,
		};

		// ============================================================
		// 单条视频流的处理快照（1 Hz 采集，供 MediaInfoSnapshot）
		//
		// 计数器是累计值；MediaControl 每秒拉一次，由统计收集器换算成每秒
		// 帧率。没有这个出口，媒体信息里的尺寸/帧率/丢帧/关键帧同步状态
		// 就只能靠猜。
		// ============================================================
		struct VideoStreamReport {
			VideoStreamKind kind = VideoStreamKind::Main;
			uint32_t encodedWidth = 0;   // 发送方向编码尺寸
			uint32_t encodedHeight = 0;
			uint32_t decodedWidth = 0;   // 接收方向解码输出尺寸
			uint32_t decodedHeight = 0;
			uint64_t framesEncoded = 0;
			uint64_t framesDecoded = 0;
			uint64_t framesDropped = 0;
			uint64_t firRequests = 0;    // 已发出的关键帧请求数
			bool firstKeyframeDecoded = false;  // 同步门是否已打开
		};

		// ============================================================
		// 单条视频流配置
		//
		// sourceId / sinkId 独立双向：
		//   - sourceId 非空：发送方向启用，订阅该采集源
		//   - sinkId   非空：接收方向启用，绑定该渲染目标
		//   - 两者都非空：双向
		//   - 两者都为空：不启用（预留）
		//
		// 主流默认 sourceId="camera:0" / sinkId="main_window"
		// 辅流默认 sourceId="screen:0" / sinkId="secondary_window"
		// 由 MediaControl 在 StartMedia 时填充默认值。
		// ============================================================
		struct VideoStreamConfig {
			VideoStreamKind kind = VideoStreamKind::Main;
			uint32_t remoteSsrc = 0;      // 0 = 由 RTPEngine 学习
			uint32_t localSsrc = 0;
			uint8_t payloadType = 0;

			codec::VideoCodecType codecType = codec::VideoCodecType::H264;
			codec::CodecBackend backend = codec::CodecBackend::Software;

			uint32_t width = 640;
			uint32_t height = 360;
			uint32_t framerate = 30;
			uint32_t bitrate = 500000;
			uint32_t keyFrameInterval = 60;

			std::string sourceId;   // 空 = 不发送
			std::string sinkId;     // 空 = 不渲染
		};

		// ============================================================
		// VideoProcessor 配置
		//
		// 默认的编解码参数。每条流的实际参数由 VideoStreamConfig 指定。
		// ============================================================
		struct VideoProcessorConfig {
			uint32_t inputWidth = 640;
			uint32_t inputHeight = 360;
			codec::VideoCodecType inputCodec = codec::VideoCodecType::H264;
			codec::CodecBackend inputBackend = codec::CodecBackend::Software;

			uint32_t outputWidth = 640;
			uint32_t outputHeight = 360;
			uint32_t framerate = 30;
			uint32_t bitrate = 500000;
			uint32_t keyFrameInterval = 60;
			codec::VideoCodecType outputCodec = codec::VideoCodecType::H264;
			codec::CodecBackend outputBackend = codec::CodecBackend::Software;

			bool testMode = false;
		};

		/**
		 * @brief Video processor（终端版）
		 *
		 * 路由模型：
		 *   发送：IVideoSource（一对多）──Subscribe──▶ 每条流的编码器 ──▶ RTPEngine
		 *   接收：VideoFrameQueue::Pop ──▶ 解码器 ──▶ Render ──▶ IVideoSink（一对一）
		 *
		 * 每条流（StreamContext）：
		 *   - 独立 decoder / encoder
		 *   - 独立 VideoFrameQueue（接收方向）
		 *   - 独立编码队列（SPSC，采集回调里快速入队）
		 *   - 独立 encodeThread
		 *   - 订阅的 source（shared_ptr<IVideoSource>）+ 绑定的 sink
		 *
		 * 线程：
		 *   - decodeThread（全局一个）：遍历所有流的队列，Pop + Decode + Render
		 *   - encodeThread（每流一个）：消费该流的 encodeQueue，Encode + SendVideoRtp
		 */
		class VideoProcessor {
		public:
			VCS_MEDIA_API VideoProcessor();
			VCS_MEDIA_API ~VideoProcessor();

			// 配置 + 设备指针（由 MediaControl 解析 sourceId / sinkId 后填入）
			struct ResolvedVideoStream {
				VideoStreamConfig config;
				std::shared_ptr<IVideoSource> source;   // null = 不发送
				std::shared_ptr<IVideoSink>   sink;     // null = 不渲染
			};

			// ============================================================
			// Initialization
			// ============================================================
			VCS_MEDIA_API bool Initialize(
				const VideoProcessorConfig& config,
				std::shared_ptr<codec::ICodecFactory> codecFactory);

			// ============================================================
			// Dependency injection
			// ============================================================
			VCS_MEDIA_API void SetRtpEngine(std::weak_ptr<RTPEngine> rtpEngine);
			VCS_MEDIA_API void SetVideoFrameQueuePool(
				std::shared_ptr<VideoFrameQueuePool> queuePool);
			VCS_MEDIA_API std::shared_ptr<VideoFrameQueuePool>
				GetVideoFrameQueuePool() const;

			// ============================================================
			// Session lifecycle
			//
			// Start：
			//   - 记录 sessionId
			//   - 为每条 stream 创建 StreamContext
			//   - source 非空：source->Subscribe(consumerId, cb)，采集回调里
			//     把帧推入 encodeQueue，encodeThread 消费
			//   - sink 非空：sink->BindStream(streamId)
			//   - 启动 decodeThread
			//
			// Stop：逆序清理，停线程、Unsubscribe、UnbindStream、销毁 StreamContext
			// ============================================================
			VCS_MEDIA_API bool Start(
				const std::string& sessionId,
				const std::vector<ResolvedVideoStream>& streams);
			VCS_MEDIA_API void Stop();

			// ============================================================
			// Stream lifecycle（会话中途增删流，如辅流中途出现/结束）
			//
			// 当前实现 Main 已支持；Secondary 也支持（接口就绪）。
			// ============================================================
			VCS_MEDIA_API bool AddStream(const ResolvedVideoStream& stream);
			VCS_MEDIA_API bool RemoveStream(VideoStreamKind kind);

			// ============================================================
			// Codec / keyframe
			// ============================================================
			// payloadType is the SDP-negotiated value: it is pushed to
			// RTPEngine for both the send path and inbound classification.
			// width/height/bitrate of 0 keep the current values (SDP carries
			// no resolution for H.264, so callers normally pass 0).
			// fmtp is the raw a=fmtp value; only packetization-mode,
			// profile-level-id and sprop-parameter-sets are consumed.
			VCS_MEDIA_API void SetCodec(VideoStreamKind kind,
				const std::string& codecName,
				uint8_t payloadType,
				uint16_t width = 0,
				uint16_t height = 0,
				uint32_t bitrate = 0,
				const std::string& fmtp = std::string());
			VCS_MEDIA_API void RequestKeyFrame(
				VideoStreamKind kind = VideoStreamKind::Main);
			// [docs/09 §2.4] 请求远端发关键帧（RTCP FIR，带节流）。
			// 与 RequestKeyFrame（本地编码器出 IDR）方向相反。
			VCS_MEDIA_API void RequestRemoteKeyFrame(
				VideoStreamKind kind = VideoStreamKind::Main);

			// ============================================================
			// Query / statistics
			// ============================================================
			VCS_MEDIA_API bool HasVideo(
				VideoStreamKind kind = VideoStreamKind::Main) const;
			VCS_MEDIA_API uint32_t GetVideoSsrc(
				VideoStreamKind kind = VideoStreamKind::Main) const;
			// 每条流一份累计计数；由 MediaControl 的 1 Hz tick 拉取。
			VCS_MEDIA_API std::vector<VideoStreamReport> GetStreamReports() const;

			// ============================================================
			// Test mode
			// ============================================================
			VCS_MEDIA_API void SetTestMode(bool enable);
			VCS_MEDIA_API bool IsTestMode() const;

		private:
			struct Impl;
			std::unique_ptr<Impl> impl_;
		};

	} // namespace media
} // namespace vcs
