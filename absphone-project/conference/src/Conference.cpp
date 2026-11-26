#include "Conference.h"
#include "ILogger.h"
#include "Json.h"
#include <algorithm>
#include <chrono>

namespace vcs {
	namespace conference {

		namespace {

			// Contract: upper layers normalize `SIP:xxx` -> `xxx` before calling
			// the execution class; the protocol adapter stays a gateway detail.
			constexpr const char* kCallProtocol = "SIP";

			// 终端只有一个本地会议；SessionManager 对未指定会议号的会话也
			// 用这个名字。
			constexpr const char* kLocalConferenceId = "local";

			int64_t SteadyNowMs() {
				return static_cast<int64_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count());
			}

			std::string JoinKinds(const std::vector<std::string>& kinds) {
				std::string joined;
				for (size_t i = 0; i < kinds.size(); ++i) {
					if (i > 0) {
						joined += ",";
					}
					joined += kinds[i];
				}
				return joined;
			}

			// docs/08 §2.9 C：ResponseCode → vcs::ErrorCode 的唯一映射
			vcs::ErrorCode MapResponseCode(ResponseCode code) {
				switch (code) {
				case ResponseCode::Success:          return vcs::ErrorCode::Success;
				case ResponseCode::InvalidParams:    return vcs::ErrorCode::InvalidParams;
				case ResponseCode::NotFound:         return vcs::ErrorCode::ResourceNotFound;
				case ResponseCode::Conflict:         return vcs::ErrorCode::ResourceConflict;
				case ResponseCode::InvalidOperation: return vcs::ErrorCode::MediaTransportNotReady;
				case ResponseCode::InternalError:    return vcs::ErrorCode::InternalError;
				case ResponseCode::NotImplemented:   return vcs::ErrorCode::NotImplemented;
				}
				return vcs::ErrorCode::InternalError;
			}

			std::vector<std::string> TransportKinds(
				const vcs::media::MemberMediaTransports& transports) {
				std::vector<std::string> kinds;
				for (const auto& kv : transports) {
					kinds.push_back(vcs::MediaStreamKindToString(kv.first));
				}
				return kinds;
			}

		} // namespace

		// ============================================================
		// ConferenceContext 实现（终端版）
		// ============================================================

