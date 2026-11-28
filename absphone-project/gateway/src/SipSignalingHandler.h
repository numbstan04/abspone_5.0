#pragma once

#include "SignalingHandler.h"
#include "SipCallHandler.h"
#include "MediaConfig.h"
#include "IMediaSdpNegotiator.h"
#include "IRtpTransport.h"
#include "RtpPortAllocator.h"
#include <memory>
#include <map>
#include <mutex>
#include <string>

// PJSIP support detection (set by CMake)
// PJSIP always enabled
#include <pjsua2.hpp>
#include <pjsip/sip_module.h>
#include <pjsip/sip_dialog.h>
#include <pjsip/sip_event.h>

namespace vcs { namespace gateway {

// Forward declaration
class SipAccount;
class SipCall;
class SipAdapter;
class ExternalSdpBuilder;

/**
 * @brief Remote media info (parsed from INVITE SDP)
 *
 * [V7.0 Phase-2 D] One terminal may carry up to N independent media streams
 * (audio + main video + secondary video for content sharing + data channel).
 * Each stream kind gets its own remote port and its own local transport.
 */
struct RemoteMediaInfo {
    std::string address;             // Remote IP address
    uint16_t audioPort = 0;          // Remote audio port
    uint16_t videoPort = 0;          // Remote main video port
    uint16_t audioPayloadType = 0;   // Audio payload type
    uint16_t videoPayloadType = 0;   // Main video payload type
    std::string conferenceId;        // Conference ID (parsed from INVITE To header)
    std::string callId;              // [V7.0 Fix] SIP Call-ID (for BYE matching)

    // [V7.0 Phase-2 D] Secondary video (content sharing / slides). A Yealink
    // that puts a second m=video line with content="slides" or content="sub"
    // negotiates this stream. It must ride its own UDP socket + transport so
    // that mixing it with the main video in a single SimpleRtpTransport
    // (PT-routed) does not silently break the main video path.
    bool        hasSecondaryVideo       = false;
    uint16_t    secondaryVideoPort      = 0;
    uint16_t    secondaryVideoPayloadType = 0;
    std::string secondaryVideoCodecName;

    // H.264 parameters (parsed from remote SDP fmtp attribute)
    std::string h264ProfileLevelId;  // profile-level-id (e.g. 640020)
    std::string h264MaxMbps;          // max-mbps (e.g. 154000)
    std::string h264MaxFs;            // max-fs (e.g. 5120)
    std::string h264SpropParameterSets;  // sprop-parameter-sets (base64 SPS,PPS)
    bool h264PacketizationMode = true;  // packetization-mode (true=1, false=0)

    // [RFC 5761] Whether the remote media line carried a=rtcp-mux.
    // When true, RTCP shares the RTP port in both directions and the SDP
    // answer must repeat a=rtcp-mux; when false, RTCP goes to RTP port + 1.
    bool audioRtcpMux = false;
    bool videoRtcpMux = false;
    bool secondaryVideoRtcpMux = false;
};

/**
 * @brief SIP Signaling Handler
 *
 * SIP signaling handler implementation based on PJSIP (if PJSIP is available).
 * If PJSIP is not available, uses stub implementation.
 *
 * Design Notes:
 * 1. Handle SIP registration, call setup/teardown and other signaling
 * 2. Inherit SignalingHandler to implement unified signaling interface and session management
 * 3. Implement ISipSignalingCallback interface for communication with SipAccount/SipCall
 * 4. SipAccount and SipCall are independent classes for easy extension and testing
 * 5. PJSIP Endpoint is created and managed by SipAdapter
 */
class SipSignalingHandler : public SignalingHandler,
                            public ISipSignalingCallback
{
public:
    /**
     * @brief Constructor
     * @param localUri Local SIP URI (e.g. "sip:user@domain.com")
     * @param localPort Local listening port
     * @param adapter SipAdapter reference (for getting pj::Endpoint)
     */
    SipSignalingHandler(const std::string& localUri,
                        uint16_t localPort,
                        SipAdapter& adapter);

    ~SipSignalingHandler() override;

    /**
     * @brief Set SIP server address (for registration)
     * @param server SIP server address (e.g. "sip:proxy.domain.com")
     * @param username Username
     * @param password Password
     */
    void SetServer(const std::string& server,
                   const std::string& username,
                   const std::string& password);

    /**
     * @brief Set SRTP configuration (configured by application layer through SipAdapter)
     *
     * This method should be called before Initialize() to ensure correct SRTP configuration when account is created.
     *
     * @param config SRTP configuration
     */
    void SetSrtpConfig(const vcs::media_config::SrtpConfig& config);

    /**
     * @brief Set media transport type
     *
     * Controls which method is used for media transport:
     * - ProtocolStack: Reuse PJSIP transport (dependency injection)
     * - WebRTC: Use WebRTC's DTLS/SRTP
     * - Custom: User-defined UDP transport (recommended for debugging and special scenarios)
     *
     * Default is ProtocolStack.
     *
     * @param type Media transport type
     */
    void SetMediaTransportType(MediaTransportType type) {
        mediaTransportType_ = type;
    }

    /**
     * @brief Get media transport type
     */
    MediaTransportType GetMediaTransportType() const {
        return mediaTransportType_;
    }

    /**
     * @brief Get local SIP URI
     */
    const std::string& GetLocalUri() const { return localUri_; }

    // ============================================================
    // Local Media Configuration (for external SDP generation)
    // ============================================================

    void SetLocalMediaAddress(const std::string& address) override;
    void SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort) override;

