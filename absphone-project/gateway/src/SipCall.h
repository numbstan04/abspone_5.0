#pragma once

/**
 * @file SipCall.h
 * @brief SIP Call Independent Implementation
 *
 * Independent SipCall class that handles a single SIP call.
 * Communicates with the upper layer through the ISipSignalingCallback interface.
 */

#include "SipCallHandler.h"
#include "gateway_export.h"
#include "NegotiatedCodec.h"
#include <memory>
#include <string>
#include <vector>

// PJSIP always enabled
#include <pjsua2.hpp>

// Forward declare PJSIP invite session struct so we can return a pointer
// to it from a public method without dragging the full pjsip.h into every
// translation unit that includes SipCall.h.
struct pjsip_inv_session;
struct pjsip_rx_data;
struct pjmedia_stream;

namespace vcs { namespace gateway {

/**
 * @brief SIP Call Handler
 *
 * Independent SIP call class, inherits from pjsua2::Call, handling:
 * 1. Call state management
 * 2. Media negotiation and state
 * 3. DTMF signals
 *
 * Notifies the upper layer through the ISipSignalingCallback interface.
 */
class SipCall : public pj::Call
{
public:
    /**
     * @brief Constructor (inbound call)
     * @param account Associated account
     * @param callId PJSIP call ID
     * @param callback Callback interface pointer
     */
    SipCall(pj::Account& account, int callId, ISipSignalingCallback* callback);

    /**
     * @brief Constructor (outbound call)
     * @param account Associated account
     * @param callback Callback interface pointer
     */
    explicit SipCall(pj::Account& account, ISipSignalingCallback* callback);

    /**
     * @brief Destructor
     */
    ~SipCall() override;

    /**
     * @brief Get call info
     */
    const SipCallInfo& GetInfo() const { return callInfo_; }

    /**
     * @brief Get call ID
     */
    int GetCallId() const { return callInfo_.callId; }

    /**
     * @brief Get remote URI
     */
    const std::string& GetRemoteUri() const { return callInfo_.remoteUri; }

    /**
     * @brief Check whether the call is active
     */
    bool IsActive() const { return callInfo_.state == PJSIP_INV_STATE_CONFIRMED; }

    /**
     * @brief Get media info list
     */
    const std::vector<SipMediaInfo>& GetMediaInfo() const { return mediaInfoList_; }

    /**
     * @brief Get remote address info for all media (prefer negotiation result, fallback to original INVITE SDP)
     * @param audioAddr Output: audio address
     * @param audioPort Output: audio port
     * @param videoAddr Output: video address
     * @param videoPort Output: video port
     */
    void GetAllMediaAddresses(std::string& audioAddr, int& audioPort,
                             std::string& videoAddr, int& videoPort) const;

    /**
     * @brief Parse negotiated codec parameters from the active remote SDP
     *
     * Fills codecName/payloadType/clockRate/fmtp for the first audio and first
     * video media lines. The fmtp value is stored without the "<pt> " prefix.
     * @return true if at least one of the two codecs was found
     */
    bool GetRemoteCodecs(vcs::NegotiatedCodec& audioCodec,
                         vcs::NegotiatedCodec& videoCodec) const;

    /**
     * @brief Read the RTP ports the media transports are actually bound to.
     *
     * The SDP we emit must advertise these ports, not the configured ones:
     * pjmedia picks the real ports at runtime, and a mismatch makes the peer
     * send to a port nobody listens on (only symmetric-RTP latching hides it).
     *
     * @return true if at least one port was read
     */
    bool GetLocalMediaPorts(int& audioPort, int& videoPort) const;

    /**
     * @brief Get remote address info for the secondary video stream (content:slides or content:sub)
     * @param secondaryVideoAddr Output: secondary video address
     * @param secondaryVideoPort Output: secondary video port
     * @param secondaryVideoPayloadType Output: secondary video payload type (0 if not found)
     * @return true if secondary video was found, false otherwise
     */
    bool GetSecondaryVideoAddress(std::string& secondaryVideoAddr,
                                  int& secondaryVideoPort,
                                  uint8_t& secondaryVideoPayloadType) const;

