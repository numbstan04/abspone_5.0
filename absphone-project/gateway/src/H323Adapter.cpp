#include "H323Adapter.h"
#include "H323MediaTransport.h"
#include "SessionManager.h"  // [V5.6] Needed for SessionManager injection
#include "ILogger.h"         // For VCS_GATEWAY_LOG_* macros
#include <sstream>
#include <iostream>

namespace vcs { namespace gateway {

H323Adapter::H323Adapter(const std::string& listenAddress, uint16_t port)
    : type_("H.323")
    , listenAddress_(listenAddress)
    , port_(port)
    , running_(false)
    , callback_(nullptr)
    , listenRunning_(false) {

    // Initialize the signaling handler
    signalingHandler_.reset(new SignalingHandler("H.323",
                                                 "h323://" + listenAddress + ":" + std::to_string(port)));

    // [V5.6] If SetSessionManager() was already called before signalingHandler_ was created,
    // forward the SessionManager to the newly created SignalingHandler now.
    if (sessionManager_) {
        signalingHandler_->SetSessionManager(sessionManager_);
    }
}

H323Adapter::~H323Adapter() {
    Stop();
}

vcs::Response H323Adapter::Start() {
    if (running_) {
        return vcs::Response::Error(vcs::ResponseCode::Conflict, "H.323 adapter already running");
    }

    // Start the signaling handler
    signalingHandler_->Start();

    // Start the listen thread
    StartListenThread();

    running_ = true;
    return vcs::Response::Success("H.323 adapter started");
}

vcs::Response H323Adapter::Stop() {
    if (!running_) {
        return vcs::Response::Success("H.323 adapter already stopped");
    }

    running_ = false;

    // Stop the listen thread
    StopListenThread();

    // Stop the signaling handler
    signalingHandler_->Stop();

    return vcs::Response::Success("H.323 adapter stopped");
}

bool H323Adapter::IsRunning() const {
    return running_;
}

void H323Adapter::SetEventBus(std::shared_ptr<vcs::IEventBus> bus) {
    eventBus_ = bus;
}

std::shared_ptr<vcs::IEventBus> H323Adapter::GetEventBus() const {
    return eventBus_;
}

void H323Adapter::SetCallback(IProtocolAdapterCallback* callback) {
    callback_ = callback;
}

IProtocolAdapterCallback* H323Adapter::GetCallback() const {
    return callback_;
}

// [V7.0 Phase-3] Inject the shared SessionManager (owned by GatewayManager).
// The H.323 adapter does not use SessionManager directly; the pointer is stored
// and forwarded to the SignalingHandler, which routes every callId/sessionId
// operation through that single SessionManager.
void H323Adapter::SetSessionManager(SessionManager* sm) {
    sessionManager_ = sm;
    if (signalingHandler_) {
        signalingHandler_->SetSessionManager(sm);
        VCS_GATEWAY_LOG_INFO("[H.323] SetSessionManager: SessionManager %p injected into SignalingHandler",
                             (void*)sm);
    }
}

ISignalingHandler* H323Adapter::GetSignalingHandler() {
    return signalingHandler_.get();
}

// ===================================================================
// Session Query
// ===================================================================

std::vector<std::string> H323Adapter::GetActiveSessionIds() const {
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

CallSessionInfo* H323Adapter::GetSessionInfo(const std::string& sessionId) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session) {
        return nullptr;
    }

    static CallSessionInfo info;
    info.sessionId = session->sessionId;
    info.caller = session->caller;
    info.callee = session->callee;
    info.conferenceId = session->conferenceId;
    info.protocol = "H.323";
    info.state = session->state;
    info.startTime = session->startTime;
    info.connectTime = session->connectTime;

