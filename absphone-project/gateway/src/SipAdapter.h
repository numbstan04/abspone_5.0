#pragma once

#include "IProtocolAdapter.h"
#include "ISignalingHandler.h"
#include "SignalingHandler.h"
#include "SipMediaTransport.h"
#include "SipSignalingHandler.h"  // Contains RemoteMediaInfo definition
#include "SimpleRtpTransport.h"
#include "IRtpTransport.h"
#include "Response.h"
#include "MediaConfig.h"
#include "SessionContext.h"
#include "CallSession.h"
#include <string>
#include <memory>

// Include conference control - needed for calling MemberJoin
// Note: Uses relative path since conference/include is in include path



// PJSIP always enabled
#include <pjsua2.hpp>

namespace vcs { namespace gateway {

// Forward declaration
class SipSignalingHandler;
class SessionManager;
class IGatewayManager;

/**
 * @brief SIP protocol adapter
 *
 * SIP protocol adapter, responsible for SIP protocol access and management.
 *
 * Design notes:
 * 1. Aggregate SipSignalingHandler for SIP signaling processing
 * 2. Manage session lifecycle (via SignalingHandler's CallSession)
 * 3. Reports state changes as platform events on IEventBus; knows no Conference types
 */
class SipAdapter : public IProtocolAdapter,
                   public ISignalingHandlerCallback {
public:
    /**
     * @brief Constructor
     * @param listenAddress Listen address
     * @param port Listen port
     */
    SipAdapter(const std::string& listenAddress, uint16_t port);
    ~SipAdapter() override;

    // ============================================================
    // IProtocolAdapter Implementation
    // ============================================================

    const std::string& GetType() const override { return type_; }
    std::string GetListenAddress() const override { return listenAddress_; }
    uint16_t GetPort() const override { return port_; }

    vcs::Response Start() override;
    vcs::Response Stop() override;
    bool IsRunning() const override;






    void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) override;
    std::shared_ptr<vcs::IEventBus> GetEventBus() const override;

    void SetCallback(IProtocolAdapterCallback* callback) override;
    IProtocolAdapterCallback* GetCallback() const override;

    ISignalingHandler* GetSignalingHandler() override;

    // [V5.6] SessionManager injection (non-owning, set by GatewayManager)
    void SetSessionManager(SessionManager* sm) override;
    SessionManager* GetSessionManager() const override { return sessionManager_; }

    std::vector<std::string> GetActiveSessionIds() const override;
    CallSessionInfo* GetSessionInfo(const std::string& sessionId) override;
    size_t GetActiveSessionCount() const override;

    vcs::Response InitiateCall(const std::string& callee,
                                const Params& params,
                                std::string& sessionId) override;
    vcs::Response AcceptCall(const std::string& sessionId, const Params& params) override;
    vcs::Response RejectCall(const std::string& sessionId, const std::string& reason) override;
    vcs::Response TerminateCall(const std::string& sessionId, const std::string& reason) override;
    vcs::Response TerminateAllCalls() override;
    vcs::Response SendDTMF(const std::string& sessionId, const std::string& digits) override;


    void HandleIncomingCall(const std::string& sessionId,
                            const std::string& caller,
                            const Params& params) override;

    // ============================================================
    // Codec configuration (called by Conference module)
    // ============================================================

    vcs::Response ConfigureVideoCodec(const vcs::media_config::VideoCodecConfig& config) override;
    vcs::Response ConfigureAudioCodec(const vcs::media_config::AudioCodecConfig& config) override;

    // ============================================================
    // SRTP configuration (configured by application layer)
    // ============================================================

    vcs::Response ConfigureSrtp(const vcs::media_config::SrtpConfig& config) override;

    // ============================================================
    // SIP-specific methods
    // ============================================================

    /**
     * @brief Set SIP server (for registration)
     */
    void SetServer(const std::string& server,
                   const std::string& username,
                   const std::string& password);

    /**
     * @brief Handle SIP INVITE
     *
     * [V5.3 Modified]: Call MemberJoin validation before sending 200 OK
     */
    vcs::Response HandleInvite(const std::string& sessionId, const Params& params);

