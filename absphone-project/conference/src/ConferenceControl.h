#pragma once

#include "IConferenceControl.h"
#include "IConferenceObserver.h"
#include "ConferenceService.h"
#include "ConferenceWorker.h"
#include "EventBus.h"

#include <atomic>
#include <memory>
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

		// Terminal call control (interface implementation).
		//
		// Degraded to "validate -> service -> return" (docs/08 §2.2):
		//   - owns the event subscriptions (call events + MediaStateChanged)
		//     and forwards them to ConferenceService
		//   - hands ConferenceService's notifications to IConferenceObserver
		//   - keeps no conference state copies; never calls gateway/media directly
		//
		// Threading contract (docs/02 §4):
		//   The bus handler runs on the GLOBAL event-dispatch thread, so it only
		//   hands the event to worker_. All business - the media handshake,
		//   StopMedia()'s join, observer callbacks - runs on the per-conference
		//   worker thread. A slow media path can therefore never delay events
		//   for the rest of the system, and no cross-module lock is ever taken
		//   on the dispatch thread. MediaStateChanged is only a wakeup: it makes
		//   the media handshake and the receive gate converge immediately
		//   instead of waiting for the 1 s tick.
		//
		// Call order: SetEventBus() -> SetObserver() -> Initialize().
		class ConferenceControl : public IConferenceControl {
		public:
			explicit ConferenceControl(vcs::gateway::IGatewayManager* gateway,
			                           vcs::media::IMediaControl* media = nullptr);
			~ConferenceControl() override;

			void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) override;
			void SetObserver(IConferenceObserver* observer) override;
			Response Initialize() override;
			void Shutdown() override;

			Response MakeCall(const std::string& remoteUri, bool enableVideo) override;
			Response HangupCall() override;
			Response AnswerCall(bool accept) override;
			Response SendDTMF(const std::string& digits) override;

			// 媒体信息：只做一次短路转发。查询不投递到工作线程——工作线程上
			// 可能正压在 StartMedia/StopMedia 上，UI 每秒轮询不该排队等它。
			Response GetMediaInfo(vcs::MediaInfoSnapshot& out) override;

		private:
			void SubscribeEvents();
			// Dispatch thread: must stay O(1) and lock-free towards services.
			void OnCallEvent(const vcs::Event& ev);
			// Dispatch thread: media state is a wakeup only (docs/06 §9.2).
			void OnMediaStateEvent(const vcs::Event& ev);
			// Worker thread: the real work, serialised per conference.
			void ExecuteCallEvent(const vcs::Event& ev);
			// Worker thread, once per second, only when the queue is empty:
			// level-triggered reconciliation. This is the repair path for any
			// lost, duplicated or reordered event.
			void ExecuteTick();
			// Worker thread: converge with no trigger event. Shared by the
			// tick (PeriodicTick) and the media-state wakeup (MediaHint); the
			// media event carries no fact the decision needs, so passing it as
			// a trigger would only invite a payload read it was never meant to
			// serve. The source decides what the pass may conclude.
			void ExecuteConverge(ConvergeSource source);
			// Worker thread: hand Converge()'s notifications to the observer.
			void DispatchNotifications(
				const std::vector<CallEventNotification>& notifications);

			// Wired before Initialize() and never replaced afterwards; no lock
			// (conference/src keeps exactly the two locks of docs/08 §2.8 A).
			std::shared_ptr<vcs::IEventBus> eventBus_;
			IConferenceObserver* observer_ = nullptr;
			vcs::Subscription callEventsSub_;
			vcs::Subscription mediaEventsSub_;

			// Shutdown 后置位：派发线程/工作线程上的在途任务立即返回。
			std::atomic<bool> stopped_{ false };

			// Declared after service_ so it is destroyed FIRST: the worker must
			// never outlive the service whose tasks it runs. Held through a
			// pointer so Shutdown can abandon it when a task is wedged - joining
			// such a thread would hang process shutdown, and destroying it would
			// call std::terminate on a joinable thread.
			ConferenceService service_;
			std::unique_ptr<ConferenceWorker> worker_;
		};

	} // namespace conference
} // namespace vcs
