#pragma once

/**
 * @file SipCallHandler.h
 * @brief SIP call and account handler interface definition
 *
 * Refactor SipAccount and SipCall into independent classes, communicating with SipSignalingHandler via callback interface.
 * This facilitates extension, testing, and maintenance.
 */

#include "gateway_export.h"
#include <string>
#include <cstdint>

// PJSIP always enabled
#include <pjsua2.hpp>

namespace vcs { namespace gateway {

// Forward declaration
class SipAccount;
class SipCall;

/**
 * @brief SIP signaling handler callback interface
 *
 * SipAccount and SipCall notify SipSignalingHandler events through this interface.
 * Use interface pattern for decoupling, facilitating extension and unit testing.
 */
class ISipSignalingCallback {
public:
    virtual ~ISipSignalingCallback() = default;

    // ========== Account-related callbacks ==========

    /// Registration state changed
    virtual void OnRegistrationState(int accountId, uint32_t code, const std::string& reason) = 0;

    /// Registration success
    virtual void OnRegistrationSuccess(int accountId) = 0;

    /// Registration failure
    virtual void OnRegistrationFailure(int accountId, uint32_t code, const std::string& reason) = 0;

    // ========== Call-related callbacks ==========

    /// Incoming call
    virtual void OnIncomingCall(int accountId, int callId, const std::string& callerUri,
                                const std::string& callerDisplayName) = 0;

    /// Call state changed
    virtual void OnCallState(int callId, uint32_t state, const std::string& stateText) = 0;

    /// Call media ready
    /// @param callId Call ID
    /// @param remoteAddress Remote media address
    /// @param remotePort Remote media port
    /// @param audioCodecName Negotiated audio codec name (e.g. "PCMU", "G722")
    /// @param audioPayloadType Audio payload type
    /// @param videoCodecName Negotiated video codec name (e.g. "H264", "VP8")
    /// @param videoPayloadType Video payload type
    ///
    /// [V5.4 Extended] Secondary stream parameters (current feature not implemented, interface reserved)
    /// @param hasSecondaryVideo Whether secondary video exists (screen sharing)
    /// @param secondaryVideoCodecName Secondary video codec name
    /// @param secondaryVideoPayloadType Secondary video payload type
    virtual void OnCallMediaReady(int callId,
                                  const std::string& remoteAddress,
                                  int remotePort,
                                  const std::string& audioCodecName = "",
                                  uint8_t audioPayloadType = 0,
                                  const std::string& videoCodecName = "",
                                  uint8_t videoPayloadType = 0,
                                  // [V5.4 New] Secondary stream parameters (reserved)
                                  bool hasSecondaryVideo = false,
                                  const std::string& secondaryVideoCodecName = "",
                                  uint8_t secondaryVideoPayloadType = 0) = 0;

    /// Call disconnected
    virtual void OnCallDisconnected(int callId, uint32_t code, const std::string& reason) = 0;

    /// Media transmission statistics
    virtual void OnMediaStatistics(int callId, uint32_t bytesSent, uint32_t bytesReceived,
                                   uint32_t packetsSent, uint32_t packetsReceived) = 0;
};

/**
 * @brief SIP call information
 *
 * Store active call-related information for easy management and query.
 */
struct SipCallInfo {
    int callId = -1;
    int accountId = -1;
    std::string localUri;
    std::string remoteUri;
    std::string remoteDisplayName;
    uint32_t state = 0;
    std::string stateText;
    bool mediaActive = false;
    std::string remoteMediaAddress;
    int remoteMediaPort = 0;
    uint64_t connectTime = 0;
    uint64_t duration = 0;
};

/**
 * @brief SIP account information
 */
struct SipAccountInfo {
    int accountId = -1;
    std::string uri;
    std::string registrar;
    std::string username;
    bool registered = false;
    uint32_t expires = 0;
};

/**
 * @brief SIP media information
 *
 * [V5.4 Extended] Reserved fields for secondary stream (screen sharing)
 *
 * Video streams in SDP are distinguished by a=content: attribute:
 * - Main video: m=video ... a=content:main
 * - Secondary video: m=video ... a=content:slides
 *
 * contentType field records the type of this media stream.
 */
struct SipMediaInfo {
    int mediaIndex = -1;
    std::string type;  // "audio" or "video"
    uint8_t payloadType = 0;
    std::string codecName;
    uint32_t sampleRate = 0;
    uint16_t localPort = 0;
    std::string remoteAddress;
    uint16_t remotePort = 0;
    bool status = false;

    // [V5.4 New] Reserved fields for secondary stream support
    // a=content: attribute value in SDP
    // Common values: "main" (main stream), "slides" (secondary/screen sharing), "sub" (sub stream)
    // If empty, means not set or legacy terminal
    std::string contentType;

    // Video stream index, used to distinguish multiple video streams
    // 0 = First video stream (usually main)
    // 1 = Second video stream (usually secondary)
    // -1 = Not used/invalid
    int videoStreamIndex = -1;

    // [Future Extension] Reserved fields
    // If secondary stream needs independent codec parameters, can be added here
    // uint8_t secondaryPayloadType = 0;
    // std::string secondaryCodecName;
};

}} // namespace vcs::gateway