    /**
     * @brief Handle SIP BYE
     */
    vcs::Response HandleBye(const std::string& sessionId);

    /**
     * @brief Handle external call BYE request
     *
     * Used for session cleanup in module mode (not via SignalingHandler::sessions_)
     */
    vcs::Response HandleExternalBye(const std::string& sessionId, const std::string& conferenceId, const std::string& reason);

    /**
     * @brief Handle SIP ACK
     */
    vcs::Response HandleAck(const std::string& sessionId, const Params& params);

    /**
     * @brief Handle SIP CANCEL
     */
    vcs::Response HandleCancel(const std::string& sessionId);

    // ============================================================
    // Session management methods (called by SipAdapterMediaCallback)
    // ============================================================

    /**
     * @brief Update session state
     */
    void UpdateSessionState(const std::string& sessionId, SessionState state);

    /**
     * @brief Cleanup session
     */
    void CleanupSession(const std::string& sessionId);

    /**
     * @brief Send BYE
     */
    void SendBye(const std::string& sessionId);

    /**
     * @brief Get session context
     */
    SessionContext* GetSessionContext(const std::string& sessionId);

    // ============================================================
    // Media transport creation methods
    // ============================================================

    /**
     * @brief Get pj::Endpoint pointer
     *
     * @return pj::Endpoint pointer (returns nullptr if not initialized)
     */
    void* GetPjsipEndpoint() const;

    /**
     * @brief Get pj::Endpoint reference
     *
     * @return pj::Endpoint reference
     * @note Only call when already initialized
     */
    pj::Endpoint& GetPjsipEndpointRef() const;

    /**
     * @brief Get local audio port
     */
    uint16_t GetLocalAudioPort() const { return localAudioPort_; }

    /**
     * @brief Get local video port
     */
    uint16_t GetLocalVideoPort() const { return localVideoPort_; }

    /**
     * @brief Get local secondary-video (content sharing) port
     */
    uint16_t GetLocalSecondaryVideoPort() const { return localSecondaryVideoPort_; }

    /**
     * @brief Set local media address (for external SDP generation)
     * @param address Local IP address
     */
    void SetLocalMediaAddress(const std::string& address);

    /**
     * @brief Get local media address (for external SDP generation)
     */
    const std::string& GetLocalMediaAddress() const { return localMediaAddress_; }

    /**
     * @brief Set local media ports (for external SDP generation)
     * @param audioPort Audio port
     * @param videoPort Video port
     */
    void SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort);

    /**
     * @brief Set local media ports including the secondary-video port
     *        (for external SDP generation and three-stream transport).
     * @param audioPort           Audio port
     * @param videoPort           Main video port
     * @param secondaryVideoPort  Secondary video (content sharing) port
     */
    void SetLocalMediaPorts(uint16_t audioPort,
                            uint16_t videoPort,
                            uint16_t secondaryVideoPort);

    /**
     * @brief Create media transport and inject into PJSIP
     *
     * @param callId Session's callId
     * @param remoteAddress Remote address
     * @param remotePort Remote port
     * @param call PJSIP Call object reference (for getting conference port)
     */
    void CreateMediaTransportWithInject(int callId, const std::string& remoteAddress,
                                         uint16_t remotePort, pj::Call& call);

    /**
     * @brief Create media transport (without injection mode)
     *
     * @param callId Session's callId
     * @param remoteAddress Remote address
     * @param remotePort Remote port
     */
    void CreateMediaTransport(int callId, const std::string& remoteAddress, uint16_t remotePort);

    /**
     * @brief Register SDP replacement module
     *
     * Register a PJSIP module to intercept 200 OK messages,
     * replace video line in SDP before sending (port from 0 to 4002).
     */
    void RegisterSdpReplacementModule();

private:
    /// Notify session count change
    void notifySessionCountChanged();

    // The one place that shuts down the pjmedia-bound transports of a
    // session. Every teardown path (Stop / TerminateCall / external BYE) must
    // go through here: pjsua hangs up by destroying call media synchronously,
    // so the send thread and the stack's external receive callback have to be
    // detached *before* the stack side is torn down. Returns the number of
    // transports that were disconnected.
    size_t ShutdownSessionTransports(const std::string& sessionId, const char* callerTag);

