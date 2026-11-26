#pragma once

#include "gateway_export.h"
#include "CallSession.h"
#include "Response.h"
#include "IRtpTransport.h"
#include <string>
#include <memory>
#include <vector>

namespace vcs {

class Params;

namespace gateway {

/**
 * @brief Media transport flavour selected by a signaling adapter.
 */
enum class MediaTransportType {
    Sip,      // SIP RTP media transport
    H323,     // H.323 RTP media transport
    Webrtc,   // WebRTC DTLS/SRTP transport
    Custom,   // User-defined transport
};

/**
 * @brief Signaling handler interface
 *
 * Responsible for protocol stack processing and session state machine management.
 *
 * Design Principles:
 * 1. Each protocol adapter corresponds to one SignalingHandler
 * 2. SignalingHandler internally manages all sessions (CallSession)
 * 3. Interacts with upper layer via ISignalingHandlerCallback
 * 4. Session state transitions are completed inside SignalingHandler
 *
 * Session State Machine:
 * IncomingPending -> Connecting -> Active -> Disconnected
 *      ^                ^           ^
 *      |------ Reject() |           |
 *                                  -- Disconnect() / BYE
 */
class ISignalingHandler {
public:
    virtual ~ISignalingHandler() = default;

    // ============================================================
    // Lifecycle
    // ============================================================

    /// Start signaling handler
    virtual bool Start() = 0;

    /// Stop signaling handler
    virtual void Stop() = 0;

    // ============================================================
    // Session queries
    // ============================================================

    /// Get session by callId
    virtual CallSession* GetSession(int callId) = 0;

    /// Get session by sessionId (used by adapter layer)
    virtual CallSession* GetSessionById(const std::string& sessionId) = 0;

    /// Update session state (used by adapter layer)
    virtual void UpdateSessionState(int callId, SessionState state) = 0;

    /// Set session media transport (used by adapter layer)
    virtual void SetSessionMediaTransport(int callId, std::shared_ptr<IRtpTransport> transport) = 0;

    /// Get session media transport (used by adapter layer)
    virtual std::shared_ptr<IRtpTransport> GetSessionMediaTransport(int callId) = 0;

    /// Get all active callId list
    virtual std::vector<int> GetActiveCallIds() const = 0;

    /// Get active session count
    virtual size_t GetActiveSessionCount() const = 0;

    // ============================================================
    // Call control
    // ============================================================

    /// Initiate outgoing call
    /// @param callee Callee
    /// @param params Call parameters
    /// @param sessionId Output: created session ID
    /// @return Response execution result
    virtual vcs::Response InitiateCall(const std::string& callee,
                                        const Params& params,
                                        std::string& sessionId) = 0;

    /// Accept incoming call
    /// @param callId Protocol stack call ID
    /// @param params Response parameters (e.g., SDP answer)
    /// @return Response execution result
    virtual vcs::Response AcceptCall(int callId, const Params& params) = 0;

    /// Reject incoming call
    /// @param callId Protocol stack call ID
    /// @param reason Rejection reason
    /// @return Response execution result
    virtual vcs::Response RejectCall(int callId, const std::string& reason) = 0;

    /// Actively terminate call
    /// @param callId Protocol stack call ID
    /// @param reason Termination reason
    /// @return Response execution result
    virtual vcs::Response TerminateCall(int callId, const std::string& reason) = 0;
};

/**
 * @brief Signaling handler callback interface
 *
 * Used for protocol stack event callbacks to upper layer
 */
class ISignalingHandlerCallback {
public:
    virtual ~ISignalingHandlerCallback() = default;

    /// New call received (incoming)
    /// @param callId Protocol stack call ID
    /// @param caller Caller
    /// @param params Call parameters
    virtual void OnCallIncoming(int callId,
                                const std::string& caller,
                                const vcs::Params& params) = 0;

    /// Call state changed
    /// @param callId Protocol stack call ID
    /// @param state New state
    virtual void OnCallStateChanged(int callId, SessionState state) = 0;

    /// Call connected (received ACK or sent 200 OK)
    /// @param callId Protocol stack call ID
    virtual void OnCallConnected(int callId) = 0;

    /// Call terminated
    /// @param callId Protocol stack call ID
    /// @param reason Termination reason
    virtual void OnCallTerminated(int callId, const std::string& reason) = 0;

    /// Signaling error
    /// @param callId Protocol stack call ID (0 if global error)
    /// @param errorCode Error code
    /// @param errorMsg Error message
    virtual void OnSignalingError(int callId,
                                  int errorCode,
                                  const std::string& errorMsg) = 0;
};

} // namespace gateway
} // namespace vcs
