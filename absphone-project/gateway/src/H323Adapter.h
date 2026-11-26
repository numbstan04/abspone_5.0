#pragma once

#include "IProtocolAdapter.h"
#include "SignalingHandler.h"
#include "H323MediaTransport.h"
#include "CallSession.h"
#include "IRtpTransport.h"
#include "SessionContext.h"
#include <string>
#include <memory>
#include <atomic>
#include <thread>

namespace vcs { namespace gateway {

// Forward declaration
class SessionManager;

/**
 * @brief H.323 Protocol Adapter
 *
 * H.323 protocol adapter implementation.
 *
 * Design notes:
 * 1. Aggregates a SignalingHandler for signaling processing
 * 2. Manages session lifecycle (via SignalingHandler's CallSession)
 * 3. Interacts with the Conference layer through CommandDispatcher
 * 4. Handles H.323 SETUP/CONNECT/RELEASE signaling
 */
class H323Adapter : public IProtocolAdapter, public IRtpTransportCallback {
public:
    /**
     * @brief Constructor
     * @param listenAddress Listen address
     * @param port Listen port (default 1720)
     */
    explicit H323Adapter(const std::string& listenAddress, uint16_t port = 1720);
    ~H323Adapter() override;

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

    // [V7.0 Phase-3] SessionManager injection (non-owning, set by GatewayManager).
    // The H.323 adapter itself never touches SessionManager directly: all session
    // operations are forwarded to the embedded SignalingHandler, which routes
    // everything through the single shared SessionManager.
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

    void HandleIncomingCall(const std::string& sessionId,
                            const std::string& caller,
                            const Params& params) override;

    // ============================================================
    // Codec Configuration
    // ============================================================

    vcs::Response ConfigureVideoCodec(const vcs::media_config::VideoCodecConfig& config) override;
    vcs::Response ConfigureAudioCodec(const vcs::media_config::AudioCodecConfig& config) override;
    vcs::Response ConfigureSrtp(const vcs::media_config::SrtpConfig& config) override;

    // ============================================================
    // Local Media Configuration (for external SDP generation)
    // ============================================================

    void SetLocalMediaAddress(const std::string& address) override;
    void SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort) override;

    // ============================================================
    // H.323-specific Methods
    // ============================================================

    vcs::Response HandleSetup(const std::string& sessionId, const Params& params);
    vcs::Response HandleConnect(const std::string& sessionId);
    vcs::Response HandleRelease(const std::string& sessionId);
    vcs::Response HandleFacility(const std::string& sessionId, const Params& params);
    vcs::Response HandleProgress(const std::string& sessionId);

    // ============================================================
    // IRtpTransportCallback Implementation (V5.4 unified interface)
    // ============================================================

    void OnRtpReceived(const RtpPacket& packet,
                       const SessionContext& context) override;
    void OnRtcpReceived(const RtcpPacket& packet,
                       const SessionContext& context) override;
    void OnTransportConnected(const SessionContext& context) override;
    void OnTransportDisconnected(const SessionContext& context,
                                 const std::string& reason) override;
    void OnTransportError(const SessionContext& context,
                         int errorCode,
                         const std::string& errorMessage) override;

    // ============================================================
    // Media Transport Creation Methods
    // ============================================================

    /**
     * @brief Create media transport for a session
     */
    void CreateMediaTransport(int callId, const std::string& remoteAddress, uint16_t remotePort);

private:
    void notifySessionCountChanged();

    // Listen thread
    void StartListenThread();
    void StopListenThread();
    void ListenLoop();

private:
    std::string type_;
    std::string listenAddress_;
    uint16_t port_;
    bool running_;

    std::shared_ptr<vcs::IEventBus> eventBus_;
    IProtocolAdapterCallback* callback_;

    // Signaling handler
    std::unique_ptr<SignalingHandler> signalingHandler_;

    // [V5.6] SessionManager (non-owning, injected by GatewayManager via SetSessionManager).
    // Lifetime is managed by GatewayManager; this adapter only borrows the pointer.
    SessionManager* sessionManager_ = nullptr;

    // Listen thread
    std::atomic<bool> listenRunning_;
    std::thread listenThread_;
};

} // namespace gateway
} // namespace vcs
