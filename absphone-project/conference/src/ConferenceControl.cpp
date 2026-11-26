#include "ConferenceControl.h"
#include "ILogger.h"

#include <exception>
#include <memory>

namespace vcs {
	namespace conference {

		namespace {
			// Short tag for the worker's diagnostics. Deliberately a local
			// switch: the dispatch thread must not call into base's registry,
			// and only four event types reach the worker.
			const char* TaskTag(vcs::EventType type) {
				switch (type) {
				case vcs::EventType::CallIncoming:      return "CallIncoming";
				case vcs::EventType::CallConnected:     return "CallConnected";
				case vcs::EventType::CallTerminated:    return "CallTerminated";
				case vcs::EventType::MediaStateChanged: return "MediaStateChanged";
				default:                                return "CallEvent";
				}
			}
		} // namespace

		ConferenceControl::ConferenceControl(vcs::gateway::IGatewayManager* gateway,
			vcs::media::IMediaControl* media)
			: service_(gateway, media),
			worker_(std::make_unique<ConferenceWorker>("conference")) {
		}

		ConferenceControl::~ConferenceControl() {
			// Idempotent: the host normally calls Shutdown() first (docs/08 §3).
			Shutdown();
		}

		void ConferenceControl::Shutdown() {
			if (stopped_.exchange(true)) {
				return;
			}
			// Subscription is an RAII handle but the bus may already be gone;
			// reset explicitly so we never touch a destroyed bus from the dtor.
			callEventsSub_.Reset();
			mediaEventsSub_.Reset();
			// Join the worker before touching the service: a task in flight is
			// usually holding Conference::mutex_ and calling into gateway/media,
			// which must finish before the service is torn down. This join is on
			// the caller's thread - never on the event-dispatch thread.
			//
			// The stop is bounded. On timeout a task is wedged inside the
			// service: joining would hang shutdown forever and destroying the
			// worker would call std::terminate, while tearing the service down
			// under the running task would be a use-after-free. So the worker
			// is abandoned (leaked on purpose) and the service is left intact;
			// the log line names the stuck task so the cause is diagnosable.
			if (!worker_->Stop()) {
				ConferenceWorker* wedged = worker_.release();
				VCS_CONFERENCE_LOG_ERROR(
					"[ConferenceControl] worker did not stop within its timeout; "
					"abandoning it (pending=%zu, rejected=%llu). Service teardown skipped "
					"because a task is still executing inside it.",
					wedged->PendingCount(),
					static_cast<unsigned long long>(wedged->RejectedCount()));
				return;
			}
			worker_.reset();
			service_.Shutdown();
		}

		void ConferenceControl::SetEventBus(std::shared_ptr<vcs::IEventBus> bus) {
			eventBus_ = std::move(bus);
		}

		void ConferenceControl::SetObserver(IConferenceObserver* observer) {
			observer_ = observer;
		}

		Response ConferenceControl::Initialize() {
			if (!eventBus_) {
				return Response::Error(ResponseCode::InvalidOperation,
					"EventBus not injected");
			}
			// Worker before subscription: an event must never be submitted to
			// a worker that is not running yet.
			if (!worker_->Start()) {
				return Response::Error(ResponseCode::InternalError,
					"conference worker failed to start");
			}
			service_.SetEventBus(eventBus_);
			SubscribeEvents();
			// Level-triggered repair on the same serial thread as event tasks:
			// any event that was lost, duplicated or reordered converges here
			// within one period. It never queues up behind a slow task.
			worker_->SetPeriodic([this]() { ExecuteTick(); },
				std::chrono::seconds(1));
			VCS_CONFERENCE_LOG_INFO("[ConferenceControl] initialized");
			return Response::Success();
		}

		void ConferenceControl::SubscribeEvents() {
			vcs::EventFilter callFilter;
			callFilter.allowList.push_back(vcs::EventType::CallIncoming);
			callFilter.allowList.push_back(vcs::EventType::CallConnected);
			callFilter.allowList.push_back(vcs::EventType::CallTerminated);

			callEventsSub_ = eventBus_->Subscribe(callFilter,
				[this](const vcs::Event& ev) { OnCallEvent(ev); });

			// 媒体状态是收敛的第二个触发器（docs/02 §4、docs/06 §9.2）：
			// Started/Stopped 一到就立刻重收敛，收包门不必再等下一秒 tick。
			// 没有这条订阅，媒体起步与开门之间最多差一个 tick，这段时间的
			// 入向包全部丢在门外——视频丢首帧，音频起步晚一拍。
			// 事件内容不参与决策：真相仍从 GetConferenceMediaInfo 观察。
			vcs::EventFilter mediaFilter;
			mediaFilter.allowList.push_back(vcs::EventType::MediaStateChanged);
			mediaEventsSub_ = eventBus_->Subscribe(mediaFilter,
				[this](const vcs::Event& ev) { OnMediaStateEvent(ev); });
		}