		ConferenceContext::ConferenceContext(const std::string& confId)
			: conferenceId_(confId)
			, createTime_(static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count())) {
		}

		ConferenceContext::~ConferenceContext() {
			VCS_CONFERENCE_LOG_INFO("[ConferenceContext] Destroyed: %s", conferenceId_.c_str());
		}

		void ConferenceContext::SetVideoCodecConfig(const vcs::media_config::VideoCodecConfig& config) {
			videoCodecConfig_ = config;
		}

		vcs::media_config::VideoCodecConfig ConferenceContext::GetVideoCodecConfig() const {
			return videoCodecConfig_;
		}


		// ============================================================
		// Conference 实现（终端版）
		// ============================================================

		Conference::Conference(const std::string& id,
			vcs::gateway::IGatewayManager* gateway,
			vcs::media::IMediaControl* media,
			vcs::IEventBus* eventBus)
			: id_(id)
			, name_(id)
			, createTime_(std::chrono::system_clock::now())
			, sharedContext_(std::make_shared<ConferenceContext>(id))
			, gateway_(gateway)
			, media_(media)
			, eventBus_(eventBus) {
		}

		Conference::~Conference() {
			Shutdown();
		}

		void Conference::Shutdown() {
			std::lock_guard<std::mutex> guard(mutex_);
			// 关机没有下一 tick：必须当场停干净（docs/08 §3 第 3 步）。
			ConvergeStopLocked(true);
			ClearCallLocked();
		}

		void Conference::SetName(const std::string& name) {
			name_ = name;
		}

		void Conference::SetConfig(const ConferenceConfig& config) {
			config_ = config;
		}

		void Conference::SetStartTime() {
			startTime_ = std::chrono::system_clock::now();
		}

		// ============================================================
		// 呼叫执行
		// ============================================================

		Response Conference::MakeCall(const std::string& remoteUri, bool enableVideo) {
			std::lock_guard<std::mutex> guard(mutex_);

			if (!gateway_) {
				return Response::Error(ResponseCode::InternalError, "No Gateway");
			}

			vcs::Params params;
			params.SetBool("video_enabled", enableVideo);

			std::string sessionId;
			Response resp = gateway_->InitiateCall(kCallProtocol, remoteUri, params, sessionId);
			if (!resp.IsSuccess()) {
				VCS_CONFERENCE_LOG_ERROR("[Conference] MakeCall to %s failed: %s",
					remoteUri.c_str(), resp.GetMsg().c_str());
				return resp;
			}

			// 只记“谁”，不记“什么状态”：状态由 Converge 从 SessionView 观察。
			activeSessionId_ = sessionId;
			remoteUri_ = remoteUri;
			hasVideo_ = enableVideo;
			connectTimeMs_ = 0;

			VCS_CONFERENCE_LOG_INFO("[Conference] MakeCall to %s, sessionId=%s",
				remoteUri.c_str(), sessionId.c_str());
			return resp;
		}

		Response Conference::AnswerCall(bool accept) {
			std::lock_guard<std::mutex> guard(mutex_);

			if (!gateway_) {
				return Response::Error(ResponseCode::InternalError, "No Gateway");
			}
			// 会话号从信令真相取，不用缓存的副本：CallIncoming 丢了也答得通。
			const std::string sessionId = ResolveSessionIdLocked();
			if (sessionId.empty()) {
				return Response::Error(ResponseCode::InvalidOperation, "No active call");
			}
			return gateway_->AnswerCall(sessionId, accept, "User rejected");
		}

		Response Conference::HangupCall() {
			std::lock_guard<std::mutex> guard(mutex_);

			if (!gateway_) {
				return Response::Error(ResponseCode::InternalError, "No Gateway");
			}
			const std::string sessionId = ResolveSessionIdLocked();
			if (sessionId.empty()) {
				return Response::Error(ResponseCode::InvalidOperation, "No active call");
			}

			Response resp = gateway_->HangupCall(sessionId, "Local hangup");
			if (!resp.IsSuccess()) {
				VCS_CONFERENCE_LOG_ERROR("[Conference] HangupCall failed: %s",
					resp.GetMsg().c_str());
			}
			return resp;
		}

		Response Conference::SendDTMF(const std::string& digits) {
			std::lock_guard<std::mutex> guard(mutex_);

			if (!gateway_) {
				return Response::Error(ResponseCode::InternalError, "No Gateway");
			}
			const std::string sessionId = ResolveSessionIdLocked();
			if (sessionId.empty()) {
				return Response::Error(ResponseCode::InvalidOperation, "No active call");
			}

			Response resp = gateway_->SendDTMF(sessionId, digits);
			if (!resp.IsSuccess()) {
				VCS_CONFERENCE_LOG_ERROR("[Conference] SendDTMF failed: %s",
					resp.GetMsg().c_str());
			}
			return resp;
		}

		Response Conference::GetMediaInfo(vcs::MediaInfoSnapshot& out) {
			std::string confId;
			{
				std::lock_guard<std::mutex> guard(mutex_);
				if (!media_) {
					return Response::Error(ResponseCode::InternalError, "No Media");
				}
				// 通话中优先用当前资源号；挂断后回落到上一通的资源号，
				// MediaControl 会在那里保留最终快照。
				confId = !startedConfId_.empty() ? startedConfId_ : lastMediaConfId_;
			}
			if (confId.empty()) {
				return Response::Error(ResponseCode::NotFound, "no media session");
			}
			return media_->GetConferenceMediaInfo(confId, out);
		}

		// ============================================================
		// 观察（真相来自 gateway / media，本类不保存用于决策的副本）
		// ============================================================

		Conference::ObservedCall Conference::ObserveLocked() {
			ObservedCall observed;
			if (!gateway_) {
				return observed;
			}

			size_t liveCount = 0;
			std::vector<std::string> ids;
			for (const auto& view : gateway_->GetActiveSessions()) {
				if (view.state == vcs::gateway::SessionState::Disconnecting) {
					// 拆链收尾中：既不算存活（不触发动作），也不当作已结束
					// （通知要等 CallTerminated 带着完整 reason 到达）。
					observed.ending = true;
					continue;
				}
				if (!IsLiveCallState(view.state)) {
					continue;
				}
				++liveCount;
				ids.push_back(view.sessionId);
				observed.view = view;
			}

			if (liveCount == 0) {
				return observed;
			}
			if (liveCount > 1) {
				// 两个存活会话时，任何选择都可能是错的那一个：停手等它们
				// 自己收敛，只按 5 秒节流留证据。
				observed.ambiguous = true;
				const int64_t nowMs = SteadyNowMs();
				if (lastConflictLogMs_ == 0 || nowMs - lastConflictLogMs_ >= 5000) {
					lastConflictLogMs_ = nowMs;
					VCS_CONFERENCE_LOG_ERROR(
						"[Conference] %zu live sessions (%s); refusing to act until one ends",
						liveCount, JoinKinds(ids).c_str());
				}
				return observed;
			}

			observed.hasCall = true;
			observed.confId = observed.view.conferenceId.empty()
				? std::string(kLocalConferenceId)
				: observed.view.conferenceId;
			return observed;
		}

		std::string Conference::ResolveSessionIdLocked() const {
			if (!gateway_) {
				return std::string();
			}
			std::string found;
			size_t liveCount = 0;
			for (const auto& view : gateway_->GetActiveSessions()) {
				if (!IsLiveCallState(view.state)) {
					continue;
				}
				++liveCount;
				found = view.sessionId;
			}
			return liveCount == 1 ? found : std::string();
		}

		std::string Conference::EffectiveConfIdLocked() const {
			if (!startedConfId_.empty()) {
				return startedConfId_;
			}
			if (!lastMediaConfId_.empty()) {
				return lastMediaConfId_;
			}
			return kLocalConferenceId;
		}

		// ============================================================
		// 收敛
		// ============================================================

		std::vector<CallEventNotification> Conference::Converge(const vcs::Event* trigger,
			ConvergeSource source) {
			std::vector<CallEventNotification> out;
			std::lock_guard<std::mutex> guard(mutex_);

			const ObservedCall observed = ObserveLocked();
			if (observed.ambiguous) {
				return out;
			}

			// 提示类：只收敛媒体。它可能跑在事实事件前面，生命周期结论
			// （终止/接通通知、会话水位）一律不在这里写——写了会被水位去重，
			// 等事实到达时真实 reason/地址已经丢了。
			if (source == ConvergeSource::MediaHint) {
				if (!observed.hasCall) {
					if (!observed.ending) {
						ConvergeStopLocked();
					}
					return out;
				}
				if (observed.view.state == vcs::gateway::SessionState::Active) {
					const MediaErrorInfo error = ConvergeStartLocked(observed);
					if (!error.stage.empty()) {
						// 媒体故障是媒体事实，不是呼叫生命周期结论。
						CallEventNotification note;
						note.hasMediaError = true;
						note.mediaError = error;
						out.push_back(note);
					}
				}
				else {
					ConvergeStopLocked();
				}
				return out;
			}

			if (!observed.hasCall) {
				if (observed.ending) {
					// 会话还在拆链收尾：等它从 SM 消失，让 CallTerminated 带着
					// gateway 的 reason/地址/通话时长到达，而不是在这里抢着
					// 用兜底原因发通知。SM 对 Disconnecting 有 5 秒超时兜底，
					// 所以这不会是永久等待；媒体也等它删完再停（避免并发 join）。
					return out;
				}
				ConvergeStopLocked();
				EmitTerminatedLocked(out, trigger, "signaling session gone");
				ClearCallLocked();
				return out;
			}

			const std::string& sessionId = observed.view.sessionId;

			if (!activeSessionId_.empty() && activeSessionId_ != sessionId) {
				// 换了一通电话：上一通的结束通知没有事件可依赖，在这里补。
				EmitTerminatedLocked(out, nullptr, "superseded");
			}
			if (activeSessionId_ != sessionId) {
				activeSessionId_ = sessionId;
				appliedTransportGeneration_ = 0;
				failureKey_.clear();
				failureAttempts_ = 0;
			}

			if (remoteUri_.empty()) {
				remoteUri_ = !observed.view.caller.empty()
					? observed.view.caller : observed.view.callee;
			}
			hasVideo_ = observed.view.hasVideo;
			if (observed.view.connectTimeMs > 0) {
				connectTimeMs_ = observed.view.connectTimeMs;
			}

			switch (observed.view.state) {
			case vcs::gateway::SessionState::IncomingPending: {
				// 响铃中：媒体不启动；顺手收掉上一通可能残留的媒体。
				ConvergeStopLocked();
				if (notifiedIncoming_ != sessionId) {
					notifiedIncoming_ = sessionId;
					CallEventNotification note;
					note.kind = CallEventKind::Incoming;
					note.remoteUri = remoteUri_;
					note.hasVideo = hasVideo_;
					out.push_back(note);
				}
				break;
			}
			case vcs::gateway::SessionState::OutgoingPending:
			case vcs::gateway::SessionState::Connecting:
				// 信令还在协商：媒体不启动，也没有可通知的状态。
				ConvergeStopLocked();
				break;
			case vcs::gateway::SessionState::Active: {
				const MediaErrorInfo error = ConvergeStartLocked(observed);
				if (notifiedConnected_ != sessionId) {
					notifiedConnected_ = sessionId;
					CallEventNotification note;
					note.kind = CallEventKind::Connected;
					note.remoteUri = remoteUri_;
					note.hasVideo = hasVideo_;
					note.hasMediaError = !error.stage.empty();
					note.mediaError = error;
					out.push_back(note);
				}
				else if (!error.stage.empty()) {
					// 已经通知过“已接通”，之后的故障只作为媒体错误上报。
					CallEventNotification note;
					note.hasMediaError = true;
					note.mediaError = error;
					out.push_back(note);
				}
				break;
			}
			default:
				break;
			}
			return out;
		}

		void Conference::ClearCallLocked() {
			activeSessionId_.clear();
			remoteUri_.clear();
			hasVideo_ = false;
			connectTimeMs_ = 0;
			appliedTransportGeneration_ = 0;
			failureKey_.clear();
			failureAttempts_ = 0;
			// notified*_ 按会话号记水位：新会话号天然不会命中旧水位，
			// 不需要清空（清空反而可能让已报过的终止重复上报）。
		}

		bool Conference::EmitTerminatedLocked(std::vector<CallEventNotification>& out,
		                                      const vcs::Event* trigger,
		                                      const char* fallbackReason) {
			const std::string sessionId = !activeSessionId_.empty()
				? activeSessionId_
				: (trigger ? trigger->sessionId : std::string());
			if (sessionId.empty() || notifiedTerminated_ == sessionId) {
				return false;
			}
			notifiedTerminated_ = sessionId;

			CallEventNotification note;
			note.kind = CallEventKind::Terminated;
			note.termination.remoteUri = remoteUri_;

			vcs::base::Json payload;
			const bool fromTrigger = trigger && !trigger->sessionId.empty() &&
				trigger->sessionId == sessionId;
			if (fromTrigger && !trigger->payload.empty()) {
				vcs::base::Json::Parse(trigger->payload, payload);
			}

			if (fromTrigger) {
				note.termination.reason = payload.GetString("reason", fallbackReason);
				note.termination.remoteAddress = payload.GetString("remoteAddress");
				note.termination.durationSec = static_cast<uint32_t>(
					payload.GetInt("durationSec", 0));
			}
			else {
				note.termination.reason = fallbackReason;
			}

			if (note.termination.remoteUri.empty()) {
				note.termination.remoteUri = payload.GetString("remoteUri");
			}
			if (note.termination.durationSec == 0 && connectTimeMs_ > 0) {
				const int64_t talkMs = SteadyNowMs() - connectTimeMs_;
				if (talkMs > 0) {
					note.termination.durationSec = static_cast<uint32_t>(talkMs / 1000);
				}
			}

			out.push_back(note);
			return true;
		}

		// ============================================================
		// 媒体收敛（docs/08 §2.3：启动 7 步 / 停止 3 步）
		// ============================================================

		MediaErrorInfo Conference::ConvergeStartLocked(const ObservedCall& observed) {
			const int64_t startMs = SteadyNowMs();
			const std::string& sessionId = observed.view.sessionId;
			const std::string& confId = observed.confId;

			if (!gateway_ || !media_) {
				MediaFailureReport report;
				report.stage = "SetupResources";
				report.op = "SetupMediaResources";
				report.conferenceId = confId;
				report.sessionId = sessionId;
				report.code = vcs::ErrorCode::InternalError;
				report.message = !gateway_ ? "gateway not injected" : "media not injected";
				report.elapsedMs = SteadyNowMs() - startMs;
				return ReportMediaFailureOnceLocked(report);
			}

			// 步 1：媒体资源（参数来源在会议层，幂等；Conflict = 已存在）
			vcs::media_config::MediaResourceConfig cfg;
			cfg.enableAudio = true;
			cfg.enableVideo = true;
			cfg.audioSourceId = "mic:default";
			cfg.audioSinkId = "speaker:0";
			cfg.mainVideoSourceId = "camera:0";
			cfg.mainVideoSinkId = "main_window";

			Response setup = media_->SetupMediaResources(confId, cfg);
			if (!setup.IsSuccess() && setup.GetCode() != ResponseCode::Conflict) {
				MediaFailureReport report;
				report.stage = "SetupResources";
				report.op = "SetupMediaResources";
				report.conferenceId = confId;
				report.sessionId = sessionId;
				report.code = MapResponseCode(setup.GetCode());
				report.message = setup.GetMsg();
				report.elapsedMs = SteadyNowMs() - startMs;
				return ReportMediaFailureOnceLocked(report);
			}

			// 步 2：观察媒体真相（不读本地记账）
			std::string liveSession;
			{
				vcs::MediaInfoSnapshot snapshot;
				if (media_->GetConferenceMediaInfo(confId, snapshot).IsSuccess() && snapshot.live) {
					liveSession = snapshot.sessionId;
				}
			}

			if (!liveSession.empty() && liveSession != sessionId) {
				// 媒体还挂在别的会话上：先确认那个会话已经被 SM 删除。
				// 未删除（典型 Disconnecting）时停媒体会与 MarkTerminated 的
				// StopReceiving 并发，等下一 tick 再来。
				vcs::gateway::SessionView stale;
				if (gateway_->GetSessionView(liveSession, stale) && stale.hasSession) {
					return MediaErrorInfo();
				}
				Response closed = gateway_->StopTransport(liveSession);
				if (!closed.IsSuccess()) {
					VCS_CONFERENCE_LOG_WARNING(
						"[Conference] StopTransport(stale) failed: session=%s, msg=%s",
						liveSession.c_str(), closed.GetMsg().c_str());
				}
				Response moved = media_->StopMedia(confId, liveSession);
				if (!moved.IsSuccess()) {
					MediaFailureReport report;
					report.stage = "StopStaleMedia";
					report.op = "StopMedia";
					report.conferenceId = confId;
					report.sessionId = liveSession;
					report.code = MapResponseCode(moved.GetCode());
					report.message = moved.GetMsg();
					report.elapsedMs = SteadyNowMs() - startMs;
					return ReportMediaFailureOnceLocked(report);
				}
				appliedTransportGeneration_ = 0;
				VCS_CONFERENCE_LOG_WARNING(
					"[Conference] stale media on session=%s stopped before session=%s",
					liveSession.c_str(), sessionId.c_str());
				liveSession.clear();
			}

			const bool mediaLive = !liveSession.empty();
			const bool needStart = !mediaLive;
			const bool needSwap = mediaLive &&
				appliedTransportGeneration_ != observed.view.transportGeneration;
			if (!needStart && !needSwap) {
				return MediaErrorInfo();   // 已收敛：稳态只读
			}

			// 步 3：传输快照（没有它就起不了媒体）
			vcs::media::MemberMediaTransports transports;
			if (!gateway_->GetSessionTransports(sessionId, transports)) {
				MediaFailureReport report;
				report.stage = "TransportSnapshotNotFound";
				report.op = "GetSessionTransports";
				report.conferenceId = confId;
				report.sessionId = sessionId;
				report.code = vcs::ErrorCode::ResourceNotFound;
				report.message = "session has no media transports";
				report.elapsedMs = SteadyNowMs() - startMs;
				return ReportMediaFailureOnceLocked(report);
			}
			if (transports.empty()) {
				MediaFailureReport report;
				report.stage = "TransportSnapshotEmpty";
				report.op = "GetSessionTransports";
				report.conferenceId = confId;
				report.sessionId = sessionId;
				report.code = vcs::ErrorCode::MediaTransportNotReady;
				report.message = "media transport snapshot is empty";
				report.elapsedMs = SteadyNowMs() - startMs;
				return ReportMediaFailureOnceLocked(report);
			}

			// 步 4：起媒体（同一会话同传输 = 幂等；传输变了 = 原地热替换）
			Response started = media_->StartMedia(confId, sessionId, transports);
			if (!started.IsSuccess()) {
				MediaFailureReport report;
				report.stage = "StartMedia";
				report.op = "StartMedia";
				report.conferenceId = confId;
				report.sessionId = sessionId;
				report.code = MapResponseCode(started.GetCode());
				report.message = started.GetMsg();
				report.elapsedMs = SteadyNowMs() - startMs;
				report.kinds = TransportKinds(transports);
				return ReportMediaFailureOnceLocked(report);
			}

			// 步 5：紧随成功记账——失败重试与停止都靠它定位媒体资源
			appliedTransportGeneration_ = observed.view.transportGeneration;
			startedConfId_ = confId;
			lastMediaConfId_ = confId;

			// 步 6：编解码协商结果只在成员/处理器就绪后生效；热替换时
			// 媒体内部已保留协商结果，不需要重设。
			if (needStart) {
				ApplyCodecsLocked(observed);
			}

			// 步 7：开门（收包门由 gateway 把守）
			Response gate = gateway_->StartTransport(sessionId);
			if (!gate.IsSuccess()) {
				// 回滚到可观察的干净态：下一 tick 从“媒体未起”重新开始。
				gateway_->StopTransport(sessionId);
				Response undone = media_->StopMedia(confId, sessionId);
				if (!undone.IsSuccess()) {
					VCS_CONFERENCE_LOG_WARNING(
						"[Conference] rollback StopMedia failed: conf=%s, session=%s, "
						"msg=%s; next tick will reconcile", confId.c_str(), sessionId.c_str(),
						undone.GetMsg().c_str());
				}
				appliedTransportGeneration_ = 0;
				startedConfId_.clear();

				MediaFailureReport report;
				report.stage = "StartTransport";
				report.op = "StartTransport";
				report.conferenceId = confId;
				report.sessionId = sessionId;
				report.code = MapResponseCode(gate.GetCode());
				report.message = gate.GetMsg();
				report.elapsedMs = SteadyNowMs() - startMs;
				report.kinds = TransportKinds(transports);
				report.severity = vcs::EventSeverity::Warning;
				return ReportMediaFailureOnceLocked(report);
			}

			// 恢复正常：解除故障节流，让下一次新故障重新上报
			failureKey_.clear();
			failureAttempts_ = 0;

			VCS_CONFERENCE_LOG_INFO(
				"[Conference] media %s: conf=%s, session=%s, gen=%u, streams=%s, elapsedMs=%lld",
				needStart ? "started" : "hot-swapped",
				confId.c_str(), sessionId.c_str(),
				static_cast<unsigned>(appliedTransportGeneration_),
				JoinKinds(TransportKinds(transports)).c_str(),
				static_cast<long long>(SteadyNowMs() - startMs));
			return MediaErrorInfo();
		}

		void Conference::ConvergeStopLocked(bool force) {
			// 停止路径的判据同样来自媒体真相：先看这一会议号下是否真的有
			// 活媒体，没有就只清记账。空闲时这里只是一次短锁拷贝。
			const std::string confId = EffectiveConfIdLocked();
			std::string liveSession;
			if (media_) {
				vcs::MediaInfoSnapshot snapshot;
				if (media_->GetConferenceMediaInfo(confId, snapshot).IsSuccess() && snapshot.live) {
					liveSession = snapshot.sessionId;
				}
			}

			if (liveSession.empty()) {
				// 媒体已经不在。
				if (force && gateway_ && !activeSessionId_.empty()) {
					// 关机兜底：媒体快照可能已不可见，但接收门仍按会话号关一次。
					gateway_->StopTransport(activeSessionId_);
				}
				appliedTransportGeneration_ = 0;
				if (!startedConfId_.empty()) {
					lastMediaConfId_ = startedConfId_;
					startedConfId_.clear();
				}
				return;
			}

			// 会话还在 SM（典型 Disconnecting）：拆链归属该路径，此刻停媒体
			// 会与 MarkTerminated 的 StopReceiving 并发，等它删完再来。
			if (!force && gateway_) {
				vcs::gateway::SessionView view;
				if (gateway_->GetSessionView(liveSession, view) && view.hasSession) {
					return;
				}
			}

			if (gateway_) {
				Response gate = gateway_->StopTransport(liveSession);
				if (!gate.IsSuccess()) {
					VCS_CONFERENCE_LOG_WARNING(
						"[Conference] StopTransport failed: session=%s, msg=%s",
						liveSession.c_str(), gate.GetMsg().c_str());
				}
			}

			if (media_) {
				Response stopped = media_->StopMedia(confId, liveSession);
				if (!stopped.IsSuccess()) {
					VCS_CONFERENCE_LOG_WARNING(
						"[Conference] StopMedia failed: conf=%s, session=%s, msg=%s",
						confId.c_str(), liveSession.c_str(), stopped.GetMsg().c_str());
					return;   // 保留 startedConfId_，下一 tick 重试
				}
			}

			VCS_CONFERENCE_LOG_INFO("[Conference] media stopped: conf=%s, session=%s",
				confId.c_str(), liveSession.c_str());
			appliedTransportGeneration_ = 0;
			// startedConfId_ 清空后 GetMediaInfo 回落到 lastMediaConfId_，
			// 于是挂断后仍能读到最终快照（legacy 行为）。
			if (!startedConfId_.empty()) {
				lastMediaConfId_ = startedConfId_;
				startedConfId_.clear();
			}
		}

		void Conference::ApplyCodecsLocked(const ObservedCall& observed) {
			// 音频和视频各自独立：缺哪一类就只告警那一类，媒体保持该类默认。
			const std::string& sessionId = observed.view.sessionId;
			const bool hasAudio = !observed.view.audioCodec.codecName.empty();
			const bool hasVideo = !observed.view.videoCodec.codecName.empty();
			if (hasAudio || hasVideo) {
				media_->OnMemberCodecNegotiated(observed.confId, sessionId,
					observed.view.audioCodec, observed.view.videoCodec);
				VCS_CONFERENCE_LOG_INFO(
					"[Conference] codec applied: session=%s, audio=%s(pt=%u), "
					"video=%s(pt=%u)",
					sessionId.c_str(),
					hasAudio ? observed.view.audioCodec.codecName.c_str() : "(none)",
					static_cast<unsigned>(observed.view.audioCodec.payloadType),
					hasVideo ? observed.view.videoCodec.codecName.c_str() : "(none)",
					static_cast<unsigned>(observed.view.videoCodec.payloadType));
			}
			if (!hasAudio) {
				VCS_CONFERENCE_LOG_WARNING(
					"[Conference] audio codec negotiation missing: session=%s, "
					"media keeps default G711U/pt0", sessionId.c_str());
			}
			if (!hasVideo) {
				VCS_CONFERENCE_LOG_WARNING(
					"[Conference] video codec negotiation missing: session=%s, "
					"media keeps default H264/pt97", sessionId.c_str());
			}
		}

		// ============================================================
		// 失败上报（唯一出口，docs/08 §2.9 D）
		// ============================================================

		MediaErrorInfo Conference::ReportMediaFailure(const MediaFailureReport& report) {
			MediaErrorInfo info;
			info.stage = report.stage ? report.stage : "";
			info.op = report.op ? report.op : "";
			info.conferenceId = report.conferenceId;
			info.sessionId = report.sessionId;
			info.errorCode = static_cast<int>(report.code);
			info.message = report.message;
			info.elapsedMs = report.elapsedMs;

			const std::string kinds = JoinKinds(report.kinds);

			if (report.severity == vcs::EventSeverity::Warning) {
				VCS_CONFERENCE_LOG_WARNING(
					"[Conference] media failure: stage=%s, op=%s, conf=%s, session=%s, "
					"code=%d, message=%s, kinds=%s, elapsedMs=%lld",
					info.stage.c_str(), info.op.c_str(), info.conferenceId.c_str(),
					info.sessionId.c_str(), info.errorCode, info.message.c_str(),
					kinds.c_str(), static_cast<long long>(info.elapsedMs));
			}
			else {
				VCS_CONFERENCE_LOG_ERROR(
					"[Conference] media failure: stage=%s, op=%s, conf=%s, session=%s, "
					"code=%d, message=%s, kinds=%s, elapsedMs=%lld",
					info.stage.c_str(), info.op.c_str(), info.conferenceId.c_str(),
					info.sessionId.c_str(), info.errorCode, info.message.c_str(),
					kinds.c_str(), static_cast<long long>(info.elapsedMs));
			}

			if (eventBus_) {
				vcs::Event ev;
				ev.type = vcs::EventType::SystemError;
				ev.severity = report.severity;
				ev.source = vcs::EventSource::Conference;
				ev.sessionId = info.sessionId;
				ev.conferenceId = info.conferenceId;
				ev.errorCode = info.errorCode;

				vcs::base::Json payload;
				payload.Set("code", info.errorCode);
				payload.Set("message", info.message);
				payload.Set("origin", "conference");
				payload.Set("stage", info.stage);
				payload.Set("op", info.op);
				payload.Set("confId", info.conferenceId);
				payload.Set("sessionId", info.sessionId);
				if (!kinds.empty()) {
					payload.Set("kinds", kinds);
				}
				payload.Set("elapsedMs", static_cast<int64_t>(info.elapsedMs));
				ev.payload = payload.Dump();
				eventBus_->Publish(ev);
			}

			return info;
		}

		MediaErrorInfo Conference::ReportMediaFailureOnceLocked(
			const MediaFailureReport& report) {
			const std::string key = report.sessionId + "|" +
				(report.stage ? report.stage : "");
			if (key == failureKey_) {
				// 同一故障的重试：修不了就不刷屏，次数留证即可。
				++failureAttempts_;
				VCS_CONFERENCE_LOG_DEBUG(
					"[Conference] media failure retry #%u: stage=%s, session=%s, msg=%s",
					static_cast<unsigned>(failureAttempts_),
					report.stage ? report.stage : "",
					report.sessionId.c_str(), report.message.c_str());
				return MediaErrorInfo();
			}
			failureKey_ = key;
			failureAttempts_ = 1;
			return ReportMediaFailure(report);
		}

	} // namespace conference
} // namespace vcs
