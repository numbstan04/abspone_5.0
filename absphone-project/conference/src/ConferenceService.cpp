#include "ConferenceService.h"
#include "ILogger.h"

namespace vcs { namespace conference {

namespace {

// SessionManager defaults every session without an explicit conference id to
// "local"; the terminal runs a single such conference.
constexpr const char* kDefaultConferenceId = "local";

} // namespace

ConferenceService::ConferenceService(vcs::gateway::IGatewayManager* gateway,
                                     vcs::media::IMediaControl* media)
	: gateway_(gateway)
	, media_(media) {
}

ConferenceService::~ConferenceService() {
	Shutdown();
}

void ConferenceService::Shutdown() {
	// 三句式：锁内换指针，锁外析构旧对象。
	// docs/08 §3 第 3 步：析构前显式收尾（停传输 + 停媒体，幂等），
	// 此时 gateway/media 仍存活。
	std::shared_ptr<Conference> retired;
	{
		std::lock_guard<std::mutex> lock(currentMutex_);
		retired = std::move(current_);
		current_.reset();
		joined_ = false;
	}
	if (retired) {
		retired->Shutdown();
	}
}

void ConferenceService::SetEventBus(std::shared_ptr<vcs::IEventBus> bus) {
	// Wiring happens before any call traffic; no lock needed (docs/08 §2.8 E).
	eventBus_ = bus.get();
}

std::shared_ptr<Conference> ConferenceService::EnsureJoinedLocked() {
	if (!joined_) {
		current_ = std::make_shared<Conference>(kDefaultConferenceId,
			gateway_, media_, eventBus_);
		joined_ = true;
		VCS_CONFERENCE_LOG_INFO("[ConferenceService] joined conference %s",
			kDefaultConferenceId);
	}
	return current_;
}

Response ConferenceService::MakeCall(const std::string& remoteUri, bool enableVideo) {
	std::shared_ptr<Conference> conf;
	{
		std::lock_guard<std::mutex> lock(currentMutex_);
		conf = EnsureJoinedLocked();
	}
	return conf->MakeCall(remoteUri, enableVideo);
}

Response ConferenceService::AnswerCall(bool accept) {
	std::shared_ptr<Conference> conf;
	{
		std::lock_guard<std::mutex> lock(currentMutex_);
		conf = EnsureJoinedLocked();
	}
	return conf->AnswerCall(accept);
}

Response ConferenceService::HangupCall() {
	std::shared_ptr<Conference> conf;
	{
		std::lock_guard<std::mutex> lock(currentMutex_);
		conf = EnsureJoinedLocked();
	}
	return conf->HangupCall();
}

Response ConferenceService::SendDTMF(const std::string& digits) {
	std::shared_ptr<Conference> conf;
	{
		std::lock_guard<std::mutex> lock(currentMutex_);
		conf = EnsureJoinedLocked();
	}
	return conf->SendDTMF(digits);
}

Response ConferenceService::GetMediaInfo(vcs::MediaInfoSnapshot& out) {
	// 只读：不加 currentMutex_ 里的建对象路径，不投递执行器。
	// 取到 shared_ptr 即放锁，快照装配在会议锁/media 里各自完成。
	std::shared_ptr<Conference> conf;
	{
		std::lock_guard<std::mutex> lock(currentMutex_);
		conf = current_;
	}
	if (!conf) {
		return Response::Error(ResponseCode::NotFound, "no conference");
	}
	return conf->GetMediaInfo(out);
}

std::vector<CallEventNotification> ConferenceService::Converge(const vcs::Event* trigger,
                                                             ConvergeSource source) {
	std::shared_ptr<Conference> conf;
	{
		std::lock_guard<std::mutex> lock(currentMutex_);
		conf = trigger ? EnsureJoinedLocked() : current_;
	}

	if (!conf) {
		// tick 自愈：会议对象还没建，但信令上确实有一通电话在跑
		// （事件全丢的极端情形），补建对象让它被接管。
		// 真相读取放在 currentMutex_ 之外，锁内只建对象。
		bool liveCall = false;
		if (gateway_) {
			for (const auto& view : gateway_->GetActiveSessions()) {
				if (IsLiveCallState(view.state)) {
					liveCall = true;
					break;
				}
			}
		}
		if (!liveCall) {
			// 空闲终端：每 1 秒只做一次空的会话列表读取，不建对象、不碰媒体。
			return std::vector<CallEventNotification>();
		}
		std::lock_guard<std::mutex> lock(currentMutex_);
		conf = EnsureJoinedLocked();
	}

	return conf->Converge(trigger, source);
}

}} // namespace vcs::conference
