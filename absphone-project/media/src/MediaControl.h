#pragma once

#include "IMediaControl.h"
#include "VideoProcessor.h"
#include "AudioProcessor.h"
#include "RTPEngine.h"
#include "IRtpTransport.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <shared_mutex>

namespace vcs {
	namespace base {
		// IConfig 前向声明
	}
}

// 前向声明，避免循环依赖
namespace vcs {
	namespace media {

		class MediaControl;
		class MediaTransportCallback;
		class MediaWorker;
		class MediaDeviceManager;
		class IPlatformMediaFactory;

		// ============================================================
		// Per-conference media-resource bundle
		//
		// 保留 confId 字段（终端只有一个会话，confId 就是 sessionId）。
		// 删除了 MediaTick 字段（终端没有音频混音 tick）。
		// ============================================================
		struct ConferenceMediaResource {
			std::string confId;
			vcs::media_config::MediaResourceConfig config;
			std::chrono::steady_clock::time_point createdAt;

			// 子系统
			std::shared_ptr<RTPEngine>      rtpEngine;
			std::shared_ptr<VideoProcessor> videoProc;
			std::shared_ptr<AudioProcessor> audioProc;

			// 接收方向异步解耦（保留）
			std::shared_ptr<MediaWorker>    worker;

			// 接收视频帧 SPSC 队列
			std::shared_ptr<VideoFrameQueuePool> videoQueuePool;

			// ============================================================
			// 媒体信息（排障拉取）
			//
			// live：正在运行的 sessionId。统计真值在 RTPEngine 持有的
			//       MediaStatisticsCollector 里，这里只记 key。
			// finalInfo：StopMedia 时抓下的最终快照；挂断后仍可查询
			//       （legacy 挂断后仍显示最后一次媒体信息）。下一次
			//       StartMedia 清零。
			//
			// 两者都由 infoMutex 保护：写者是会议执行线程（Start/Stop），
			// 读者是 UI 线程（每秒轮询）。锁内不做任何外部调用。
			// ============================================================
			mutable std::mutex infoMutex;
			std::string sessionId;
			vcs::MediaInfoSnapshot finalInfo;
		};

		/**
		 * @brief Media transport callback
		 *
		 * 接收方向：gateway 的 IRtpTransportCallback → RTPEngine
		 * 异步路径：worker 非空时入本 kind 对应的 SPSC lane，worker 线程调 RTPEngine
		 * 同步路径：worker 为空时直接调 RTPEngine
		 *
		 * kind 必须由构造方给定：一个 callback 只服务一个 transport，而一个
		 * transport 只对应一个 kind，这是 MediaWorker 每条 lane 单生产者
		 * 契约的前提。
		 */
		class MediaTransportCallback : public vcs::IRtpTransportCallback {
		public:
			explicit MediaTransportCallback(RTPEngine* rtpEngine,
				MediaWorker* worker,
				vcs::MediaStreamKind kind,
				std::string memberId,
				std::string conferenceId);
			~MediaTransportCallback() override;

			const char* GetClassName() const { return "MediaTransportCallback"; }

			void OnRtpReceived(const vcs::RtpPacket& packet,
				const vcs::SessionContext& context) override;
			void OnRtcpReceived(const vcs::RtcpPacket& packet,
				const vcs::SessionContext& context) override;
			void OnTransportConnected(const vcs::SessionContext& context) override;
			void OnTransportDisconnected(const vcs::SessionContext& context,
				const std::string& reason) override;
			void OnTransportError(const vcs::SessionContext& context,
				int errorCode,
				const std::string& errorMessage) override;

		private:
			RTPEngine* rtpEngine_;
			MediaWorker* worker_;
			vcs::MediaStreamKind kind_;
			std::string memberId_;
			std::string conferenceId_;

