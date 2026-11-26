#pragma once

#include "gateway_export.h"
#include "Response.h"
#include "IRtpTransport.h"
#include "ISignalingHandler.h"
#include "EventBus.h"
#include "CallSession.h"
#include "MediaConfig.h"
#include <string>
#include <memory>
#include <vector>

namespace vcs {

class Params;

namespace gateway {

// Forward declarations
class IProtocolAdapter;
class SessionManager;

/**
 * @brief Protocol adapter callback interface
 *
 * Notifies upper layer when session state managed by the adapter changes.
 *
 * Design principles:
 * 1. Only notify state changes, do not handle business logic
 * 2. Sessions managed internally by adapter via SignalingHandler
 * 3. Business logic handled by CommandDispatcher
 */
class IProtocolAdapterCallback {
public:
    virtual ~IProtocolAdapterCallback() = default;

    /**
     * @brief Adapter's session count changed
     *
     * @param adapterType Adapter type
     * @param activeCount Current active session count
     */
    virtual void OnAdapterSessionCountChanged(const std::string& adapterType,
                                             size_t activeCount) = 0;

    /**
     * @brief Adapter error occurred
     *
     * @param adapterType Adapter type
     * @param errorCode Error code
     * @param errorMsg Error message
     */
    virtual void OnAdapterError(const std::string& adapterType,
                               int errorCode,
                               const std::string& errorMsg) = 0;

};

/**
 * @brief Protocol adapter interface
 *
 * Defines unified interface for all protocol adapters (SIP, H.323, WebRTC).
 *
 * Design principles:
 * 1. Aggregate ISignalingHandler for signaling processing
 * 2. Sessions managed internally by SignalingHandler (CallSession)
 * 3. Business logic observes this layer through platform events on IEventBus;
 *    the adapter never knows the Conference layer types
 * 4. Each adapter can manage multiple concurrent sessions
 */
class IProtocolAdapter {
public:
    virtual ~IProtocolAdapter() = default;

    // ============================================================
    // Basic information
    // ============================================================

    /// Get adapter type (SIP/H323/WebRTC)
    virtual const std::string& GetType() const = 0;

    /// Get listen address
    virtual std::string GetListenAddress() const = 0;

    /// Get listen port
    virtual uint16_t GetPort() const = 0;

    // ============================================================
    // Lifecycle management
    // ============================================================

    virtual vcs::Response Start() = 0;
    virtual vcs::Response Stop() = 0;
    virtual bool IsRunning() const = 0;

    // ============================================================
    // Business layer integration
    // ============================================================

    /// Set callback
    virtual void SetCallback(IProtocolAdapterCallback* callback) = 0;
    virtual IProtocolAdapterCallback* GetCallback() const = 0;

    /// Get signaling handler
    virtual ISignalingHandler* GetSignalingHandler() = 0;

    /// Platform event bus. Gateway only publishes signaling facts (incoming call,
    /// call terminated, ...) and never calls into upper layers.
    virtual void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) = 0;
    virtual std::shared_ptr<vcs::IEventBus> GetEventBus() const = 0;


    // ============================================================
    // [V5.6] SessionManager injection (shared across all adapters)
    // ============================================================

    /**
     * @brief Inject the shared SessionManager (owned by GatewayManager)
     *
     * Must be called before Start(). The adapter stores the pointer (non-owning)
     * and forwards it to its internal SignalingHandler.
     *
     * @param sm Pointer to the shared SessionManager (lifetime managed by GatewayManager)
     */
    virtual void SetSessionManager(SessionManager* sm) = 0;

    /**
     * @brief Get the injected SessionManager (may return nullptr before SetSessionManager)
     */
    virtual SessionManager* GetSessionManager() const = 0;

    // ============================================================
    // Session query
    // ============================================================

    /// Get all active session IDs
    virtual std::vector<std::string> GetActiveSessionIds() const = 0;

    /// Get specified session information
    virtual CallSessionInfo* GetSessionInfo(const std::string& sessionId) = 0;

    /// Get active session count
    virtual size_t GetActiveSessionCount() const = 0;