    /**
     * @brief Inject the application-side SDP negotiator (mediaAppManaged)
     *
     * Stored here and handed to every SipCall (outbound) plus used directly by
     * handleInvite (inbound 200 OK), so SDP is created and validated by the
     * application layer instead of pjsua.
     */
    void SetMediaSdpNegotiator(
        std::shared_ptr<vcs::media_config::IMediaSdpNegotiator> negotiator) {
        sdpNegotiator_ = std::move(negotiator);
    }

    std::shared_ptr<vcs::media_config::IMediaSdpNegotiator>
        GetMediaSdpNegotiator() const { return sdpNegotiator_; }
    // [V7.0 Phase-2 D] Three-port overload for terminals that carry audio +
    // main video + secondary video (content sharing) on three independent
    // UDP sockets / transports.
    void SetLocalMediaPorts(uint16_t audioPort,
                            uint16_t videoPort,
                            uint16_t secondaryVideoPort);

    // [V7.1 Fix 2026-08-13] Override the per-call RTP port allocator range.
    // Defaults to 4004..5098 in pairs of 2, which gives ~547 independent
    // port pairs �� enough for several hundred concurrent terminals.
    void SetRtpPortRange(uint16_t rangeStart, uint16_t rangeEnd, uint16_t stride = 2) {
        portAllocator_.Reset(rangeStart, rangeEnd, stride);
    }

    // [V7.1 Fix 2026-08-13] Look up the per-call local RTP ports reserved
    // for a given INVITE. Returns false when no entry exists, in which
    // case the caller should fall back to the global localAudioPort_/
    // localVideoPort_ via getMediaInfo().
    //
    // @param inv           PJSIP invite session
    // @param outAudioPort  filled in with the allocated audio port
    // @param outVideoPort  filled in with the allocated video port
    bool GetPerCallLocalPorts(pjsip_inv_session* inv,
                              uint16_t& outAudioPort,
                              uint16_t& outVideoPort) const;

    // ============================================================
    // INVITE Interception and External SDP Handling
    // ============================================================

    /**
     * @brief Get PJSIP module (for INVITE interception)
     */
    pjsip_module* GetModule() { return &module_; }

    /**
     * @brief Register conference validation callback (called before sending 200 OK)
     */
    using ConferenceValidationCallback = std::function<bool(const std::string& conferenceId)>;
    void setConferenceValidationCallback(ConferenceValidationCallback callback) {
        validationCallback_ = std::move(callback);
    }

