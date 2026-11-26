#pragma once

#include "conference_export.h"
#include "MediaConfig.h"
#include "IGatewayManager.h"
#include "IMediaControl.h"
#include "IConferenceObserver.h"
#include "EventBus.h"
#include "ErrorCode.h"
#include "Response.h"
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>

namespace vcs {
	namespace conference {

		// ============================================================
		// 终端会议配置（保留核心的编解码配置）
		// ============================================================
		struct ConferenceConfig {
			std::string name;

			// 编解码配置（实际编解码由 Media 模块实现）
			struct CodecConfig {
				std::string videoCodec = "H264";
				uint32_t videoWidth = 1280;
				uint32_t videoHeight = 720;
				uint32_t videoFramerate = 30;
				uint32_t videoBitrate = 1000000;
				uint32_t videoKeyFrameInterval = 60;

				std::string audioCodec = "G722";
				uint32_t audioSampleRate = 16000;
				uint32_t audioChannels = 1;
				uint32_t audioBitrate = 64000;

				bool videoEnabled = true;
				bool audioEnabled = true;
			} codec;
		};

		// ============================================================
		// 会议上下文（终端版的单一数据源，只记录自己的媒体配置）
		// ============================================================
		class ConferenceContext {
		public:
			explicit ConferenceContext(const std::string& confId);
			~ConferenceContext();

			const std::string& GetConferenceId() const { return conferenceId_; }
			uint64_t GetCreateTime() const { return createTime_; }

			void SetVideoCodecConfig(const vcs::media_config::VideoCodecConfig& config);
			vcs::media_config::VideoCodecConfig GetVideoCodecConfig() const;

			// 终端本地媒体状态（预留）
			void SetMicMuted(bool muted) { isMicMuted_ = muted; }
			bool IsMicMuted() const { return isMicMuted_; }

			void SetCameraMuted(bool muted) { isCameraMuted_ = muted; }
			bool IsCameraMuted() const { return isCameraMuted_; }

		private:
			std::string conferenceId_;
			uint64_t createTime_;
			vcs::media_config::VideoCodecConfig videoCodecConfig_;

			std::atomic<bool> isMicMuted_{ false };
			std::atomic<bool> isCameraMuted_{ false };
		};

		// ============================================================
		// 事件 → 通知（接口实现据此调用 IConferenceObserver）
		// ============================================================
		enum class CallEventKind {
			None,
			Incoming,
			Connected,
			Terminated
		};

		struct CallEventNotification {
			CallEventKind kind = CallEventKind::None;
			std::string remoteUri;
			bool hasVideo = false;
			CallTerminationInfo termination;
			bool hasMediaError = false;
			MediaErrorInfo mediaError;
		};

		// 信令层面“这个会话还算一通电话”的唯一判据。
		// Disconnecting 已被排除：它表示拆链已经开始，若仍算存活，一通
		// 卡在 Disconnecting 的旧会话就会和下一通来电凑成“两个会话”，
		// 让收敛永远停在冲突态——正是本设计要根除的故障。
		inline bool IsLiveCallState(vcs::gateway::SessionState state) {
			switch (state) {
			case vcs::gateway::SessionState::IncomingPending:
			case vcs::gateway::SessionState::OutgoingPending:
			case vcs::gateway::SessionState::Connecting:
			case vcs::gateway::SessionState::Active:
				return true;
			case vcs::gateway::SessionState::Disconnecting:
			case vcs::gateway::SessionState::Disconnected:
			default:
				return false;
			}
		}

		// ============================================================
		// 收敛的触发类别（docs/02 §4：事实 / 提示 / 兜底）
		//
		// 类别不是"谁调的"这种实现细节，而是"这一次收敛允许写什么结论"：
		//   CallFact      呼叫事实事件到达：可发接通/终止通知，终止原因取自事件
		//   PeriodicTick  1 秒兜底：可补发终止（事实丢失时的自愈），原因用兜底文案
		//   MediaHint     媒体状态唤醒：只收敛媒体，不写任何呼叫生命周期结论
		//
		// 用类别而不是布尔，是为了让第三类触发出现时不必再加一个 flag：
		// 新增触发来源只能落进这三类之一，落不进就说明模型缺一条腿。
		// ============================================================
		enum class ConvergeSource {
			CallFact,
			PeriodicTick,
			MediaHint
		};