    return &info;
}

size_t H323Adapter::GetActiveSessionCount() const {
    return signalingHandler_->GetActiveSessionCount();
}

// ===================================================================
// Call Control
// ===================================================================

vcs::Response H323Adapter::InitiateCall(const std::string& callee,
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

vcs::Response H323Adapter::AcceptCall(const std::string& sessionId, const Params& params) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId <= 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    return signalingHandler_->AcceptCall(session->callId, params);
}

vcs::Response H323Adapter::RejectCall(const std::string& sessionId, const std::string& reason) {
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

vcs::Response H323Adapter::TerminateCall(const std::string& sessionId, const std::string& reason) {
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

vcs::Response H323Adapter::TerminateAllCalls() {
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

void H323Adapter::HandleIncomingCall(const std::string& sessionId,
                                   const std::string& caller,
                                   const Params& params) {
    (void)sessionId;
    (void)caller;
    (void)params;
    // Inbound calls are handled by SignalingHandler callbacks
}

// ===================================================================
// Codec Configuration
// ===================================================================

vcs::Response H323Adapter::ConfigureVideoCodec(const vcs::media_config::VideoCodecConfig& config) {
    (void)config;
    // H.323 stub - actual configuration is handled by OpenH323
    return vcs::Response::Success("Video codec configured (H.323 stub)");
}

vcs::Response H323Adapter::ConfigureAudioCodec(const vcs::media_config::AudioCodecConfig& config) {
    (void)config;
    // H.323 stub - actual configuration is handled by OpenH323
    return vcs::Response::Success("Audio codec configured (H.323 stub)");
}

vcs::Response H323Adapter::ConfigureSrtp(const vcs::media_config::SrtpConfig& config) {
    (void)config;
    // H.323 stub - SRTP configuration is handled by OpenH323
    return vcs::Response::Success("SRTP configured (H.323 stub)");
}

// ===================================================================
// Local Media Configuration (for external SDP generation)
// ===================================================================

void H323Adapter::SetLocalMediaAddress(const std::string& address) {
    // H.323 stub - actual configuration is handled by OpenH323
    (void)address;
}

void H323Adapter::SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort) {
    // H.323 stub - actual configuration is handled by OpenH323
    (void)audioPort;
    (void)videoPort;
}

// ===================================================================
// H.323-specific Methods
// ===================================================================

vcs::Response H323Adapter::HandleSetup(const std::string& sessionId, const Params& params) {
    std::string caller;
    auto optCaller = params.GetString("caller");
    if (optCaller) {
        caller = *optCaller;
    }

    HandleIncomingCall(sessionId, caller, params);
    return vcs::Response::Success("SETUP processed");
}

vcs::Response H323Adapter::HandleConnect(const std::string& sessionId) {
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
    return vcs::Response::Success("CONNECT processed");
}

vcs::Response H323Adapter::HandleRelease(const std::string& sessionId) {
    return TerminateCall(sessionId, "RELEASE received");
}

vcs::Response H323Adapter::HandleFacility(const std::string& sessionId, const Params& params) {
    (void)sessionId;
    (void)params;
    return vcs::Response::Success("FACILITY processed");
}

vcs::Response H323Adapter::HandleProgress(const std::string& sessionId) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (session && session->callId > 0) {
        signalingHandler_->UpdateSessionState(session->callId, SessionState::Connecting);
    }
    return vcs::Response::Success("PROGRESS processed");
}

// ===================================================================
// Private Methods
// ===================================================================

void H323Adapter::notifySessionCountChanged() {
    if (callback_) {
        size_t count = GetActiveSessionCount();
        callback_->OnAdapterSessionCountChanged(type_, count);
    }
}

// ===================================================================
// Listen Thread Implementation
// ===================================================================

void H323Adapter::StartListenThread() {
    if (listenRunning_.load()) {
        return;
    }

    listenRunning_ = true;
    listenThread_ = std::thread([this]() { ListenLoop(); });
}

void H323Adapter::StopListenThread() {
    if (!listenRunning_.load()) {
        return;
    }

    listenRunning_ = false;

    if (listenThread_.joinable()) {
        listenThread_.join();
    }
}

void H323Adapter::ListenLoop() {
    // TODO: When integrating OpenH323, replace this with OpenH323's event loop
    while (listenRunning_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ===================================================================
// IRtpTransportCallback Implementation (V5.4 unified interface)
// ===================================================================

void H323Adapter::OnRtpReceived(const RtpPacket& packet,
                                const SessionContext& context) {
    // H.323 stub - unified RTP packet reception, Media layer parses RTP header to determine type
    (void)packet;
    (void)context;
}

void H323Adapter::OnRtcpReceived(const RtcpPacket& packet,
                                 const SessionContext& context) {
    // H.323 stub - handle RTCP feedback
    (void)packet;
    (void)context;
}

void H323Adapter::OnTransportConnected(const SessionContext& context) {
    // H323 transport connected
    (void)context;
}

void H323Adapter::OnTransportDisconnected(const SessionContext& context,
                                          const std::string& reason) {
    // H323 transport disconnected
    (void)context;
    (void)reason;
}

void H323Adapter::OnTransportError(const SessionContext& context,
                                   int errorCode,
                                   const std::string& errorMessage) {
    // Handle media transport error
    (void)context;
    (void)errorCode;
    (void)errorMessage;
    if (callback_) {
        callback_->OnAdapterError(type_, errorCode, errorMessage);
    }
}

// ===================================================================
// Media Transport Creation Methods
// ===================================================================

void H323Adapter::CreateMediaTransport(int callId, const std::string& remoteAddress, uint16_t remotePort) {
    // Resolve through SessionManager (shared_ptr): a raw CallSession* could
    // dangle if the call is torn down between lookup and use.
    auto session = sessionManager_ ? sessionManager_->GetSessionByCallId(callId) : nullptr;
    if (!session) {
        return;
    }

    auto transport = std::make_shared<H323MediaTransport>(
        session->sessionId,
        remoteAddress,
        remotePort
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
