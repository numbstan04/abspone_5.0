#include "SignalingHandler.h"
#include "SessionManager.h"
#include "ILogger.h"
#include "TimeUtils.h"
#include <sstream>
#include <chrono>
#include <utility>

namespace vcs { namespace gateway {

// ============================================================
// Construction / Destruction
// ============================================================

SignalingHandler::SignalingHandler(const std::string& protocol,
                                   const std::string& localUri,
                                   SessionManager* sessionManager)
    : protocol_(protocol)
    , localUri_(localUri)
    , running_(false)
    , callback_(nullptr)
    , sessionManager_(sessionManager) {
    VCS_GATEWAY_LOG_INFO("[SignalingHandler] Created: protocol=%s, localUri=%s, sessionManager=%s",
                         protocol_.c_str(), localUri_.c_str(),
                         sessionManager_ ? "injected" : "not-set");
}

SignalingHandler::~SignalingHandler() {
    Stop();
}

void SignalingHandler::SetSessionManager(SessionManager* sessionManager) {
    sessionManager_ = sessionManager;
    VCS_GATEWAY_LOG_INFO("[SignalingHandler] SessionManager set: %s",
                         sessionManager_ ? "valid" : "null");
}

ProtocolType SignalingHandler::GetProtocolType() const {
    if (protocol_ == "SIP")    return ProtocolType::SIP;
    if (protocol_ == "H.323")  return ProtocolType::H323;
    if (protocol_ == "WebRTC" || protocol_ == "WEBRTC") return ProtocolType::WEBRTC;
    return ProtocolType::UNKNOWN;
}

// ============================================================
// ISignalingHandler Implementation
// ============================================================

bool SignalingHandler::Start() {
    if (running_) {
        return false;
    }

    if (!sessionManager_) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] Start failed: SessionManager not injected");
        return false;
    }

    bool result = DoStart();
    if (result) {
        running_ = true;
        // Auto-start the background cleanup thread (cadence only; timeout
        // judgment lives in SessionManager). Failure is logged but does not
        // fail Start() — only stale-session reaping would be lost.
        StartCleanupThread();
    }
    return result;
}

void SignalingHandler::Stop() {
    if (!running_) {
        return;
    }

    // Stop the cleanup thread FIRST so it cannot race the teardown done by
    // GatewayManager::Stop (which calls SM->CleanupAllSessions).
    StopCleanupThread();

    DoStop();
    running_ = false;
}

CallSession* SignalingHandler::GetSession(int callId) {
    if (!sessionManager_) return nullptr;
    return sessionManager_->GetSessionByCallIdRaw(callId);
}

CallSession* SignalingHandler::GetSessionById(const std::string& sessionId) {
    if (!sessionManager_) return nullptr;
    auto sp = sessionManager_->GetSession(sessionId);
    return sp.get();
}

std::vector<int> SignalingHandler::GetActiveCallIds() const {
    if (!sessionManager_) return {};
    return sessionManager_->GetActiveCallIds();
}

size_t SignalingHandler::GetActiveSessionCount() const {
    if (!sessionManager_) return 0;
    return sessionManager_->GetActiveSessionCount();
}

vcs::Response SignalingHandler::InitiateCall(const std::string& callee,
                                             const Params& params,
                                             std::string& sessionId) {
    if (!running_) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError, "Handler not running");
    }
    if (!sessionManager_) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "SessionManager not set");
    }

    // Subclass implements the actual call. NOTE: the protocol stack may fire
    // state callbacks during DoInitiateCall — that is why the session is
    // created (and the callId bound) only afterwards.
    int callId = DoInitiateCall(callee, params);
    if (callId < 0) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] InitiateCall: stack rejected '%s'", callee.c_str());
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "Failed to initiate call: " + callee);
    }

    CallSession* session = CreateSession(callId, "", "", SessionState::OutgoingPending);
    if (!session) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError, "Failed to create session");
    }

    // 发起期状态回调：会话已建立（OutgoingPending）且 callId 已绑定，
    // 提交 callee 并发布 CallOutgoing。SM 是唯一发布点（docs/07 §2.2），
    // 只订阅事件的观察者由此才能看到"呼出中"，而不是直接跳到接通/失败。
    if (!sessionManager_->MarkOutgoing(session->sessionId, {callee})) {
        VCS_GATEWAY_LOG_WARNING("[SignalingHandler] InitiateCall: MarkOutgoing rejected "
                                "(callId=%d, sessionId=%s)",
                                callId, session->sessionId.c_str());
    }
    sessionId = session->sessionId;

    return vcs::Response::Success("Call initiated");
}