    /**
     * @brief Register call established callback (INVITE CONFIRMED state)
     *
     * [V7.0 Phase-2 G] Critical contract: this callback is invoked BEFORE
     * 200 OK is sent to the peer. The implementation MUST return `true` only
     * when ALL backend setup is complete (transport created, CallSession
     * registered, conference member joined, ...). Returning `false` causes
     * the gateway to abort the call with a 5xx error response instead of
     * sending 200 OK, so the peer never starts sending RTP against a
     * half-initialized receiver.
     *
     * The previous (broken) contract triggered this callback AFTER sending
     * 200 OK, which caused the peer to begin transmitting RTP before any
     * receiver state existed - silently dropping the first keyframe and
     * many subsequent packets.
     */
    using CallEstablishedCallback = std::function<bool(pjsip_inv_session* inv, const RemoteMediaInfo& remoteInfo)>;
    void setCallEstablishedCallback(CallEstablishedCallback callback) {
        establishedCallback_ = std::move(callback);
    }

    /**
     * @brief Register SDP offer callback (for re-INVITE)
     */
    using SdpOfferCallback = std::function<pjmedia_sdp_session*(pj_pool_t* pool, pjsip_inv_session* inv, pjmedia_sdp_session* offer)>;
    void setSdpOfferCallback(SdpOfferCallback callback) {
        sdpOfferCallback_ = std::move(callback);
    }

    /**
     * @brief Parse remote media info from INVITE SDP
     */
    RemoteMediaInfo parseRemoteMediaInfo(pjsip_rx_data* rdata);

    /**
     * @brief Get current media configuration
     */
    void getMediaInfo(std::string& localIp, uint16_t& audioPort, uint16_t& videoPort) const {
        std::lock_guard<std::mutex> lock(mediaMutex_);
        localIp = localMediaAddress_;
        audioPort = localAudioPort_;
        videoPort = localVideoPort_;
    }

// PJSIP always enabled
    // ============== PJSIP Implementation ==============

protected:
    // SignalingHandler Implementation
    bool DoStart() override;
    void DoStop() override;
    bool DoSendSignaling(const std::string& msg) override;
    int DoInitiateCall(const std::string& callee, const Params& params) override;
    bool DoAcceptCall(int callId, const Params& params) override;
    bool DoRejectCall(int callId, const std::string& reason) override;
    bool DoTerminateCall(int callId, const std::string& reason) override;

private:
    // ISipSignalingCallback Implementation
    void OnRegistrationState(int accountId, uint32_t code, const std::string& reason) override;
    void OnRegistrationSuccess(int accountId) override;
    void OnRegistrationFailure(int accountId, uint32_t code, const std::string& reason) override;
    void OnIncomingCall(int accountId, int callId, const std::string& callerUri,
                        const std::string& callerDisplayName,
                        const std::string& sdpBody) override;
    void OnCallState(int callId, uint32_t state, const std::string& stateText) override;
    void OnCallMediaReady(int callId,
                          const std::string& remoteAddress,
                          int remotePort,
                          const std::string& audioCodecName = "",
                          uint8_t audioPayloadType = 0,
                          const std::string& videoCodecName = "",
                          uint8_t videoPayloadType = 0,
                          bool hasSecondaryVideo = false,
                          const std::string& secondaryVideoCodecName = "",
                          uint8_t secondaryVideoPayloadType = 0) override;
    void OnCallDisconnected(int callId, uint32_t code, const std::string& reason) override;
    void OnMediaStatistics(int callId, uint32_t bytesSent, uint32_t bytesReceived,
                           uint32_t packetsSent, uint32_t packetsReceived) override;

    // PJSIP initialization
    bool initPjsip();
    void cleanupPjsip();

    /**
     * @brief Whether the selected transport lets the protocol stack own RTP TX.
     *
     * The single classification point for media ownership, so a transport
     * mode never has to be encoded into the pjproject sources.
     */
    bool UsesStackMediaEngine() const;

    // INVITE interception module initialization (called by SipAdapter)
    bool initModule();
    void deinitModule();

    // Friend declaration
    friend class SipAdapter;

    // Call management
    SipCall* GetActiveCall(int callId);
    void RemoveActiveCall(int callId);

    /**
     * @brief 延迟销毁 RemoveActiveCall 摘下的呼叫对象
     *
     * RemoveActiveCall() 是从 SipCall::onCallState() 内部被调到的
     * （onCallState -> callback_->OnCallState -> TerminateSession ->
     * RemoveActiveCall）。此时该 SipCall 对象正处在自己的回调栈上，
     * 直接 erase 会析构掉正在执行的 this，回调返回后任何成员访问
     * （例如 onCallState 末尾的日志）都是 use-after-free。
     * 因此摘下的 unique_ptr 先寄存，等回到 pjsip 的入口回调（此时
     * 旧对象的栈帧已经展开完毕）再统一销毁。
     */
    void DrainRetiredCalls();

