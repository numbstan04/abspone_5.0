#pragma once

#include "media_export.h"
#include "ErrorCode.h"
#include "Response.h"
#include "EventTypes.h"
#include "MediaConfig.h"
#include "MediaStreamKind.h"
#include "IRtpTransport.h"
#include "NegotiatedCodec.h"
#include "MediaInfoSnapshot.h"
#include "IConfig.h"

#include <memory>
#include <string>
#include <list>
#include <vector>
#include <map>

namespace vcs {

	namespace base {
		class IConfig;
	}

	class IEventBus;

	namespace media {

		// ============================================================
		// 一组 per-kind 媒体传输
		//
		// 唯一定义在 common/IRtpTransport.h（docs/09 §1.2）；
		// 此处仅保留 vcs::media 限定名，兼容既有引用。
		// ============================================================
		using vcs::MemberMediaTransports;

		// ============================================================
		// Negotiated codec information
		//
		// 唯一定义在 common/NegotiatedCodec.h；gateway 产出、conference
		// 中转、media 消费。此处仅保留 vcs::media 限定名，兼容既有引用。
		// ============================================================
		using vcs::NegotiatedCodec;

		// ============================================================
		// Media observer
		//
		// Notifications only, and only for once-per-negotiation / once-per-
		// stream-change facts. Periodic media information is NOT pushed: it is
		// pulled through GetConferenceMediaInfo(), so no business work ever
		// runs on the media worker thread that owns audio/video dispatch.
		// ============================================================
		class IMediaControlObserver {
		public:
			virtual ~IMediaControlObserver() = default;

			virtual void OnMediaStreamStateChanged(
				const std::string& confId,
				const std::string& memberId,
				const std::string& streamType,
				bool isActive) = 0;

			virtual void OnMemberCodecNegotiated(
				const std::string& confId,
				const std::string& memberId,
				const NegotiatedCodec& audioCodec,
				const NegotiatedCodec& videoCodec) = 0;
		};

		/**
		 * @brief Media control interface（终端版）
		 *
		 * 媒体模块入口。终端只有一个会话，不再有会议/成员/混流语义。
		 *
		 * 保留：
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
		 *   - StartRecording / StopRecording / PauseRecording / ResumeRecording
		 *   - 所有 [AbsPhone Extension] mixer 接口
		 *
		 * 新增：
		 *   - ListVideoSources / ListAudioSources / ListVideoSinks / ListAudioSinks
		 *     （设备查询，设备管理在 MediaDeviceManager 内部）
		 */
		class IMediaControl {
		public:
			virtual ~IMediaControl() = default;

			// ============================================================
			// Codec configuration
			// ============================================================
			virtual Response SetConferenceCodecConfig(
				const std::string& confId,
				const vcs::media_config::VideoCodecConfig& config) = 0;

			virtual vcs::media_config::VideoCodecConfig GetConferenceCodecConfig(
				const std::string& confId) const = 0;

			// ============================================================
			// Media lifecycle
			//
			// StartMedia：
			//   1) 校验 transports 非空、非 null
			//   2) 取/建会话媒体上下文（key = confId）
			//   3) 分配 SSRC，对每个 transport SetSsrc
			//   4) rtpEngine->AddMember(sessionId, ssrc, transports)
			//   5) 从 MediaDeviceManager 解析 sourceId / sinkId
			//      audioProcessor->Start(sessionId, resolvedAudio)
			//      videoProcessor->Start(sessionId, resolvedVideoStreams)
			//   6) 每个 kind 创建 MediaTransportCallback + SetCallback
			//   7) rtpEngine->BeginReceive()（RTPEngine 内部门）
			// 收包门（逐 transport StartReceiving）不在 media：由 gateway 的
			// StartTransport 在本方法成功返回后把守（docs/09 §2.2）。
			//
			// StopMedia：
			//   1) 每个 transport StopReceiving()
			//   2) videoProcessor->Stop() / audioProcessor->Stop()
			//   3) rtpEngine->RemoveMember(sessionId)
			//
			// 中途增删流（如辅流中途出现/结束）由 MediaControl 内部在
			// re-INVITE / SSRC 更新时调用 videoProcessor->AddStream /
			// RemoveStream，不暴露到 IMediaControl 接口上。
			// ============================================================
			virtual Response StartMedia(
				const std::string& confId,
				const std::string& sessionId,
				const MemberMediaTransports& transports) = 0;

