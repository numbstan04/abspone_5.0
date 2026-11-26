#pragma once

#include "IService.h"
#include "Response.h"
#include "Conference.h"
#include "EventBus.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vcs {
	namespace gateway {
		class IGatewayManager;
	}
	namespace media {
		class IMediaControl;
	}

	namespace conference {

		/**
		 * @brief 会议域服务类
		 *
		 * 只做"会议域操作的管理"：接收请求、决定先后与失败回滚、把最后一棒
		 * 交给执行类 Conference。不碰 gateway/media、不持状态副本、不建线程。
		 *
		 * 锁（docs/08 §2.8 A/B）：currentMutex_ 只保护 current_ 指针与建对象，
		 * 取到 shared_ptr 立即放锁，再到会议锁里做业务。
		 */
		class ConferenceService : public IService {
		public:
			ConferenceService(vcs::gateway::IGatewayManager* gateway,
			                  vcs::media::IMediaControl* media);
			~ConferenceService() override;

			const char* GetName() const override { return "ConferenceService"; }
			bool Initialize() override { return true; }
			void Shutdown() override;

			// 事件总线（失败上报用）。非拥有；必须比本服务活得久。
			void SetEventBus(std::shared_ptr<vcs::IEventBus> bus);

			// ============================================================
			// 会议域操作（接口实现调用）
			// ============================================================
			Response MakeCall(const std::string& remoteUri, bool enableVideo);
			Response AnswerCall(bool accept);
			Response HangupCall();
			Response SendDTMF(const std::string& digits);

			// 媒体信息（排障）：直接读当前会话的快照，不投递到执行器。
			// 未入会（从未打过电话）时返回 NotFound，不为了查询去建会议对象。
			Response GetMediaInfo(vcs::MediaInfoSnapshot& out);

			// ============================================================
			// 收敛入口（T1）
			//
			// 事件只是触发器：它的内容不进决策。PeriodicTick 时若还没有会议
			// 对象，只有信令上确实存在存活会话才会补建（事件全丢的自愈路径）。
			// source 决定本次能写什么结论，见 ConvergeSource：
			//   CallFact / PeriodicTick → 完整状态机
			//   MediaHint               → 只收敛媒体
			// ============================================================
			std::vector<CallEventNotification> Converge(const vcs::Event* trigger,
			                                            ConvergeSource source);

		private:
			// 仅锁内调用；只建对象，不做 I/O
			std::shared_ptr<Conference> EnsureJoinedLocked();

			mutable std::mutex currentMutex_;   // 只保护 current_
			std::shared_ptr<Conference> current_;
			bool joined_ = false;

			// 非拥有
			vcs::IEventBus* eventBus_ = nullptr;
			vcs::gateway::IGatewayManager* gateway_ = nullptr;
			vcs::media::IMediaControl* media_ = nullptr;
		};

	}
} // namespace vcs::conference
