#include "WebrtcAdapter.h"
#include "WebrtcMediaTransport.h"
#include "SessionManager.h"  // [V5.6] Needed for SessionManager injection
#include <sstream>
#include <iostream>

namespace vcs { namespace gateway {

WebrtcAdapter::WebrtcAdapter(const std::string& listenAddress, uint16_t port)
    : type_("WebRTC")
    , listenAddress_(listenAddress)
    , port_(port)
    , running_(false)
    , callback_(nullptr)
    , listenRunning_(false) {

    // Initialize the signaling handler
    signalingHandler_.reset(new SignalingHandler("WebRTC",
                                                 "ws://" + listenAddress + ":" + std::to_string(port)));

    // [V5.6] If SetSessionManager() was already called before signalingHandler_ was created,
    // forward the SessionManager to the newly created SignalingHandler now.
    if (sessionManager_) {
        signalingHandler_->SetSessionManager(sessionManager_);
    }
}

WebrtcAdapter::~WebrtcAdapter() {
    Stop();
}

vcs::Response WebrtcAdapter::Start() {
    if (running_) {
        return vcs::Response::Error(vcs::ResponseCode::Conflict, "WebRTC adapter already running");
    }

    // Start the signaling handler
    signalingHandler_->Start();

    // Start the listen thread
    StartListenThread();

    running_ = true;
    return vcs::Response::Success("WebRTC adapter started");
}

vcs::Response WebrtcAdapter::Stop() {
    if (!running_) {
        return vcs::Response::Success("WebRTC adapter already stopped");
    }

    running_ = false;

    // Stop the listen thread
    StopListenThread();

    // Stop the signaling handler
    signalingHandler_->Stop();

    return vcs::Response::Success("WebRTC adapter stopped");
}

bool WebrtcAdapter::IsRunning() const {
    return running_;
}

void WebrtcAdapter::SetEventBus(std::shared_ptr<vcs::IEventBus> bus) {
    eventBus_ = bus;
}

std::shared_ptr<vcs::IEventBus> WebrtcAdapter::GetEventBus() const {
    return eventBus_;
}

void WebrtcAdapter::SetCallback(IProtocolAdapterCallback* callback) {
    callback_ = callback;
}

IProtocolAdapterCallback* WebrtcAdapter::GetCallback() const {
    return callback_;
}

// [V7.0 Phase-3] Inject the shared SessionManager (owned by GatewayManager).
// The WebRTC adapter does not use SessionManager directly; the pointer is stored
// and forwarded to the SignalingHandler, which routes every callId/sessionId
// operation through that single SessionManager.
void WebrtcAdapter::SetSessionManager(SessionManager* sm) {
    sessionManager_ = sm;
    if (signalingHandler_) {
        signalingHandler_->SetSessionManager(sm);
    }
}

ISignalingHandler* WebrtcAdapter::GetSignalingHandler() {
    return signalingHandler_.get();
}

// ===================================================================
// Session Query
// ===================================================================

std::vector<std::string> WebrtcAdapter::GetActiveSessionIds() const {
    std::vector<std::string> result;
    if (!sessionManager_) {
        return result;
    }
    for (int callId : signalingHandler_->GetActiveCallIds()) {
        auto session = sessionManager_->GetSessionByCallId(callId);
        if (session) {
            result.push_back(session->sessionId);
        }
    }
    return result;
}

CallSessionInfo* WebrtcAdapter::GetSessionInfo(const std::string& sessionId) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session) {
        return nullptr;
    }

    static CallSessionInfo info;
    info.sessionId = session->sessionId;
    info.caller = session->caller;
    info.callee = session->callee;
    info.conferenceId = session->conferenceId;
    info.protocol = "WebRTC";
    info.state = session->state;
    info.startTime = session->startTime;
    info.connectTime = session->connectTime;

    return &info;
}

size_t WebrtcAdapter::GetActiveSessionCount() const {
    return signalingHandler_->GetActiveSessionCount();
}

// ===================================================================
// Call Control
// ===================================================================

vcs::Response WebrtcAdapter::InitiateCall(const std::string& callee,
                                          const Params& params,
                                          std::string& sessionId) {
    if (!running_) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError, "Adapter not running");
    }

    // Initiate the call through SignalingHandler
    std::string newSessionId;
    vcs::Response resp = signalingHandler_->InitiateCall(callee, params, newSessionId);
    if (!resp.IsSuccess()) {
        return resp;
    }

    sessionId = newSessionId;
    notifySessionCountChanged();

    return vcs::Response::Success("Call initiated");
}

vcs::Response WebrtcAdapter::AcceptCall(const std::string& sessionId, const Params& params) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId <= 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    return signalingHandler_->AcceptCall(session->callId, params);
}

vcs::Response WebrtcAdapter::RejectCall(const std::string& sessionId, const std::string& reason) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId <= 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    vcs::Response resp = signalingHandler_->RejectCall(session->callId, reason);
    if (resp.IsSuccess()) {
        notifySessionCountChanged();
    }

    return resp;
}

vcs::Response WebrtcAdapter::TerminateCall(const std::string& sessionId, const std::string& reason) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId <= 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    vcs::Response resp = signalingHandler_->TerminateCall(session->callId, reason);
    if (resp.IsSuccess()) {
        notifySessionCountChanged();
    }

    return resp;
}

vcs::Response WebrtcAdapter::TerminateAllCalls() {
    auto activeCallIds = signalingHandler_->GetActiveCallIds();

    for (int callId : activeCallIds) {
        signalingHandler_->TerminateCall(callId, "Terminated by adapter");
    }

    notifySessionCountChanged();
    return vcs::Response::Success("All calls terminated");
}