vcs::Response SignalingHandler::AcceptCall(int callId, const Params& params) {
    if (!sessionManager_) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "SessionManager not set");
    }

    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    if (session->state != SessionState::IncomingPending &&
        session->state != SessionState::OutgoingPending) {
        std::ostringstream oss;
        oss << "Cannot accept in state: " << static_cast<int>(session->state);
        return vcs::Response::Error(vcs::ResponseCode::InvalidOperation, oss.str());
    }

    if (!DoAcceptCall(callId, params)) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError, "Failed to accept call");
    }

    sessionManager_->UpdateSessionState(session->sessionId, SessionState::Connecting);
    NotifyCallStateChanged(callId, SessionState::Connecting);

    return vcs::Response::Success("Call accepted");
}

vcs::Response SignalingHandler::RejectCall(int callId, const std::string& reason) {
    if (!sessionManager_) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "SessionManager not set");
    }

    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    if (session->state != SessionState::IncomingPending) {
        return vcs::Response::Error(vcs::ResponseCode::InvalidOperation,
                                    "Session not in IncomingPending state");
    }

    if (!DoRejectCall(callId, reason)) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] RejectCall: stack failed to reject callId=%d",
                              callId);
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "Failed to reject call " + std::to_string(callId));
    }

    // Teardown + CallTerminated publish happen inside MarkTerminated (single
    // publish point). The session must still exist at this point — removing
    // it beforehand would swallow the event.
    sessionManager_->MarkTerminated(session->sessionId, {reason, 0});

    NotifyCallTerminated(callId, reason);
    return vcs::Response::Success("Call rejected");
}

vcs::Response SignalingHandler::TerminateCall(int callId, const std::string& reason) {
    if (!sessionManager_) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "SessionManager not set");
    }

    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    if (session->state == SessionState::Disconnected ||
        session->state == SessionState::Disconnecting) {
        return vcs::Response::Error(vcs::ResponseCode::InvalidOperation, "Session already disconnected");
    }

    sessionManager_->UpdateSessionState(session->sessionId, SessionState::Disconnecting);

    // 本地会话必须无条件收尾(否则上层永远挂着),但 BYE 失败不能报成功。
    const bool byeSent = DoTerminateCall(callId, reason);

    sessionManager_->MarkTerminated(session->sessionId, {reason, 0});

    NotifyCallTerminated(callId, reason);

    if (!byeSent) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] TerminateCall: BYE failed, "
                              "local session torn down: callId=%d", callId);
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "Call terminated locally, BYE not sent");
    }
    return vcs::Response::Success("Call terminated");
}

// ============================================================
// Callback Settings
// ============================================================

void SignalingHandler::SetCallback(ISignalingHandlerCallback* callback) {
    callback_ = callback;
}

void SignalingHandler::SetCallIncomingCallback(
    std::function<void(int, const std::string&, const Params&)> callback) {
    callIncomingCallback_ = callback;
}

void SignalingHandler::SetCallStateChangedCallback(
    std::function<void(int, SessionState)> callback) {
    callStateChangedCallback_ = callback;
}

void SignalingHandler::SetCallConnectedCallback(
    std::function<void(int)> callback) {
    callConnectedCallback_ = callback;
}

void SignalingHandler::SetCallTerminatedCallback(
    std::function<void(int, const std::string&)> callback) {
    callTerminatedCallback_ = callback;
}

void SignalingHandler::SetSignalingErrorCallback(
    std::function<void(int, int, const std::string&)> callback) {
    signalingErrorCallback_ = callback;
}

void SignalingHandler::SetMediaReadyCallback(
    std::function<void(int, const std::string&, std::shared_ptr<IRtpTransport>)> callback) {
    mediaReadyCallback_ = callback;
}

void SignalingHandler::SetMediaReadyCallbackEx(
    std::function<void(const MediaReadyInfo&)> callback) {
    mediaReadyCallbackEx_ = callback;
}

// ============================================================
// Internal Methods
// ============================================================