    // Get PJSIP Endpoint (from SipAdapter)
    pj::Endpoint& GetPjsipEndpoint() const;

    // INVITE interception handling
    static pj_bool_t on_rx_request(pjsip_rx_data* rdata);
    pj_status_t handleInvite(pjsip_rx_data* rdata);
    pjmedia_sdp_session* buildExternalSdp(pj_pool_t* pool);
    pjmedia_sdp_session* buildExternalSdp(pj_pool_t* pool,
                                          const std::string& localIp,
                                          uint16_t audioPort,
                                          uint16_t videoPort,
                                          const RemoteMediaInfo* remoteInfo = nullptr);
    pj_status_t create200OkWithSdp(pjsip_dialog* dlg, pjsip_rx_data* rdata,
                                    pjsip_inv_session* inv, pjmedia_sdp_session* sdp,
                                    pjsip_tx_data** p_tdata);

    // Contact URI this endpoint is reachable on for an established dialog.
    // Host mirrors the SDP c= address; port is the configured signaling port.
    std::string BuildLocalContactUri(pjsip_rx_data* rdata) const;

    // pjsip_dlg_create_response() never adds Contact (beautify uses
    // add_headers=PJ_FALSE), but a 2xx to INVITE must carry one.
    void AddContactHeader(pjsip_tx_data* tdata, pjsip_rx_data* rdata) const;
    pj_status_t handleReinvite(pjsip_inv_session* inv, pjsip_rx_data* rdata, pjmedia_sdp_session* offer);
    pj_status_t sendErrorResponse(pjsip_rx_data* rdata, int code, const char* reason);

    /**
     * @brief Update INVITE session's sessionId (called by onExternalCallEstablished)
     */
    void UpdateInviteSessionId(pjsip_inv_session* inv, const std::string& sessionId);

    // INVITE session state change member function
    void onInviteStateChanged(pjsip_inv_session* inv, int old_state, int new_state);


	vcs::Response SendDTMF(int callId, const std::string& digits);

    // INVITE interception module static instance pointer
    static void SetInviteModuleInstance(SipSignalingHandler* instance);
    static SipSignalingHandler* GetInviteModuleInstance();

    std::string localUri_;
    std::string server_;
    std::string username_;
    std::string password_;
    uint16_t localPort_;

    // SRTP configuration (set by application layer through ConfigureSrtp())
    vcs::media_config::SrtpConfig srtpConfig_;

    // Media transport type configuration (default uses SIP transport mode)
    MediaTransportType mediaTransportType_ = MediaTransportType::Sip;

    // Local media configuration (set by SipAdapter after creation, for external SDP generation)
    std::string localMediaAddress_;  // Set via SetLocalMediaAddress() (auto-detected or from config)
    uint16_t localAudioPort_ = 0;            // 0 = auto-detect, set via SetLocalMediaPorts()
    uint16_t localVideoPort_ = 0;            // 0 = auto-detect, set via SetLocalMediaPorts()
    uint16_t localSecondaryVideoPort_ = 0;   // 0 = auto-detect, set via SetLocalMediaPorts() [V7.0 Phase-2 D]
    mutable std::mutex mediaMutex_;    // Protects media configuration

    // [V7.1 Fix 2026-08-13] Per-call RTP port allocator.
    //
    // Until this fix, every inbound INVITE re-used the same global
    // localAudioPort_/localVideoPort_ (e.g. 4004/4006) and the
    // SimpleRtpTransport called bind() with SO_REUSEADDR=true. That made
    // the second terminal's audio/video sockets silently co-exist with
    // the first terminal's on the same port and the kernel demultiplexed
    // UDP datagrams between them in an undefined way (SSRC oscillation
    // in RTPEngine, second transport starved of packets).
    //
    // The allocator now hands out a fresh even/odd port pair to every
    // INVITE and reclaims it when the dialog is destroyed. The legacy
    // localAudioPort_/localVideoPort_ are still honoured as a fall-back
    // (when the pool is exhausted, or when an upstream caller disabled
    // pooling by setting the configured range to size 0).
    RtpPortAllocator portAllocator_;