		void ConferenceControl::OnCallEvent(const vcs::Event& ev) {
			if (stopped_.load()) {
				return;
			}
			// Runs on the single GLOBAL event-dispatch thread: do nothing but
			// hand the event over. The 7-step media handshake must never run
			// here (a slow subscriber stalls every event in the process, and any
			// blocking join here is a deadlock waiting to happen).
			// A rejection (worker stopping, or its bounded queue full) is
			// logged by the worker itself with the task name; the event is not
			// processed, and the log is the only record of that.
			vcs::Event copy = ev;
			worker_->Submit(TaskTag(ev.type), [this, copy]() { ExecuteCallEvent(copy); });
		}

		void ConferenceControl::OnMediaStateEvent(const vcs::Event& ev) {
			if (stopped_.load()) {
				return;
			}
			// Same dispatch-thread contract as call events: submit and return.
			// The event is deliberately not carried into the task - a media
			// state is a "converge now" hint, not a call fact; Converge reads
			// the truth from media instead of from this payload.
			worker_->Submit(TaskTag(ev.type),
				[this]() { ExecuteConverge(ConvergeSource::MediaHint); });
		}

		void ConferenceControl::ExecuteCallEvent(const vcs::Event& ev) {
			if (stopped_.load()) {
				return;
			}
			DispatchNotifications(service_.Converge(&ev, ConvergeSource::CallFact));
		}

		void ConferenceControl::ExecuteConverge(ConvergeSource source) {
			if (stopped_.load()) {
				return;
			}
			DispatchNotifications(service_.Converge(nullptr, source));
		}

		void ConferenceControl::ExecuteTick() {
			ExecuteConverge(ConvergeSource::PeriodicTick);
		}

		void ConferenceControl::DispatchNotifications(
			const std::vector<CallEventNotification>& notifications) {
			IConferenceObserver* observer = observer_;
			if (!observer) {
				return;
			}
			for (const auto& notification : notifications) {
				switch (notification.kind) {
				case CallEventKind::Incoming:
					observer->OnIncomingCall(notification.remoteUri, notification.hasVideo);
					break;
				case CallEventKind::Connected:
					observer->OnCallConnected(notification.remoteUri, notification.hasVideo);
					break;
				case CallEventKind::Terminated:
					observer->OnCallTerminated(notification.termination);
					break;
				default:
					break;
				}

				if (notification.hasMediaError) {
					observer->OnMediaError(notification.mediaError);
				}
			}
		}

		Response ConferenceControl::MakeCall(const std::string& remoteUri, bool enableVideo) {
			std::string pureUri = remoteUri;
			if (pureUri.rfind("SIP:", 0) == 0 || pureUri.rfind("sip:", 0) == 0) {
				pureUri = pureUri.substr(4);
			}
			if (pureUri.empty()) {
				return Response::Error(ResponseCode::InvalidParams, "remoteUri is empty");
			}
			return service_.MakeCall(pureUri, enableVideo);
		}

		Response ConferenceControl::HangupCall() {
			return service_.HangupCall();
		}

		Response ConferenceControl::AnswerCall(bool accept) {
			return service_.AnswerCall(accept);
		}

		Response ConferenceControl::SendDTMF(const std::string& digits) {
			if (digits.empty()) {
				return Response::Error(ResponseCode::InvalidParams, "digits is empty");
			}
			return service_.SendDTMF(digits);
		}

		Response ConferenceControl::GetMediaInfo(vcs::MediaInfoSnapshot& out) {
			if (stopped_.load()) {
				return Response::Error(ResponseCode::InvalidOperation,
					"conference control is shut down");
			}
			return service_.GetMediaInfo(out);
		}

	} // namespace conference
} // namespace vcs