			// Resolved at construction, after RTPEngine::AddMember. Keeps the
			// per-packet receive path off MediaStatisticsCollector's mutex, which
			// the 1 Hz tick and snapshot queries also take - that contention sat
			// directly on pjmedia's single ioqueue.
			std::shared_ptr<MediaStatisticsCollector::StreamCounter> statsCounter_;
		};

		/**
		 * @brief Media control implementation（终端版）
		 *
		 * 保留：
		 *   - Impl 结构、ConferenceMediaResource、MediaTransportCallback
		 *   - SetupMediaResources / TeardownMediaResources / HasMediaResources
		 *   - SetObserver / GetObserver
		 *   - SetConfig
		 *   - SetConferenceCodecConfig / GetConferenceCodecConfig
		 *   - OnMemberCodecNegotiated
		 *   - GetMemberMediaInfo / GetConferenceMediaInfo
		 *
		 * 替换：
		 *   - AddMemberStreams  -> StartMedia(confId, sessionId, transports)
		 *   - RemoveMemberStream -> StopMedia(confId, sessionId)
		 *
		 * 删除：
		 *   - CreateConference / DestroyConference
		 *   - SetLayout / GetLayout
		 *   - 所有 recording 方法
		 *   - 所有 [AbsPhone Extension] 方法
		 *   - MediaTick 字段和调用
		 *
		 * 新增：
		 *   - ListVideoSources / ListAudioSources / ListVideoSinks / ListAudioSinks
		 *   - 构造函数接受 IPlatformMediaFactory
		 */
		class MediaControl : public IMediaControl {
		public:
			// 构造函数接受平台工厂，由 MediaFactory::CreateMediaControl 注入。
			// platformFactory 为 null 时 deviceManager_ 不可用（仅用于测试）。
			explicit MediaControl(std::shared_ptr<IPlatformMediaFactory> platformFactory);
			~MediaControl() override;

			MediaControl(const MediaControl&) = delete;
			MediaControl& operator=(const MediaControl&) = delete;

			// ============================================================
			// IMediaControl implementation
			// ============================================================

			// Codec configuration
			Response SetConferenceCodecConfig(
				const std::string& confId,
				const vcs::media_config::VideoCodecConfig& config) override;
			vcs::media_config::VideoCodecConfig GetConferenceCodecConfig(
				const std::string& confId) const override;

			// Media lifecycle（替换 AddMemberStreams / RemoveMemberStream）
			Response StartMedia(
				const std::string& confId,
				const std::string& sessionId,
				const MemberMediaTransports& transports) override;
			Response StopMedia(
				const std::string& confId,
				const std::string& sessionId) override;

			// Codec negotiation
			void OnMemberCodecNegotiated(
				const std::string& confId,
				const std::string& memberId,
				const NegotiatedCodec& audioCodec,
				const NegotiatedCodec& videoCodec) override;

			// Media info（拷贝出；可在任意线程调用）
			Response GetMemberMediaInfo(const std::string& memberId,
				vcs::MediaInfoSnapshot& out) const override;
			Response GetConferenceMediaInfo(const std::string& confId,
				vcs::MediaInfoSnapshot& out) const override;

			// Per-conference resource lifecycle
			Response SetupMediaResources(
				const std::string& confId,
				const vcs::media_config::MediaResourceConfig& config) override;
			vcs::ErrorCode TeardownMediaResources(
				const std::string& confId) override;
			bool HasMediaResources(const std::string& confId) const override;

			// Observer
			void SetObserver(IMediaControlObserver* observer) override;
			IMediaControlObserver* GetObserver() const override;

			// 事件总线（只发布，不订阅）
			void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) override;

			// Configuration injection
			void SetConfig(std::shared_ptr<base::IConfig> config) override;

			// 设备查询
			std::vector<std::string> ListVideoSources() const override;
			std::vector<std::string> ListAudioSources() const override;
			std::vector<std::string> ListVideoSinks() const override;
			std::vector<std::string> ListAudioSinks() const override;

		private:
			struct Impl;
			std::unique_ptr<Impl> impl_;
		};

	} // namespace media
} // namespace vcs