    // Application-side SDP negotiator (mediaAppManaged). Null = pjsua builds SDP.
    std::shared_ptr<vcs::media_config::IMediaSdpNegotiator> sdpNegotiator_;

    // Per-call RTP ports taken from portAllocator_ (inbound and outbound),
    // keyed by callId. Zero values mean "not from the pool".
    // Guarded by callsMutex_.
    std::map<int, std::pair<uint16_t, uint16_t>> callPorts_;

    SipAdapter& sipAdapter_;     // SipAdapter reference (holds pj::Endpoint)
    std::unique_ptr<SipAccount> sipAccount_;
    pj::AccountConfig accountConfig_;
    std::map<int, std::unique_ptr<SipCall>> activeCalls_;

    // 已摘下但尚未销毁的呼叫（见 DrainRetiredCalls）。Guard by callsMutex_.
    std::vector<std::unique_ptr<SipCall>> retiredCalls_;

    std::mutex callsMutex_;

    bool pjsipInitialized_;

    // ============================================================
    // INVITE Interception Module (merged from SipExternalModule)
    // ============================================================
    pjsip_module module_;              // PJSIP module
    pj_str_t module_name_;            // Module name

    // Callback functions
    ConferenceValidationCallback validationCallback_;
    CallEstablishedCallback establishedCallback_;
    SdpOfferCallback sdpOfferCallback_;

    // INVITE session info (for BYE handling)
    struct InviteSessionInfo {
        std::string sessionId;      // sessionId in SessionManager (memberId)
        std::string conferenceId;   // Conference ID
        std::string callId;         // SIP Call-ID (for precise BYE matching)

        // [V7.1 Fix 2026-08-13] Per-call local RTP ports, freshly allocated
        // by the RtpPortAllocator so multiple terminals do NOT collide on
        // 4004/4006. Defaults to 0 = "use the static global ports" (legacy
        // single-tenant behaviour, kept for tests / fall-back).
        uint16_t    allocatedAudioPort = 0;
        uint16_t    allocatedVideoPort = 0;

        // [V7.4 Bookkeeping] True iff the per-call ports were taken from
        // the pool by this INVITE �� used to decide whether to Release()
        // them in DISCONNECTED. False means the ports came from the
        // global localAudioPort_/localVideoPort_ and should NOT be
        // returned to the pool.
        bool        portsAllocatedFromPool = false;

        // [V7.0 Phase-2 G] Bookkeeping for the "send 200 OK only after backend
        // setup" contract. `backendSetupDone` is set true after the
        // establishedCallback_ has been invoked; `backendSetupOk` records
        // its return value. handleInvite uses these to decide whether to
        // (a) send 200 OK (true && ok), (b) send 5xx error (true && !ok),
        // or (c) leave the entry in a "pending" state (false).
        bool backendSetupDone = false;
        bool backendSetupOk   = false;
    };

    // Track active invite sessions: inv* (as uintptr_t for safety) -> (sessionId, conferenceId, callId)
    // [V5.5 Fix] Use uintptr_t instead of raw pointer to avoid dangling pointer issues
    // PJSIP may destroy inv sessions internally, leaving dangling pointers in the map
    // [V7.0 Fix] Added callId field for proper BYE routing (V5.9 Critical Fix)
    std::map<uintptr_t, InviteSessionInfo> activeInvites_;
    mutable std::mutex invitesMutex_;  // Protects activeInvites_

    // [V7.0 Fix] Secondary index: SIP Call-ID -> inv session pointer
    // Enables O(1) lookup of invite session by Call-ID during BYE processing
    std::map<std::string, uintptr_t> callIdToInvite_;
    mutable std::mutex callIdMutex_;  // Protects callIdToInvite_

    // Store remote media info for each invite session
    std::map<uintptr_t, RemoteMediaInfo> remoteMediaInfos_;
    mutable std::mutex remoteInfoMutex_;  // Protects remoteMediaInfos_
};

}} // namespace vcs::gateway