    /**
     * @brief Get remote address info for the MAIN video stream (content:main, or first video with no content attr)
     * @param mainVideoAddr Output: main video address
     * @param mainVideoPort Output: main video port
     * @param mainVideoPayloadType Output: main video payload type (0 if not found)
     * @return true if main video was found, false otherwise
     */
    bool GetMainVideoAddress(std::string& mainVideoAddr,
                            int& mainVideoPort,
                            uint8_t& mainVideoPayloadType) const;

    /**
     * @brief Parse and store remote ports from original INVITE SDP (fallback when video negotiation fails)
     * @param sdpText SDP text content
     */
    void ParseAndStoreRemotePortsFromSdp(const std::string& sdpText);

    /**
     * @brief Get remote audio port parsed from original INVITE SDP
     */
    int GetRemoteAudioPortFromInvite() const { return remoteAudioPortFromInvite_; }

    /**
     * @brief Get remote video port parsed from original INVITE SDP
     */
    int GetRemoteVideoPortFromInvite() const { return remoteVideoPortFromInvite_; }

    /**
     * @brief Initiate outbound call
     * @param target Target URI
     * @param audioCount Number of audio streams
     * @param videoCount Number of video streams
     * @return true on success, false on failure
     */
    bool MakeCall(const std::string& target, int audioCount = 1, int videoCount = 0);

    /**
     * @brief Answer the call
     * @param code Status code (default 200 OK)
     * @return true on success, false on failure
     */
    bool Answer(int code = 200);

    /**
     * @brief Reject the call
     * @param code Status code (default 486 Busy Here)
     * @return true on success, false on failure
     */
    bool Reject(int code = PJSIP_SC_BUSY_HERE);

    /**
     * @brief Hang up the call
     * @param code Status code (default normal release)
     * @return true on success, false on failure
     */
    bool Hangup(int code = PJSIP_SC_OK);

    /**
     * @brief Hold the call
     * @return true on success, false on failure
     */
    bool Hold();

    /**
     * @brief Resume the call
     * @return true on success, false on failure
     */
    bool Unhold();

    /**
     * @brief Send DTMF
     * @param digits DTMF digits
     * @return true on success, false on failure
     */
    bool SendDTMF(const std::string& digits);

    /**
     * @brief Request a key frame (for video)
     * @return true on success, false on failure
     */
    bool RequestKeyFrame();

    /**
     * @brief Get PJSIP media transport pointer
     * @param mediaIndex Media index
     * @return pjmedia_transport pointer, nullptr on failure
     */
    pjmedia_transport* GetPjMediaTransport(unsigned mediaIndex);

    /**
     * @brief Send RTP packet directly to remote (bypassing conference bridge)
     * @param mediaIndex Media index
     * @param rtpData RTP packet data
     * @param size Data size
     * @return true on success, false on failure
     */
    bool SendRtpPacket(unsigned mediaIndex, const void* rtpData, size_t size);

    /**
     * @brief Set local media ports (for external SDP generation)
     * @param audioPort Audio port
     * @param videoPort Video port
     */
    void SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort) {
        localAudioPort_ = audioPort;
        localVideoPort_ = videoPort;
    }

    /**
     * @brief Declare whether the application owns the RTP TX path for this call.
     *
     * True when the media transport sends RTP itself (SimpleRtpTransport with
     * its own socket, or SipMediaTransport passthrough over pjmedia's socket).
     * The signaling stack then stays signaling-only: pjmedia must not emit its
     * own audio/video stream, otherwise two SSRCs leave the same 5-tuple and
     * the remote mixer latches one of them.
     *
     * Keep false for a transport that reuses the stack's own media engine.
     */
    void SetExternalMediaPlane(bool enabled) { externalMediaPlane_ = enabled; }