    // ShutdownSessionTransports + SessionManager::MarkTerminated. Used by
    // teardown paths that do not have a pjsua call to hang up.
    void TeardownSession(const std::string& sessionId, const std::string& reason,
                         const char* callerTag);

    // Bind one stream of a call to the stack's pjmedia transport.
    //
    // A call's transport object is stable for its whole life. The media layer
    // pinned the shared_ptr handed out on the first CONFIRMED, and
    // SessionManager::SetTransport StopReceiving()s the object it replaces - so
    // creating a second object on a re-CONFIRMED would silently kill the live
    // stream. An existing object is therefore reused and only its address
    // metadata is refreshed.
    //
    // Returns nullptr when the transport is unusable: no pjmedia transport, or
    // the stack replaced it underneath the call (the media layer cannot follow
    // a swap, so the stale object is detached loudly rather than left to send
    // through a pointer pjsua may free).
    std::shared_ptr<vcs::IRtpTransport> BindOrReuseTransport(
        const std::shared_ptr<CallSession>& session,
        vcs::MediaStreamKind kind,
        const std::string& address,
        int port,
        pjmedia_transport* pjTp);

    // ============================================================
    // ISignalingHandlerCallback Implementation
    // ============================================================
    void OnCallIncoming(int callId, const std::string& caller, const vcs::Params& params) override;
    void OnCallStateChanged(int callId, SessionState state) override;
    void OnCallConnected(int callId) override;
    void OnCallTerminated(int callId, const std::string& reason) override;
    void OnSignalingError(int callId, int errorCode, const std::string& errorMsg) override;

    // ============================================================
    // External call establishment callback handling (called by SipExternalModule)
    //
    // [V7.0 Phase-2 G] Returns true on successful full backend setup
    // (transport, CallSession, conference join all wired up). The SIP
    // signaling handler will only forward 200 OK to the peer after this
    // returns true, so returning false causes a 5xx error response instead
    // and prevents the peer from streaming RTP into a half-initialized
    // receiver.
    // ============================================================
    bool onExternalCallEstablished(pjsip_inv_session* inv, const RemoteMediaInfo& remoteInfo);

    // ============================================================
    // Private helper methods
    // ============================================================

    /// Parse conference ID
    std::string ParseConferenceId(const Params& params);

    /// Map conference error code to SIP response code
    int MapConferenceErrorToSipCode(int errorCode);

    /// Create media transport (with callback and context)
    std::shared_ptr<SimpleRtpTransport> CreateMediaTransportWithCallback(
        int callId,
        const SessionContext& context,
        const RemoteMediaInfo& remoteInfo);

private:
    std::string type_;
    std::string listenAddress_;
    uint16_t port_;
    bool running_;

    std::shared_ptr<vcs::IEventBus> eventBus_;
    IProtocolAdapterCallback* callback_;

    std::unique_ptr<SipSignalingHandler> signalingHandler_;

    // [V5.6] SessionManager (non-owning, injected by GatewayManager via SetSessionManager)
    // Lifetime is managed by GatewayManager; this adapter only borrows the pointer.
    SessionManager* sessionManager_ = nullptr;

    // SRTP configuration (set by application layer via ConfigureSrtp())
    vcs::media_config::SrtpConfig srtpConfig_;

    // Video codec configuration (set by Conference via ConfigureVideoCodec())
    // Used for SDP video negotiation
    vcs::media_config::VideoCodecConfig videoConfig_;
    bool videoConfigValid_;

    // Local RTP port configuration (for external SDP generation)
    // Read from configuration file by GatewayFactory
    std::string localMediaAddress_;             // Local media IP address (set by GatewayFactory via SetLocalMediaAddress)
    uint16_t localAudioPort_         = 4004;    // Local audio port
    uint16_t localVideoPort_         = 4002;    // Local main-video port
    uint16_t localSecondaryVideoPort_= 4006;    // Local secondary-video (content sharing) port

    // PJSIP Endpoint - unified management
    std::unique_ptr<pj::Endpoint> pjsipEndpoint_;
};

}} // namespace vcs::gateway