		// ============================================================
		// 会议聚合根（终端的“当前会话”实体）+ 会议域执行类
		//
		// 状态的真相只有一个所有者：信令事实在 gateway 的 SessionView，
		// 媒体事实在 media 的 MediaInfoSnapshot。本类不保存它们的副本用于
		// 决策——Converge() 每次都向所有者重新观察，只在观察到分歧时动作。
		// 事件（CallIncoming/Connected/Terminated）只是“立刻收敛一次”的
		// 触发器，丢了、重了、乱了都不影响结果；MediaStateChanged 是“立刻
		// 收敛媒体”的提示，同样只是理由；执行器每秒还会在空队列时补一次
		// tick，所以一次丢事件最多晚 1 秒收敛，且一定会收敛。
		//
		// 一把 mutex_ 串行化本会议的全部操作（docs/08 §2.8 A）；持锁期间
		// 允许调用 gateway/media——它们不认识会议类型，事件只入队，不会
		// 回头申请本锁。Converge 只在执行器线程上运行。
		// ============================================================
		class Conference {
		public:
			Conference(const std::string& id,
			           vcs::gateway::IGatewayManager* gateway,
			           vcs::media::IMediaControl* media,
			           vcs::IEventBus* eventBus = nullptr);
			~Conference();

			// 获取共享上下文
			std::shared_ptr<ConferenceContext> GetSharedContext() const { return sharedContext_; }

			// 基本信息
			const std::string& GetId() const { return id_; }
			const std::string& GetName() const { return name_; }
			const ConferenceConfig& GetConfig() const { return config_; }

			void SetName(const std::string& name);
			void SetConfig(const ConferenceConfig& config);

			// 时间戳
			std::chrono::system_clock::time_point GetCreateTime() const { return createTime_; }
			std::chrono::system_clock::time_point GetStartTime() const { return startTime_; }
			void SetStartTime();

			// ============================================================
			// 呼叫执行（服务类调用；最后一棒）
			// ============================================================
			Response MakeCall(const std::string& remoteUri, bool enableVideo);
			Response AnswerCall(bool accept);
			Response HangupCall();
			Response SendDTMF(const std::string& digits);

			// ============================================================
			// 媒体信息（排障拉取）
			//
			// 本类不缓存媒体数据：拿到 confId 就交给 media 的统计库拷出。
			// confId 在锁内取（避免 StopMedia 清掉后用到空串），media 调用在
			// 锁外进行——media 不回调会议层，但也没必要让 UI 轮询排在
			// StartMedia/StopMedia 后面。
			//
			// 通话结束后仍可查询：MediaControl 保留最终快照（out.live=false），
			// 所以这里记住 lastMediaConfId_，而不是清空重来。
			// ============================================================
			Response GetMediaInfo(vcs::MediaInfoSnapshot& out);

			// ============================================================
			// 收敛入口（执行器线程）。trigger 只是“现在收敛一次”的理由，
			// 决定动作的是 gateway/media 的当前事实，不是事件内容。
			// source 决定本次允许写的结论，见 ConvergeSource 的说明；
			// trigger 只在 CallFact 下用于终止通知的原因/地址，其他类别传 nullptr。
			//
			// 返回需要交给 IConferenceObserver 的通知：每个通知在同一个
			// 会话内至多产生一次（用已通知水位去重），媒体错误另按
			// (session, stage) 每次故障只报一次。
			// ============================================================
			std::vector<CallEventNotification> Converge(const vcs::Event* trigger,
			                                            ConvergeSource source);

			// docs/08 §3 第 3 步：析构/关机收尾——停传输 + 停媒体（幂等），
			// 不发起信令挂断。服务类在换指针后、对象析构前调用。
			void Shutdown();