			virtual Response StopMedia(
				const std::string& confId,
				const std::string& sessionId) = 0;

			// ============================================================
			// Codec negotiation
			// ============================================================
			virtual void OnMemberCodecNegotiated(
				const std::string& confId,
				const std::string& memberId,
				const NegotiatedCodec& audioCodec,
				const NegotiatedCodec& videoCodec) = 0;

			// ============================================================
			// Media info（排障用，同步拉取）
			//
			// 拷贝出语义：结果写进调用方的 out，不返回任何指向媒体内部状态的
			// 指针，因此不存在会话结束后悬空的问题。可在任意线程调用（UI 每秒
			// 轮询），只做一次短路拷贝，不参与媒体派发。
			//
			// Success    out 已填充
			// NotFound   confId 未知，或该会议从未起过媒体
			//
			// 生命周期：StopMedia 之后仍可查询，此时返回的是挂断前的最终快照
			// （out.live == false），与旧系统挂断后仍显示最后一次媒体信息一致；
			// 下一通电话 StartMedia 会清掉它。
			//
			// 快照对每种流各给一条记录（audio / video / secondary video，
			// 会话带数据流时还有 data；不在会话里的流 active == false）：
			// 协商结果、累计收发计数、每秒速率、编解码尺寸、丢包率、
			// 首帧是否解出、FIR 次数、本地/远端地址。字段来源全部在
			// 运行管线里，没有"预留字段"；管线尚未提供来源的值保持 0
			// （当前是 jitterUs / packetsLost / rttMs，见交付报告）。
			// ============================================================
			virtual Response GetMemberMediaInfo(
				const std::string& memberId,
				vcs::MediaInfoSnapshot& out) const = 0;

			virtual Response GetConferenceMediaInfo(
				const std::string& confId,
				vcs::MediaInfoSnapshot& out) const = 0;

			// ============================================================
			// Per-conference resource lifecycle
			// ============================================================
			// SetupMediaResources 契约：
			//   Success        资源已就绪
			//   Conflict       该 confId 已存在（调用方按幂等处理）
			//   InvalidParams  confId 为空
			//   InternalError  底噪可区分：message 指明 video/audio processor
			virtual Response SetupMediaResources(
				const std::string& confId,
				const vcs::media_config::MediaResourceConfig& config) = 0;

			virtual vcs::ErrorCode TeardownMediaResources(
				const std::string& confId) = 0;

			virtual bool HasMediaResources(const std::string& confId) const = 0;

			// ============================================================
			// Observer / configuration
			// ============================================================
			// 事件总线注入（由 absphone 组合根注入同一实例）。media 只发布、
			// 不订阅；未注入时事件被丢弃。
			virtual void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) = 0;

			virtual void SetObserver(IMediaControlObserver* observer) = 0;
			virtual IMediaControlObserver* GetObserver() const = 0;

			virtual void SetConfig(std::shared_ptr<base::IConfig> config) = 0;

			// ============================================================
			// 设备查询（设备管理在媒体模块内部，上层不注册设备）
			// ============================================================
			virtual std::vector<std::string> ListVideoSources() const = 0;
			virtual std::vector<std::string> ListAudioSources() const = 0;
			virtual std::vector<std::string> ListVideoSinks() const = 0;
			virtual std::vector<std::string> ListAudioSinks() const = 0;
		};

	} // namespace media
} // namespace vcs