CallSession* SignalingHandler::CreateSession(int callId,
                                          const std::string& caller,
                                          const std::string& confId,
                                          SessionState initialState) {
    if (!sessionManager_) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] CreateSession: no SessionManager injected");
        return nullptr;
    }

    SessionConfig config;
    config.conferenceId = confId.empty() ? SessionManager::kDefaultConferenceId : confId;
    config.callerUri = caller;
    config.protocol = GetProtocolType();

    auto session = sessionManager_->CreateSession(config, initialState);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] CreateSession: SM->CreateSession failed "
                              "(callId=%d, confId=%s)",
                              callId, config.conferenceId.c_str());
        return nullptr;
    }

    // Bind the protocol-stack call ID and register the callId index.
    session->callId = callId;
    if (!sessionManager_->AddSession(callId, session)) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] CreateSession: AddSession failed "
                              "(callId=%d, sessionId=%s) — rolling back",
                              callId, session->sessionId.c_str());
        sessionManager_->MarkTerminated(session->sessionId, {"callId index bind failed", 0});
        return nullptr;
    }

    VCS_GATEWAY_LOG_INFO("[SignalingHandler] CreateSession: callId=%d, sessionId=%s, confId=%s, state=%d",
                         callId, session->sessionId.c_str(),
                         config.conferenceId.c_str(), static_cast<int>(initialState));

    return session.get();
}

void SignalingHandler::UpdateSessionState(int callId, SessionState state) {
    if (!sessionManager_) return;

    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) {
        return;
    }

    if (!sessionManager_->UpdateSessionState(session->sessionId, state)) {
        return;
    }

    NotifyCallStateChanged(callId, state);

    // Legacy hook: adapter starts media on this notification (signaling
    // confirmed). CallConnected event publication itself goes through
    // SM->MarkConnected — invoked by the adapter from this callback.
    if (state == SessionState::Active) {
        NotifyCallConnected(callId);
    }
}

void SignalingHandler::SetSessionMediaTransport(int callId, std::shared_ptr<IRtpTransport> transport) {
    if (!sessionManager_) return;
    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) return;
    // Single write entry: SessionManager owns the transport table. This
    // interface method means "this is the primary (Audio) transport".
    sessionManager_->AttachTransport(session->sessionId, MediaStreamKind::Audio,
                                     std::move(transport));
}

std::shared_ptr<IRtpTransport> SignalingHandler::GetSessionMediaTransport(int callId) {
    if (!sessionManager_) return nullptr;
    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) return nullptr;
    return session->GetPrimaryTransportObject();
}

void SignalingHandler::NotifyCallIncoming(int callId, const std::string& caller, const Params& params) {
    if (callback_) {
        callback_->OnCallIncoming(callId, caller, params);
    }
    if (callIncomingCallback_) {
        callIncomingCallback_(callId, caller, params);
    }
}

void SignalingHandler::NotifyCallStateChanged(int callId, SessionState state) {
    if (callback_) {
        callback_->OnCallStateChanged(callId, state);
    }
    if (callStateChangedCallback_) {
        callStateChangedCallback_(callId, state);
    }
}

void SignalingHandler::NotifyCallConnected(int callId) {
    if (callback_) {
        callback_->OnCallConnected(callId);
    }
    if (callConnectedCallback_) {
        callConnectedCallback_(callId);
    }
}

void SignalingHandler::NotifyCallTerminated(int callId, const std::string& reason) {
    if (callback_) {
        callback_->OnCallTerminated(callId, reason);
    }
    if (callTerminatedCallback_) {
        callTerminatedCallback_(callId, reason);
    }
}

void SignalingHandler::NotifySignalingError(int callId, int errorCode, const std::string& errorMsg) {
    if (callback_) {
        callback_->OnSignalingError(callId, errorCode, errorMsg);
    }
    if (signalingErrorCallback_) {
        signalingErrorCallback_(callId, errorCode, errorMsg);
    }
}

void SignalingHandler::NotifyMediaReady(int callId, const std::string& remoteAddress, int remotePort) {
    if (!sessionManager_) return;
    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) return;

    // Endpoint-only update: no transport object yet (or keep the current one),
    // just record where the remote expects media. SessionManager applies it
    // under the shard lock; an empty transport never clears a live object.
    sessionManager_->AttachTransport(session->sessionId, MediaStreamKind::Audio,
                                     nullptr, remoteAddress,
                                     static_cast<uint16_t>(remotePort));

    VCS_GATEWAY_LOG_INFO("[SignalingHandler] Media ready for session %s - remote: %s:%d",
                         session->sessionId.c_str(), remoteAddress.c_str(), remotePort);

    if (mediaReadyCallback_) {
        mediaReadyCallback_(callId, remoteAddress, session->GetPrimaryTransportObject());
    }
}

void SignalingHandler::NotifyMediaReadyEx(const MediaReadyInfo& info) {
    if (sessionManager_) {
        auto session = sessionManager_->GetSessionByCallId(info.callId);
        if (session) {
            // SessionManager decides reuse vs. replace and stops a replaced
            // object outside the lock. The old inline StopReceiving here ran
            // on the caller's thread without synchronization.
            sessionManager_->AttachTransport(session->sessionId, MediaStreamKind::Audio,
                                             info.transport, info.remoteAddress,
                                             static_cast<uint16_t>(info.remotePort));
        }
    }

    if (mediaReadyCallbackEx_) {
        mediaReadyCallbackEx_(info);
    }
}