		private:
			// 一次观察的结果：信令上说这通电话现在是什么样。
			struct ObservedCall {
				bool hasCall = false;
				bool ambiguous = false;          // 同时存在多个存活会话
				bool ending = false;             // 有会话处于 Disconnecting（拆链收尾中）
				vcs::gateway::SessionView view;  // hasCall 时有效
				std::string confId;              // 媒体侧会议号
			};

			// 向真相所有者重新读取（持锁调用；下层不会回调本类）。
			ObservedCall ObserveLocked();
			// 当前唯一存活会话号；0 个或多个时为空（调用方按“无电话”处理）。
			std::string ResolveSessionIdLocked() const;
			// 媒体资源号：本类起过媒体就用它，否则回落到本地会议号。
			std::string EffectiveConfIdLocked() const;

			// 观察 → 动作：把媒体收敛到“Active 就用当前会话的传输跑起来，
			// 其他状态什么都不跑”。无分歧时只做读，不产生任何副作用。
			MediaErrorInfo ConvergeStartLocked(const ObservedCall& observed);
			// force=true 用于关机：此时没有下一 tick，且调用方保证 gateway/media
			// 仍存活，必须当场停干净，不能因为“会话还在 SM 里”而等待。
			void ConvergeStopLocked(bool force = false);
			void ApplyCodecsLocked(const ObservedCall& observed);

			// 终止通知（去重）；返回是否真的产生了一条。
			bool EmitTerminatedLocked(std::vector<CallEventNotification>& out,
			                          const vcs::Event* trigger,
			                          const char* fallbackReason);
			void ClearCallLocked();

			// §2.9 D 的唯一上报出口：一行 ERROR 日志 + 一条 SystemError，
			// 返回 info 供接口层交给 IConferenceObserver::OnMediaError。
			struct MediaFailureReport {
				const char* stage = "";
				const char* op = "";
				std::string conferenceId;
				std::string sessionId;
				vcs::ErrorCode code = vcs::ErrorCode::InternalError;
				std::string message;
				int64_t elapsedMs = 0;
				std::vector<std::string> kinds;
				// §2.9 B 的等级列：StartTransport 为 WARNING，其余 ERROR
				vcs::EventSeverity severity = vcs::EventSeverity::Error;
			};
			MediaErrorInfo ReportMediaFailure(const MediaFailureReport& report);
			// 同一 (session, stage) 故障在恢复前只上报一次；tick 重试不再刷屏。
			MediaErrorInfo ReportMediaFailureOnceLocked(const MediaFailureReport& report);

		private:
			std::string id_;
			std::string name_;
			ConferenceConfig config_;

			std::chrono::system_clock::time_point createTime_;
			std::chrono::system_clock::time_point startTime_;

			std::shared_ptr<ConferenceContext> sharedContext_;

			// 下层（非拥有）
			vcs::gateway::IGatewayManager* gateway_ = nullptr;
			vcs::media::IMediaControl* media_ = nullptr;
			vcs::IEventBus* eventBus_ = nullptr;

			// 本域状态 + 会话索引（唯一写者，均受 mutex_ 保护）
			mutable std::mutex mutex_;

			// 最近一次观察采纳的会话（用于识别换会话、终止通知与 API 兜底）
			std::string activeSessionId_;
			std::string remoteUri_;
			bool hasVideo_ = false;
			int64_t connectTimeMs_ = 0;

			// 通知水位：记录“已经为哪个会话发过”，重复收敛不再重发
			std::string notifiedIncoming_;
			std::string notifiedConnected_;
			std::string notifiedTerminated_;

			// 已接入媒体的传输对象版本；与 SessionView 不一致即需要热切换
			uint32_t appliedTransportGeneration_ = 0;
			std::string startedConfId_;
			// 上一通电话的媒体资源号：挂断后媒体信息查询仍要用它
			// （MediaControl 保留了最终快照）。下一通 StartMedia 覆盖。
			std::string lastMediaConfId_;

			// 故障节流：同一 (session, stage) 一次故障只上报一次
			std::string failureKey_;
			uint32_t failureAttempts_ = 0;
			int64_t lastConflictLogMs_ = 0;
		};

	} // namespace conference
} // namespace vcs