// ===================================================================
// Inbound Call Handling
// ===================================================================

void WebrtcAdapter::HandleIncomingCall(const std::string& sessionId,
                                      const std::string& caller,
                                      const Params& params) {
    (void)sessionId;
    (void)caller;
    (void)params;
    // Inbound calls are handled by SignalingHandler callbacks
}

// ===================================================================
// WebRTC-specific Methods
// ===================================================================

vcs::Response WebrtcAdapter::HandleOffer(const std::string& sessionId, const Params& params) {
    std::string caller;
    auto optCaller = params.GetString("caller");
    if (optCaller) {
        caller = *optCaller;
    }

    HandleIncomingCall(sessionId, caller, params);
    return vcs::Response::Success("Offer processed");
}

vcs::Response WebrtcAdapter::HandleAnswer(const std::string& sessionId, const Params& params) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId <= 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }
    const int callId = session->callId;

    // [V7.0 Phase-2 F] Read the primary transport's remote address/port
    // rather than the deleted bare session fields. The session is already
    // held by shared_ptr above, so no second (raw) lookup is needed.
    if (!session->GetPrimaryRemoteAddress().empty() && session->GetPrimaryRemotePort() > 0) {
        CreateMediaTransport(callId, session->GetPrimaryRemoteAddress(), session->GetPrimaryRemotePort());
    }

    signalingHandler_->UpdateSessionState(callId, SessionState::Active);
    (void)params;
    return vcs::Response::Success("Answer processed");
}

vcs::Response WebrtcAdapter::HandleIceCandidate(const std::string& sessionId, const Params& params) {
    // Update state on ICE connection success
    auto optConnected = params.GetBool("iceConnected");
    if (optConnected && *optConnected) {
        auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
        if (session && session->callId > 0) {
            signalingHandler_->UpdateSessionState(session->callId, SessionState::Active);
        }
    }

    (void)params;
    return vcs::Response::Success("ICE candidate processed");
}

// ===================================================================
// Private Methods
// ===================================================================

void WebrtcAdapter::notifySessionCountChanged() {
    if (callback_) {
        size_t count = GetActiveSessionCount();
        callback_->OnAdapterSessionCountChanged(type_, count);
    }
}

// ===================================================================
// Listen Thread Implementation
// ===================================================================

void WebrtcAdapter::StartListenThread() {
    if (listenRunning_.load()) {
        return;
    }

    listenRunning_ = true;
    listenThread_ = std::thread([this]() { ListenLoop(); });
}

void WebrtcAdapter::StopListenThread() {
    if (!listenRunning_.load()) {
        return;
    }

    listenRunning_ = false;

    if (listenThread_.joinable()) {
        listenThread_.join();
    }
}

void WebrtcAdapter::ListenLoop() {
    // TODO: When integrating libwebsockets, replace this with the WebSocket server event loop
    while (listenRunning_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ===================================================================
// IRtpTransportCallback Implementation (V5.4 unified interface)
// ===================================================================

void WebrtcAdapter::OnRtpReceived(const RtpPacket& packet,
                                 const SessionContext& context) {
    // WebRTC stub - unified RTP packet reception, Media layer parses RTP header to determine type
    (void)packet;
    (void)context;
}

void WebrtcAdapter::OnRtcpReceived(const RtcpPacket& packet,
                                  const SessionContext& context) {
    // WebRTC stub - handle RTCP feedback
    (void)packet;
    (void)context;
}

void WebrtcAdapter::OnTransportConnected(const SessionContext& context) {
    (void)context;
    // VCS_GATEWAY_LOG_INFO("WebRTC transport connected: sessionId=%s", context.sessionId.c_str());
}

void WebrtcAdapter::OnTransportDisconnected(const SessionContext& context,
                                          const std::string& reason) {
    (void)context;
    (void)reason;
    // VCS_GATEWAY_LOG_INFO("WebRTC transport disconnected: sessionId=%s, reason=%s",
    //                     context.sessionId.c_str(), reason.c_str());
}

void WebrtcAdapter::OnTransportError(const SessionContext& context,
                                   int errorCode,
                                   const std::string& errorMessage) {
    (void)context;
    (void)errorCode;
    (void)errorMessage;
    // VCS_GATEWAY_LOG_ERROR("WebRTC transport error: sessionId=%s, code=%d, msg=%s",
    //                      context.sessionId.c_str(), errorCode, errorMessage.c_str());
    if (callback_) {
        callback_->OnAdapterError(type_, errorCode, errorMessage);
    }
}

// ===================================================================
// Media Transport Creation Methods
// ===================================================================

void WebrtcAdapter::CreateMediaTransport(int callId, const std::string& remoteAddress, uint16_t remotePort) {
    // Resolve through SessionManager (shared_ptr): a raw CallSession* could
    // dangle if the call is torn down between lookup and use.
    auto session = sessionManager_ ? sessionManager_->GetSessionByCallId(callId) : nullptr;
    if (!session) {
        return;
    }

    // WebRTC uses DTLS/SRTP, no separate port parameter required
    auto transport = std::make_shared<WebrtcMediaTransport>(
        session->sessionId,
        remoteAddress
    );

    transport->SetCallback(this);
    // [Single write entry] SessionManager owns the transport table; register
    // the Audio slot together with its endpoint in one call. If a live
    // transport is already attached (re-CONNECT), AttachTransport reuses the
    // entry or replaces+stops the old object outside its lock.
    if (sessionManager_) {
        sessionManager_->AttachTransport(session->sessionId, MediaStreamKind::Audio,
                                         transport, remoteAddress, remotePort);
    }
}

} // namespace gateway
} // namespace vcs