    // ============================================================
    // Call control
    // ============================================================

    /// Initiate outbound call
    /// @param callee Callee
    /// @param params Call parameters
    /// @param sessionId Output: created session ID
    virtual vcs::Response InitiateCall(const std::string& callee,
                                       const Params& params,
                                       std::string& sessionId) = 0;

    /// Accept inbound call
    /// @param sessionId Session ID
    /// @param params Response parameters
    virtual vcs::Response AcceptCall(const std::string& sessionId,
                                       const Params& params) = 0;

    /// Reject inbound call
    /// @param sessionId Session ID
    /// @param reason Rejection reason
    virtual vcs::Response RejectCall(const std::string& sessionId,
                                       const std::string& reason) = 0;

    /// Terminate call
    /// @param sessionId Session ID
    /// @param reason Termination reason
    virtual vcs::Response TerminateCall(const std::string& sessionId,
                                         const std::string& reason) = 0;

    /// Terminate all calls
    virtual vcs::Response TerminateAllCalls() = 0;

    /// Send DTMF digits (RFC 2833 / SIP INFO) on an active call
    /// @param sessionId Session ID
    /// @param digits DTMF string, e.g. "1234#"
    /// Adapters that cannot carry DTMF keep this default and report NotImplemented.
    virtual vcs::Response SendDTMF(const std::string& sessionId,
                                   const std::string& digits) {
        (void)sessionId;
        (void)digits;
        return vcs::Response::Error(vcs::ResponseCode::NotImplemented,
                                    "DTMF not supported by this adapter");
    }



    // ============================================================
    // Inbound call handling
    // ============================================================

    /**
     * @brief Handle inbound call (triggered by protocol stack internal callback)
     *
     * Adapter creates session internally and processes via SignalingHandler.
     *
     * @param sessionId Session ID (generated by protocol stack)
     * @param caller Caller identifier
     * @param params Call parameters
     */
    virtual void HandleIncomingCall(const std::string& sessionId,
                                    const std::string& caller,
                                    const Params& params) = 0;

    // ============================================================
    // Codec configuration (called by Conference module)
    // ============================================================

    /**
     * @brief Configure video codec parameters (for SDP negotiation)
     *
     * Called by Conference module, passes conference-configured codec parameters.
     * PJSIP generates SDP offer/answer based on this.
     *
     * @param config Video codec configuration
     * @return Configuration result
     */
    virtual vcs::Response ConfigureVideoCodec(const vcs::media_config::VideoCodecConfig& config) = 0;

    /**
     * @brief Configure audio codec parameters
     *
     * @param config Audio codec configuration
     * @return Configuration result
     */
    virtual vcs::Response ConfigureAudioCodec(const vcs::media_config::AudioCodecConfig& config) = 0;

    // ============================================================
    // SRTP configuration (configured by application layer)
    // ============================================================

    /**
     * @brief Configure SRTP encryption parameters
     *
     * Called by application layer, decides whether to enable SRTP encryption.
     * Application layer should decide configuration based on capabilities of connected devices (e.g. Yealink doesn't support SRTP).
     *
     * @param config SRTP configuration
     * @return Configuration result
     */
    virtual vcs::Response ConfigureSrtp(const vcs::media_config::SrtpConfig& config) = 0;

    // ============================================================
    // Local media configuration (for external SDP generation, called by GatewayFactory)
    // ============================================================

    /**
     * @brief Set local media address (for SDP generation)
     *
     * Called by GatewayFactory when creating adapter, read from configuration file.
     * This address will be used in SIP 200 OK SDP (c= line and m= line port).
     *
     * @param address Local IP address (e.g. 172.16.40.158)
     */
    virtual void SetLocalMediaAddress(const std::string& address) = 0;

    /**
     * @brief Set local media ports (for SDP generation)
     *
     * Called by GatewayFactory when creating adapter, read from configuration file.
     * Audio and video use different ports.
     *
     * @param audioPort Local audio RTP port
     * @param videoPort Local video RTP port
     */
    virtual void SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort) = 0;
};

} // namespace gateway
} // namespace vcs