bool SignalingHandler::TerminateSession(int callId, const std::string& reason, int cause) {
    if (!sessionManager_) return false;

    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session) {
        return false;
    }

    const bool terminated =
        sessionManager_->MarkTerminated(session->sessionId, {reason, cause});
    if (terminated) {
        NotifyCallTerminated(callId, reason);
    }
    return terminated;
}

// ============================================================
// Background Cleanup Thread
// ============================================================

void SignalingHandler::SetCleanupInterval(uint32_t intervalSeconds) {
    if (intervalSeconds < 1) {
        VCS_GATEWAY_LOG_WARNING("[SignalingHandler] SetCleanupInterval: %u is too small, "
                                "clamping to 1 second",
                                intervalSeconds);
        intervalSeconds = 1;
    }
    cleanupIntervalSeconds_ = intervalSeconds;
    VCS_GATEWAY_LOG_INFO("[SignalingHandler] Cleanup interval set to %u seconds",
                         cleanupIntervalSeconds_);
}

bool SignalingHandler::StartCleanupThread() {
    // Idempotent: if already running, no-op.
    bool expected = false;
    if (!cleanupThreadRunning_.compare_exchange_strong(expected, true)) {
        return true;
    }

    if (!sessionManager_) {
        VCS_GATEWAY_LOG_WARNING("[SignalingHandler] Cleanup thread NOT started: "
                                "SessionManager not injected");
        cleanupThreadRunning_.store(false);
        return false;
    }

    try {
        cleanupThread_ = std::thread(&SignalingHandler::CleanupThreadFunc, this);
    } catch (const std::exception& e) {
        VCS_GATEWAY_LOG_ERROR("[SignalingHandler] Failed to start cleanup thread: %s",
                              e.what());
        cleanupThreadRunning_.store(false);
        return false;
    }

    VCS_GATEWAY_LOG_INFO("[SignalingHandler] Cleanup thread started: interval=%us",
                         cleanupIntervalSeconds_);
    return true;
}

void SignalingHandler::StopCleanupThread() {
    // Idempotent: if not running, nothing to do.
    if (!cleanupThreadRunning_.load()) {
        // Defensive: if a thread object exists but is not joinable,
        // detach it to avoid std::terminate on destruction.
        if (cleanupThread_.joinable()) {
            cleanupThread_.detach();
        }
        return;
    }

    cleanupThreadRunning_.store(false);

    if (cleanupThread_.joinable()) {
        try {
            cleanupThread_.join();
        } catch (const std::exception& e) {
            VCS_GATEWAY_LOG_ERROR("[SignalingHandler] Error joining cleanup thread: %s",
                                  e.what());
        }
    }

    VCS_GATEWAY_LOG_INFO("[SignalingHandler] Cleanup thread stopped");
}

void SignalingHandler::CleanupThreadFunc() {
    VCS_GATEWAY_LOG_INFO("[SignalingHandler] Cleanup worker: started (interval=%us)",
                         cleanupIntervalSeconds_);

    // Small sleep slice so StopCleanupThread() is responsive even with a
    // large configured interval.
    constexpr uint32_t kSliceMs = 200;

    while (cleanupThreadRunning_.load()) {
        const uint32_t totalMs = cleanupIntervalSeconds_ * 1000u;
        for (uint32_t slept = 0;
             slept < totalMs && cleanupThreadRunning_.load();
             slept += kSliceMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
        }
        if (!cleanupThreadRunning_.load()) {
            break;
        }

        if (!sessionManager_) {
            continue;
        }

        try {
            // Timeout policy (per-state table) lives inside the SM; each
            // reaped session exits through MarkTerminated and publishes
            // CallTerminated like any other teardown.
            size_t removed = sessionManager_->CleanupTimeoutSessions();
            if (removed > 0) {
                VCS_GATEWAY_LOG_INFO("[SignalingHandler] Cleanup worker: reaped %zu timed-out session(s)",
                                     removed);
            }
        } catch (const std::exception& e) {
            VCS_GATEWAY_LOG_ERROR("[SignalingHandler] Cleanup worker: exception during cleanup: %s",
                                  e.what());
        } catch (...) {
            VCS_GATEWAY_LOG_ERROR("[SignalingHandler] Cleanup worker: unknown exception during cleanup");
        }
    }

    VCS_GATEWAY_LOG_INFO("[SignalingHandler] Cleanup worker: exiting");
}

} // namespace gateway
} // namespace vcs