    /**
     * @brief Set local IP address (for external SDP generation)
     * @param ip Local IP address
     */
    void SetLocalIpAddress(const std::string& ip) {
        localIpAddress_ = ip;
    }

    /**
     * @brief Set local signaling port (used in the Contact header of the
     *        raw 200 OK; 0 = fall back to the INVITE transport's local port)
     */
    void SetLocalSignalingPort(uint16_t port) {
        localSignalingPort_ = port;
    }

    /**
     * @brief Take a private clone of the incoming INVITE request.
     *
     * pjsua only keeps pjsua_call::incoming_data during the
     * on_incoming_call callback and frees it when the callback returns
     * (pjsua_call.c on_return). The external-SDP answer path builds a raw
     * response from that request, so it must clone it while the callback is
     * still on the stack; otherwise a delayed Answer() sees rdata == NULL.
     *
     * @return true when the clone was stored
     */
    bool CloneIncomingRequest(pjsip_rx_data* src);

    /**
     * @brief Get local IP address
     */
    const std::string& GetLocalIpAddress() const { return localIpAddress_; }

    /**
     * @brief Get local audio port
     */
    uint16_t GetLocalAudioPort() const { return localAudioPort_; }

    /**
     * @brief Get local video port
     */
    uint16_t GetLocalVideoPort() const { return localVideoPort_; }

    /**
     * @brief Get the underlying pjsip_inv_session* pointer (used as key in handler maps)
     * @return pjsip_inv_session pointer, or nullptr if the call/inv is invalid
     */
    pjsip_inv_session* GetInvSession() const;

protected:
    // ========== pjsua2::Call Callbacks ==========

    /// Call state change callback
    void onCallState(pj::OnCallStateParam& prm) override;

    /// Media state change callback
    void onCallMediaState(pj::OnCallMediaStateParam& prm) override;

    /// Audio stream created callback (fires right after pjmedia_stream_start)
    void onStreamCreated(pj::OnStreamCreatedParam& prm) override;

    /// Media transport state callback
    void onCallMediaTransportState(pj::OnCallMediaTransportStateParam& prm) override;

    /// [Key] Callback when receiving a remote offer - invoked before generating answer SDP.
    /// The offer can be modified and video capabilities set here.
    void onCallRxOffer(pj::OnCallRxOfferParam& prm) override;


    /// SDP creation callback
    void onCallSdpCreated(pj::OnCallSdpCreatedParam& prm) override;

private:
    /// Update call info
    void UpdateCallInfo();

    /// Parse call state to text
    std::string GetStateText(uint32_t state);

    ISipSignalingCallback* callback_;
    SipCallInfo callInfo_;
    std::vector<SipMediaInfo> mediaInfoList_;
    bool isOriginated_;  // Whether this side is the originator

    // Save modified local SDP (used by pjsua_call_answer_with_sdp)
    pjmedia_sdp_session* localSdpForAnswer_;

    // Application-owned RTP plane: pjmedia's audio encoder is paused so the
    // stack does not emit a second RTP stream next to the passthrough.
    bool externalMediaPlane_ = false;
    pjmedia_stream* audioStream_ = nullptr;

    // Local media configuration (set by SipSignalingHandler when creating the Call)
    std::string localIpAddress_;       // Local IP address (from config or SipAdapter)
    uint16_t localAudioPort_ = 4004;  // Local audio port
    uint16_t localVideoPort_ = 4002;  // Local video port
    uint16_t localSignalingPort_ = 0; // Local SIP port for the Contact header

    // Private clone of the INVITE that started this call (external-SDP answer).
    // Owned by this object; released with pjsip_rx_data_free_cloned().
    pjsip_rx_data* incomingRdata_ = nullptr;

    // Remote ports parsed from the original INVITE SDP (fallback when video negotiation fails)
    int remoteAudioPortFromInvite_ = 0;
    int remoteVideoPortFromInvite_ = 0;
    std::string remoteAddressFromInvite_;  // Remote IP address parsed from the SDP c= line
};

}} // namespace vcs::gateway
