#include "SipSignalingHandler.h"
#include "SipAccount.h"
#include "SipCall.h"
#include "SipMediaTransport.h"
#include "SimpleRtpTransport.h"
#include "SipAdapter.h"
#include "ExternalSdpBuilder.h"
#include "SdpCodecUtil.h"
#include "SessionManager.h"
#include "ILogger.h"
#include <thread>       // [Diag 2026-08-12] for std::hash<std::thread::id> tracer
#include <cstring>      // [Diag 2026-08-12] for memcpy in method-name copy
#include <pjsip/sip_endpoint.h>
#include <pjsip/sip_util.h>
#include <pjsip/sip_msg.h>
#include <pjsip/sip_parser.h>
#include <pjsip/sip_auth.h>
#include <pjsip/sip_dialog.h>
#include <pjsip/sip_event.h>
#include <pjsip/sip_ua_layer.h>
#include <pjsua-lib/pjsua_internal.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>
#include <pjlib-util/resolver.h>
#include <pj/os.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <memory>

namespace vcs { namespace gateway {

// PJSIP always enabled

#define MODULE_NAME "mcu-signaling"

// Static instance pointer (for INVITE interception module callback)
static SipSignalingHandler* g_inviteModuleInstance = nullptr;

namespace {
// pjmedia（pjsua 信令路径）每通呼叫占 4 个 UDP 端口：音频 RTP/RTCP +
// 视频 RTP/RTCP。PJSUA_MAX_CALLS 是同一时刻能存在的 pjsua 呼叫上限，
// 乘 4 就是它可能占用的端口数；端口池必须从这段之后开始，否则转发
// INVITE 的 SimpleRtpTransport 会和 pjmedia 抢同一个端口。
constexpr uint16_t kPjsuaRtpPortRange = PJSUA_MAX_CALLS * 4;
// pjmedia 在 next_rtp_port == port + portRange 时仍会绑定（RTCP 再 +1），
// 超过后才回绕，所以真正的占用块比 portRange 多 2。
constexpr uint16_t kPjsuaRtpPortBlock = kPjsuaRtpPortRange + 2;
} // namespace

static void EnsurePjThreadRegistered() {
	if (!pj_thread_is_registered()) {
		// 必须是 thread_local 或 static，生命周期必须足够长
		thread_local pj_thread_desc desc;
		pj_thread_t* thread = nullptr;
		pj_status_t status = pj_thread_register("mcu_ext_thread", desc, &thread);
		if (status != PJ_SUCCESS) {
			VCS_GATEWAY_LOG_ERROR("[SIP] Failed to register external thread to PJSIP");
		}
		else {
			VCS_GATEWAY_LOG_INFO("[SIP] Successfully registered external thread to PJSIP");
		}
	}
}

SipSignalingHandler* GetCurrentInviteModule() {
    return g_inviteModuleInstance;
}

// ============== PJSIP Implementation ==============

// Forward declaration: Extract number from SIP URI
static std::string ExtractSipNumber(const std::string& sipUri);

// ============================================================================
// INVITE Interception Module Implementation
// ============================================================================

pj_bool_t SipSignalingHandler::on_rx_request(pjsip_rx_data* rdata) {
    // [Diag 2026-08-12] Track every request that reaches our module so we
    // can tell whether a stuck BYE is starving the worker thread pool.
    if (rdata && rdata->msg_info.msg && rdata->msg_info.msg->type == PJSIP_REQUEST_MSG) {
        const pjsip_method* m = &rdata->msg_info.msg->line.req.method;
        // Make a safe null-terminated copy of the method name so the
        // log formatter is happy even when the source is not terminated.
        char methodName[16] = {0};
        int n = m->name.slen < (int)sizeof(methodName) - 1
              ? m->name.slen : (int)sizeof(methodName) - 1;
        if (n > 0) memcpy(methodName, m->name.ptr, n);
        methodName[n] = '\0';
        VCS_GATEWAY_LOG_INFO(
            "[RX-TRACE][tid=%lu] on_rx_request ENTER method=%s",
            (unsigned long)std::hash<std::thread::id>{}(std::this_thread::get_id()),
            methodName);
    }

    SipSignalingHandler* handler = GetCurrentInviteModule();
    if (!handler) {
        return PJ_FALSE;
    }

    // Check if it's a request message
    if (rdata->msg_info.msg->type != PJSIP_REQUEST_MSG) {
        return PJ_FALSE;
    }

    const pjsip_method* method = &rdata->msg_info.msg->line.req.method;

    // [V5.5 Critical Fix] Proper BYE handling with session cleanup
    // Get INV session from dialog via mod_data[module_id] (set during INVITE creation)
    if (method->id == PJSIP_BYE_METHOD) {
        pjsip_dialog* byeDlg = nullptr;
        pjsip_inv_session* byeInv = nullptr;
        std::string sessionId;
        std::string conferenceId;

        // [V5.5 Fix] Try multiple methods to get the dialog and inv session
        int modId = handler->module_.id;

        // Method 1: Try pjsip_rdata_get_dlg() (works when module priority >= DIALOG)
        byeDlg = pjsip_rdata_get_dlg(rdata);
        if (byeDlg && modId >= 0) {
            byeInv = reinterpret_cast<pjsip_inv_session*>(byeDlg->mod_data[modId]);
        }

        // Method 2: If Method 1 failed, use Call-ID secondary index (O(1) lookup)
        // [V7.0 Fix] Properly track Call-ID to inv mapping
        std::string extractedCallId;
        if (!byeInv) {
            VCS_GATEWAY_LOG_INFO("[Signaling] BYE: pjsip_rdata_get_dlg returned NULL, trying Call-ID lookup");

            // Extract Call-ID from BYE request
            pjsip_cid_hdr* cid_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CALL_ID, nullptr);
            if (cid_hdr) {
                char callIdBuf[256];
                snprintf(callIdBuf, sizeof(callIdBuf), "%.*s",
                        (int)cid_hdr->id.slen, cid_hdr->id.ptr);
                extractedCallId = callIdBuf;

                // [V7.0 Fix] Use callIdToInvite_ secondary index for O(1) lookup
                std::lock_guard<std::mutex> cidLock(handler->callIdMutex_);
                auto cidIt = handler->callIdToInvite_.find(extractedCallId);
                if (cidIt != handler->callIdToInvite_.end()) {
                    uintptr_t invKey = cidIt->second;
                    // Look up full session info using the inv key
                    std::lock_guard<std::mutex> invLock(handler->invitesMutex_);
                    auto invIt = handler->activeInvites_.find(invKey);
                    if (invIt != handler->activeInvites_.end()) {
                        byeInv = reinterpret_cast<pjsip_inv_session*>(invKey);
                        sessionId = invIt->second.sessionId;
                        conferenceId = invIt->second.conferenceId;
                        VCS_GATEWAY_LOG_INFO("[Signaling] BYE: Call-ID matched inv=%p, sessionId='%s', confId='%s'",
                                           (void*)byeInv, sessionId.c_str(), conferenceId.c_str());
                    } else {
                        VCS_GATEWAY_LOG_WARNING("[Signaling] BYE: Call-ID '%s' found in index but inv session missing from activeInvites_",
                                             extractedCallId.c_str());
                    }
                } else {
                    VCS_GATEWAY_LOG_INFO("[Signaling] BYE: Call-ID '%s' not found in callIdToInvite_ (size=%zu)",
                                       extractedCallId.c_str(), handler->callIdToInvite_.size());
                }
            }
        }

        // Method 3: If still no byeInv, iterate through activeInvites_ and try to match
        if (!byeInv) {
            std::lock_guard<std::mutex> lock(handler->invitesMutex_);
            VCS_GATEWAY_LOG_INFO("[Signaling] BYE: Iterating %zu active invites to find match",
                               handler->activeInvites_.size());

            // For BYE, we can use any active invite since there's typically only one
            // per terminal. Just take the first one as the target.
            if (!handler->activeInvites_.empty()) {
                auto it = handler->activeInvites_.begin();
                byeInv = reinterpret_cast<pjsip_inv_session*>(it->first);
                sessionId = it->second.sessionId;
                conferenceId = it->second.conferenceId;
                VCS_GATEWAY_LOG_INFO("[Signaling] BYE: Using first active invite as target: sessionId=%s",
                                   sessionId.c_str());
            }
        }

        // Process the BYE if we found an inv session
        if (byeInv) {
            VCS_GATEWAY_LOG_INFO("[Signaling] BYE received: sessionId=%s, confId=%s",
                               sessionId.c_str(), conferenceId.c_str());

            // [Critical] Cleanup session - call HandleExternalBye to clean up conference member and RTP transport
            if (!sessionId.empty()) {
                VCS_GATEWAY_LOG_INFO("[Signaling] Calling HandleExternalBye for session cleanup");
                handler->sipAdapter_.HandleExternalBye(sessionId, conferenceId, "Terminal hung up");
            }

            // Remove from activeInvites_
            {
                std::lock_guard<std::mutex> lock(handler->invitesMutex_);
                handler->activeInvites_.erase(reinterpret_cast<uintptr_t>(byeInv));
            }

            // [V7.0 Fix] Remove from callIdToInvite_ index
            {
                std::lock_guard<std::mutex> lock(handler->callIdMutex_);
                for (auto it = handler->callIdToInvite_.begin(); it != handler->callIdToInvite_.end(); ) {
                    if (it->second == reinterpret_cast<uintptr_t>(byeInv)) {
                        VCS_GATEWAY_LOG_INFO("[Signaling] Removed callIdToInvite_['%s'] for inv=%p",
                                           it->first.c_str(), (void*)byeInv);
                        it = handler->callIdToInvite_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // Remove from remoteMediaInfos_
            {
                std::lock_guard<std::mutex> lock(handler->remoteInfoMutex_);
                handler->remoteMediaInfos_.erase(reinterpret_cast<uintptr_t>(byeInv));
            }

            // Clear mod_data to prevent double cleanup
            if (byeDlg && modId >= 0) {
                byeDlg->mod_data[modId] = nullptr;
            }
        } else {
            VCS_GATEWAY_LOG_WARNING("[Signaling] BYE received but no matching invite session found");
        }

        // Return PJ_FALSE to let PJSIP send 200 OK response
        return PJ_FALSE;
    }

    // Only handle INVITE requests
    if (method->id != PJSIP_INVITE_METHOD) {
        return PJ_FALSE;
    }

    // Check target URI
    pjsip_uri* uri = rdata->msg_info.msg->line.req.uri;
    if (!uri) {
        return PJ_FALSE;
    }

    // Get generic URI using pjsip_uri_get_uri, then check type with scheme
    void* generic_uri = pjsip_uri_get_uri(uri);
    pjsip_sip_uri* req_uri = (pjsip_sip_uri*)generic_uri;
    if (!req_uri) {
        // Non-SIP URI, let other modules handle
        return PJ_FALSE;
    }

    // Check if it's sip: or sips: URI via scheme
    const pj_str_t* scheme = pjsip_uri_get_scheme(uri);
    if (!scheme || (pj_strcmp2(scheme, "sip") != 0 && pj_strcmp2(scheme, "sips") != 0)) {
        // Non-SIP URI, let other modules handle
        return PJ_FALSE;
    }

    // Check if destination is local bound port (external module listens on port 5060)
    VCS_GATEWAY_LOG_INFO("[Signaling] Received INVITE: uri=%.*s:%d",
                         (int)req_uri->host.slen, req_uri->host.ptr, req_uri->port);

    VCS_GATEWAY_LOG_INFO("[Signaling] Media configured: %s:%d:%d",
                         handler->localMediaAddress_.c_str(), handler->localAudioPort_, handler->localVideoPort_);

    // [Critical Fix] Validate conference before handleInvite
    // Parse conferenceId
    RemoteMediaInfo remoteInfo = handler->parseRemoteMediaInfo(rdata);
    std::string conferenceIdToValidate = remoteInfo.conferenceId.empty() ? "local" : remoteInfo.conferenceId;

    // Validate conference
    auto validationCallback = handler->validationCallback_;
    if (validationCallback && !validationCallback(conferenceIdToValidate)) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Conference validation FAILED: %s - rejecting INVITE", conferenceIdToValidate.c_str());
        // [Critical] Return 403 status code so PJSIP automatically sends response
        return 403;
    }
    VCS_GATEWAY_LOG_INFO("[Signaling] Conference validation PASSED: %s", conferenceIdToValidate.c_str());

    // After validation passes, process completely
    pj_status_t status = handler->handleInvite(rdata);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] handleInvite failed: %d", status);
        // handleInvite handles error response internally
    }

    // Return TRUE to indicate request has been handled (no longer passed to other modules including PJSUA)
    return PJ_TRUE;
}

SipSignalingHandler::SipSignalingHandler(
    const std::string& localUri,
    uint16_t localPort,
    SipAdapter& adapter)
    : SignalingHandler("SIP", localUri)
    , localUri_(localUri)
    , localPort_(localPort)
    , sipAdapter_(adapter)
    , pjsipInitialized_(false)
    , module_()
    , module_name_(pj_str((char*)MODULE_NAME))
{
}

SipSignalingHandler::~SipSignalingHandler() {
    deinitModule();
    DoStop();
}

// Static method implementation
void SipSignalingHandler::SetInviteModuleInstance(SipSignalingHandler* instance) {
    g_inviteModuleInstance = instance;
}

SipSignalingHandler* SipSignalingHandler::GetInviteModuleInstance() {
    return g_inviteModuleInstance;
}

pj::Endpoint& SipSignalingHandler::GetPjsipEndpoint() const {
    return sipAdapter_.GetPjsipEndpointRef();
}

void SipSignalingHandler::SetServer(
    const std::string& server,
    const std::string& username,
    const std::string& password)
{
    server_ = server;
    username_ = username;
    password_ = password;
}

void SipSignalingHandler::SetSrtpConfig(const vcs::media_config::SrtpConfig& config) {
    srtpConfig_ = config;
    VCS_GATEWAY_LOG_INFO("[SIP] SRTP config stored, will be applied to account: mode=%d",
                        static_cast<int>(config.mode));
}

bool SipSignalingHandler::DoStart() {
    return initPjsip();
}

void SipSignalingHandler::DoStop() {
    std::lock_guard<std::mutex> lock(callsMutex_);
    activeCalls_.clear();

    // ============================================================
    // Important Fix: Do NOT destroy account!
    // Account needs to remain to handle subsequent inbound INVITE requests
    // If account is destroyed, PJSIP will reject inbound calls:
    // "No available account to handle Request msg INVITE"
    // ============================================================
    // sipAccount_.reset();  // Commented out, do NOT destroy account in DoStop

    // Only cleanup calls state, keep account
    // PJSIP Endpoint is managed by SipAdapter, no cleanup needed here
}

// ===================================================================
// Local Media Configuration (for external SDP generation)
// ===================================================================

void SipSignalingHandler::SetLocalMediaAddress(const std::string& address) {
    localMediaAddress_ = address;
    VCS_GATEWAY_LOG_INFO("[SIP] Local media address set: %s", address.c_str());
}

void SipSignalingHandler::SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort) {
    // [V7.0 Phase-2 D] Keep the 2-arg overload for backward compatibility
    // with legacy callers. The 3-arg overload below is preferred.
    localAudioPort_ = audioPort;
    localVideoPort_ = videoPort;
    VCS_GATEWAY_LOG_INFO("[SIP] Local media ports set: audio=%u, video=%u (secondary video port unset)", audioPort, videoPort);
    if (pjsipInitialized_) {
        // pjmedia 的 RTP 起点在账户创建时就固定了，端口池的范围也一样；
        // 运行期再改这里只会影响"兜底值"，不会移动任何真实 socket。
        VCS_GATEWAY_LOG_WARNING("[SIP] Local media ports changed after PJSIP init; "
                                "pjmedia RTP base and the per-call pool keep their "
                                "startup values");
    }
}

void SipSignalingHandler::SetLocalMediaPorts(uint16_t audioPort,
                                            uint16_t videoPort,
                                            uint16_t secondaryVideoPort) {
    localAudioPort_ = audioPort;
    localVideoPort_ = videoPort;
    localSecondaryVideoPort_ = secondaryVideoPort;
    VCS_GATEWAY_LOG_INFO("[SIP] Local media ports set: audio=%u, video=%u, secondaryVideo=%u",
                         audioPort, videoPort, secondaryVideoPort);
}

bool SipSignalingHandler::DoSendSignaling(const std::string& msg) {
    (void)msg;
    // PJSIP implementation doesn't need this method, calls handled by DoInitiateCall etc.
    return true;
}

bool SipSignalingHandler::UsesStackMediaEngine() const {
    switch (mediaTransportType_) {
    case MediaTransportType::Sip:
    case MediaTransportType::H323:
    case MediaTransportType::Webrtc:
    case MediaTransportType::Custom:
        // 已实现的传输全部由应用侧发 RTP：Custom 走 SimpleRtpTransport 独立
        // 套接字，Sip/H323/Webrtc 走 SipMediaTransport 的 pjmedia 套接字
        // 透传，编解码与混音都在 MCU。新增"协议栈自持媒体"模式时在此登记为
        // true，不需要改 pjproject 源码。
        return false;
    }
    return false;
}

int SipSignalingHandler::DoInitiateCall(const std::string& callee, const Params& params) {
    if (!pjsipInitialized_ || !sipAccount_) {
        VCS_GATEWAY_LOG_ERROR("[SIP] DoInitiateCall: stack not ready (initialized=%d, account=%p)",
                              static_cast<int>(pjsipInitialized_), (void*)sipAccount_.get());
        return -1;
    }

    try {
		EnsurePjThreadRegistered();

        // Create outbound call
        auto call = std::make_unique<SipCall>(*sipAccount_, this);

        // mediaAppManaged 下 pjsua 不创建媒体通道，出呼的 RTP 由应用侧
        // SimpleRtpTransport 承担（SipAdapter::OnCallConnected）。因此这里
        // 按呼叫分配一对端口，既写进 offer，也用于后续绑定。
        uint16_t audioPort = localAudioPort_;
        uint16_t videoPort = localVideoPort_;
        uint16_t allocatedAudioPort = 0;
        uint16_t allocatedVideoPort = 0;
        const bool portsFromPool = portAllocator_.Allocate(allocatedAudioPort, allocatedVideoPort);
        if (portsFromPool) {
            audioPort = allocatedAudioPort;
            videoPort = allocatedVideoPort;
        }

        // Set local media configuration (read from config file, set via SetLocalMediaAddress/SetLocalMediaPorts)
        call->SetLocalIpAddress(localMediaAddress_);
        call->SetLocalMediaPorts(audioPort, videoPort);
        call->SetLocalSignalingPort(localPort_);
        // 应用侧自持 RTP：pjsua 只保留信令，pjmedia 不得再发一路媒体。
        call->SetExternalMediaPlane(!UsesStackMediaEngine());
        // 应用侧 SDP：出呼 offer / 收到 re-offer 的 answer 都由它生成。
        if (sdpNegotiator_) {
            call->SetMediaSdpNegotiator(sdpNegotiator_);
        }

        std::string target = callee;
        if (target.find("sip:") == std::string::npos) {
            target = "sip:" + target;
        }

        // Get audio/video parameters from params
        int audioCount = 1;
        int videoCount = 1;
        auto videoEnabled = params.GetBool("video_enabled");
        if (videoEnabled.has_value() && videoEnabled.value()) {
            videoCount = 1;
        }

        if (call->MakeCall(target, audioCount, videoCount)) {
			int callId = call->getId();
            std::lock_guard<std::mutex> lock(callsMutex_);
            activeCalls_[callId] = std::move(call);
            callPorts_[callId] = std::make_pair(
                portsFromPool ? allocatedAudioPort : 0,
                portsFromPool ? allocatedVideoPort : 0);
            return callId;
        }

        if (portsFromPool) {
            portAllocator_.Release(allocatedAudioPort, allocatedVideoPort);
        }
        VCS_GATEWAY_LOG_ERROR("[SIP] DoInitiateCall: MakeCall('%s') returned false", target.c_str());
        return -1;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to initiate call: %s", err.info().c_str());
        NotifySignalingError(0, static_cast<int>(err.status), err.reason);
        return -1;
    }
}

bool SipSignalingHandler::DoAcceptCall(int callId, const Params& params) {
	EnsurePjThreadRegistered();
    SipCall* call = GetActiveCall(callId);
    if (!call) {
        return false;
    }

    try {
        return call->Answer();
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to accept call: %s", err.info().c_str());
        return false;
    }
}

bool SipSignalingHandler::DoRejectCall(int callId, const std::string& reason) {
	EnsurePjThreadRegistered();
    SipCall* call = GetActiveCall(callId);
    if (!call) {
        return false;
    }

    try {
        return call->Reject();
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to reject call: %s", err.info().c_str());
        return false;
    }
}

bool SipSignalingHandler::DoTerminateCall(int callId, const std::string& reason) {
	EnsurePjThreadRegistered();
    SipCall* call = GetActiveCall(callId);
    if (!call) {
        return false;
    }

    try {
        return call->Hangup();
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to terminate call: %s", err.info().c_str());
        return false;
    }
}

// ============== ISipSignalingCallback Implementation ==============

void SipSignalingHandler::OnRegistrationState(int accountId, uint32_t code, const std::string& reason) {
    (void)accountId;
    VCS_GATEWAY_LOG_INFO("[SipSignalingHandler] Registration state: code=%u, reason=%s",
                         code, reason.c_str());
}

void SipSignalingHandler::OnRegistrationSuccess(int accountId) {
    (void)accountId;
    VCS_GATEWAY_LOG_INFO("[SipSignalingHandler] Registration successful");
}

void SipSignalingHandler::OnRegistrationFailure(int accountId, uint32_t code, const std::string& reason) {
    (void)accountId;
    VCS_GATEWAY_LOG_ERROR("[SipSignalingHandler] Registration failed: code=%u, reason=%s",
                          code, reason.c_str());
}

void SipSignalingHandler::OnIncomingCall(int accountId, int callId,
                                         const std::string& callerUri,
                                         const std::string& callerDisplayName,
                                         const std::string& sdpBody) {
    (void)accountId;

    VCS_GATEWAY_LOG_INFO("Incoming call: callId=%d, caller=%s, display=%s",
                        callId, callerUri.c_str(), callerDisplayName.c_str());

    // Create SipCall object for inbound call and add to activeCalls_
    std::unique_ptr<SipCall> incomingCall(new SipCall(*sipAccount_, callId, this));

    // mediaAppManaged 下 pjsua 不创建媒体通道，入呼的 RTP 由应用侧传输承担，
    // 因此每通呼叫从端口池分配一对端口：既写进 200 OK 的 SDP，也用于绑定。
    uint16_t audioPort = localAudioPort_;
    uint16_t videoPort = localVideoPort_;
    uint16_t allocatedAudioPort = 0;
    uint16_t allocatedVideoPort = 0;
    const bool portsFromPool = portAllocator_.Allocate(allocatedAudioPort, allocatedVideoPort);
    if (portsFromPool) {
        audioPort = allocatedAudioPort;
        videoPort = allocatedVideoPort;
    }

    // Set local media configuration (read from config file, set via SetLocalMediaAddress/SetLocalMediaPorts)
    incomingCall->SetLocalIpAddress(localMediaAddress_);
    incomingCall->SetLocalMediaPorts(audioPort, videoPort);
    incomingCall->SetLocalSignalingPort(localPort_);
    // 应用侧自持 RTP：pjsua 只保留信令，pjmedia 不得再发一路媒体。
    incomingCall->SetExternalMediaPlane(!UsesStackMediaEngine());
    if (sdpNegotiator_) {
        incomingCall->SetMediaSdpNegotiator(sdpNegotiator_);
    }

    // 对端 offer 由 SipAccount 从 INVITE 里取出（pjsua2 已清空
    // incoming_data）；后续 SipCall::Answer 用它生成 answer。
    incomingCall->SetIncomingOfferSdp(sdpBody);

    // Add to activeCalls_
    {
        std::lock_guard<std::mutex> lock(callsMutex_);
        activeCalls_[callId] = std::move(incomingCall);
        if (portsFromPool) {
            callPorts_[callId] = std::make_pair(allocatedAudioPort, allocatedVideoPort);
        }
    }

    // [V7.0 Phase-2 A] Extract the conference id BEFORE creating the session.
    //
    // Reason: CreateSession now accepts an explicit confId and uses it to
    // resolve the per-conference SessionManager (V5.9 design Sec.8.4 / Sec.9.1).
    // If we created the session first (in the default SM) and then wrote
    // the confId onto the CallSession afterwards, the session would be
    // stranded in the wrong SM, causing every later per-conference lookup
    // (RemoveSession, BindTransport, ...) to miss it.
    //
    // The confId is currently derived from the callee number of the local
    // URI (same heuristic used historically, see line 521 below). If the
    // SipCall has not yet been created above, this block returns an empty
    // confId and CreateSession falls back to defaultConferenceId_.
    std::string confId;
    {
        SipCall* call = GetActiveCall(callId);
        if (call) {
            const SipCallInfo& info = call->GetInfo();
            std::string callee = ExtractSipNumber(info.localUri);
            if (!callee.empty()) {
                confId = callee;
                VCS_GATEWAY_LOG_DEBUG(
                    "Pre-resolved conferenceId from localUri: %s for call %d",
                    callee.c_str(), callId);
            }
        }
    }

    // Create session (V7.0: confId forwarded so the new session lands in
    // the right per-conference SM).
    CallSession* session = CreateSession(callId, callerUri, confId);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("Failed to create session for call %d", callId);
        std::lock_guard<std::mutex> lock(callsMutex_);
        auto it = callPorts_.find(callId);
        if (it != callPorts_.end()) {
            portAllocator_.Release(it->second.first, it->second.second);
            callPorts_.erase(it);
        }
        activeCalls_.erase(callId);
        return;
    }

    // [V5.7] v2 architecture: mark this session as coming from the SIP protocol
    session->protocol = ProtocolType::SIP;

    // [V7.0 Phase-2 A] Cache the resolved confId on the session so downstream
    // code (NotifyCallIncoming, conference layer, ...) sees the same value
    // that was used to pick the per-conference SM.
    if (!confId.empty()) {
        session->conferenceId = confId;
    }

    // ============================================================
    // Critical Fix: Extract remote address from callerUri
    // callerUri format may be <sip:172.16.40.247> or sip:172.16.40.247:5060
    //
    // Important: Port in SIP URI is SIP signaling port, not RTP media port!
    // RTP port must be obtained from SDP m=audio XXX line (e.g. Yealink's 50094)
    // So only extract IP address here, port uses 5060 (SIP port) or 0 (obtained from SDP later)
    // ============================================================
    std::string remoteIp;

    // Try to parse IP address from URI
    std::string uriToParse = callerUri;

    // Remove possible <> wrapping
    size_t startPos = uriToParse.find('<');
    if (startPos != std::string::npos) {
        size_t endPos = uriToParse.find('>');
        if (endPos != std::string::npos) {
            uriToParse = uriToParse.substr(startPos + 1, endPos - startPos - 1);
        }
    }

    // Check if URI contains explicit port (sip:IP:Port format)
    // If port exists, it's a complete SIP URI; if not, only contains IP
    size_t colonPos = uriToParse.rfind(':');
    bool hasExplicitPort = false;

    if (colonPos != std::string::npos) {
        // Check if characters after colon are all digits (port)
        std::string potentialPort = uriToParse.substr(colonPos + 1);
        bool allDigits = !potentialPort.empty();
        for (char c : potentialPort) {
            if (!std::isdigit(c)) {
                allDigits = false;
                break;
            }
        }

        if (allDigits && potentialPort.length() <= 5) {
            // It's indeed a port (not part of IP address)
            hasExplicitPort = true;
        }
    }

    // Extract IP address (skip sip: prefix)
    size_t ipStart = uriToParse.find("sip:");
    if (ipStart != std::string::npos) {
        ipStart += 4;  // Skip "sip:"
    } else {
        ipStart = 0;
    }

    // Find IP end position
    size_t ipEnd = uriToParse.length();
    if (hasExplicitPort) {
        ipEnd = colonPos;  // IP ends at port colon
    }

    remoteIp = uriToParse.substr(ipStart, ipEnd - ipStart);

    if (!remoteIp.empty()) {
        // [V7.0 Phase-2 F] Seed the remote address into the Audio transport
        // slot through SessionManager's single write entry. The RTP port is
        // not known yet (it comes from SDP), so this is an address-only
        // update: it never clears an already registered transport object.
        if (auto* sm = GetSessionManager()) {
            sm->AttachTransport(session->sessionId, MediaStreamKind::Audio,
                                nullptr, remoteIp, 0);
        }
        // Note: remotePort not set here because RTP port needs to be obtained from SDP
        // Port in SIP URI is 5060, but RTP port is in SDP m=audio line (e.g. 50094)
        VCS_GATEWAY_LOG_INFO("Extracted remote IP from URI: %s (RTP port will be from SDP)", remoteIp.c_str());
    }
    // ============================================================

    // Notify upper layer of inbound call
    vcs::Params callParams;
    callParams.SetString("caller_display_name", callerDisplayName);
    callParams.SetString("callee", session->callee);
    callParams.SetString("conference_id", session->conferenceId);
    NotifyCallIncoming(callId, callerUri, callParams);

    // Auto-accept inbound call
    VCS_GATEWAY_LOG_DEBUG("Auto-accepting call %d", callId);
    AcceptCall(callId, Params());
}

void SipSignalingHandler::OnCallState(int callId, uint32_t state, const std::string& stateText) {
    VCS_GATEWAY_LOG_INFO("Call state: callId=%d, state=%s (code=%u)",
                        callId, stateText.c_str(), state);

	VCS_GATEWAY_LOG_INFO("[DIAG-3] SipSignalingHandler::OnCallState ENTER: callId=%d, state=%u, stateText=%s",
		callId, state, stateText.c_str());

    switch (state) {
        case PJSIP_INV_STATE_CALLING:
            UpdateSessionState(callId, SessionState::Connecting);
            break;
        case PJSIP_INV_STATE_EARLY:
            UpdateSessionState(callId, SessionState::Connecting);
            break;
        case PJSIP_INV_STATE_CONNECTING:
            UpdateSessionState(callId, SessionState::Connecting);
            break;
        case PJSIP_INV_STATE_CONFIRMED:
            VCS_GATEWAY_LOG_INFO("Call confirmed: callId=%d", callId);
            // UpdateSessionState 在 Active 迁移内已 NotifyCallConnected；
            // 此处再通知一次会让 OnCallConnected 同步跑两遍：第二遍新建一套
            // 传输并用 SetTransport 覆盖会话登记。事件分发线程若在两遍之间
            // 取到传输快照，媒体就握住被替换掉的旧对象，整通 SendRtp 报
            // pjTransport=0（实测 1/3 轮）。契约是每通一次。
            UpdateSessionState(callId, SessionState::Active);
            break;
        case PJSIP_INV_STATE_DISCONNECTED:
            VCS_GATEWAY_LOG_INFO("Call disconnected: callId=%d, reason=%s", callId, stateText.c_str());
            // pj 安全停点在 TerminateSession → SM.MarkTerminated 内完成：
            // 拆传输并在本回调栈内 StopReceiving()，无需额外的媒体停止调用。
            // 媒体停止由 conference 在 CallTerminated 事件后执行（docs/08 §2.4）。
            // Teardown + CallTerminated publish via SM (single publish point).
            TerminateSession(callId, "Remote terminated");
            RemoveActiveCall(callId);
            break;
        default:
            break;
    }
}

void SipSignalingHandler::OnCallMediaReady(int callId,
                                           const std::string& remoteAddress,
                                           int remotePort,
                                           const std::string& audioCodecName,
                                           uint8_t audioPayloadType,
                                           const std::string& videoCodecName,
                                           uint8_t videoPayloadType,
                                           bool hasSecondaryVideo,
                                           const std::string& secondaryVideoCodecName,
                                           uint8_t secondaryVideoPayloadType) {
    VCS_GATEWAY_LOG_INFO("==================================================");
    VCS_GATEWAY_LOG_INFO("Media ready: callId=%d, remote=%s:%d, audio=%s(pt=%u), video=%s(pt=%u), transport_type=%d",
                        callId, remoteAddress.c_str(), remotePort,
                        audioCodecName.c_str(), audioPayloadType,
                        videoCodecName.c_str(), videoPayloadType,
                        static_cast<int>(mediaTransportType_));
    if (hasSecondaryVideo) {
        VCS_GATEWAY_LOG_INFO("Secondary video: %s(pt=%u)",
                            secondaryVideoCodecName.c_str(), secondaryVideoPayloadType);
    }
    VCS_GATEWAY_LOG_INFO("==================================================");

    // Get session
    auto session = GetSession(callId);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("Session not found for callId=%d", callId);
        NotifyMediaReady(callId, remoteAddress, remotePort);
        return;
    }

    // Get Call object
    SipCall* call = GetActiveCall(callId);
    if (!call) {
        VCS_GATEWAY_LOG_ERROR("Call not found for callId=%d", callId);
        NotifyMediaReady(callId, remoteAddress, remotePort);
        return;
    }

    // ============================================================
    // [V7.0 Phase-2 E] Persist the secondary video stream's remote
    // address/port/PT into `remoteMediaInfos_` so the transport layer
    // can build a dedicated SimpleRtpTransport for the auxiliary
    // (content:slides) stream later on. This fixes the long-standing
    // bug where audio RTP was sent to the terminal's main video port.
    // ============================================================
    if (hasSecondaryVideo) {
        pjsip_inv_session* invKey = call->GetInvSession();
        if (invKey) {
            std::string secondaryVideoAddr;
            int secondaryVideoPort = 0;
            uint8_t secondaryVideoPtLocal = 0;
            const bool found = call->GetSecondaryVideoAddress(
                secondaryVideoAddr, secondaryVideoPort, secondaryVideoPtLocal);
            if (found && !secondaryVideoAddr.empty() && secondaryVideoPort > 0) {
                std::lock_guard<std::mutex> lock(remoteInfoMutex_);
                auto infoIt = remoteMediaInfos_.find(reinterpret_cast<uintptr_t>(invKey));
                if (infoIt != remoteMediaInfos_.end()) {
                    infoIt->second.secondaryVideoPort =
                        static_cast<uint16_t>(secondaryVideoPort);
                    infoIt->second.secondaryVideoPayloadType =
                        secondaryVideoPayloadType > 0
                            ? secondaryVideoPayloadType
                            : secondaryVideoPtLocal;
                    if (infoIt->second.address.empty()) {
                        infoIt->second.address = secondaryVideoAddr;
                    }
                    VCS_GATEWAY_LOG_INFO(
                        "[V7.0] Saved secondary video into RemoteMediaInfo: "
                        "inv=%p, addr=%s, port=%u, pt=%u",
                        (void*)invKey, secondaryVideoAddr.c_str(),
                        infoIt->second.secondaryVideoPort,
                        infoIt->second.secondaryVideoPayloadType);
                } else {
                    // No RemoteMediaInfo entry yet - create a new one
                    // seeded with the address from the call.
                    RemoteMediaInfo info;
                    info.address = secondaryVideoAddr;
                    info.secondaryVideoPort =
                        static_cast<uint16_t>(secondaryVideoPort);
                    info.secondaryVideoPayloadType =
                        secondaryVideoPayloadType > 0
                            ? secondaryVideoPayloadType
                            : secondaryVideoPtLocal;
                    remoteMediaInfos_[reinterpret_cast<uintptr_t>(invKey)] = info;
                    VCS_GATEWAY_LOG_INFO(
                        "[V7.0] Created new RemoteMediaInfo with secondary video: "
                        "inv=%p, addr=%s, port=%u, pt=%u",
                        (void*)invKey, secondaryVideoAddr.c_str(),
                        info.secondaryVideoPort,
                        info.secondaryVideoPayloadType);
                }
            } else {
                VCS_GATEWAY_LOG_WARNING(
                    "[V7.0] hasSecondaryVideo=true but SipCall could not "
                    "extract secondary video address/port (addr='%s', port=%d)",
                    secondaryVideoAddr.c_str(), secondaryVideoPort);
            }
        } else {
            VCS_GATEWAY_LOG_WARNING(
                "[V7.0] hasSecondaryVideo=true but inv session is NULL for callId=%d",
                callId);
        }
    }

    // Determine remote address and port
    std::string actualRemoteAddr = remoteAddress;
    int actualRemotePort = remotePort;

    // [V7.0 Phase-2 F] Read the primary transport's remoteAddress as a
    // fallback when the caller did not pass one explicitly.
    if (!session->GetPrimaryRemoteAddress().empty() && actualRemoteAddr.empty()) {
        actualRemoteAddr = session->GetPrimaryRemoteAddress();
    }

    std::shared_ptr<IRtpTransport> transport;

    // Pre-declare variables needed for Custom mode (avoid jumping over initialization between cases)
    std::string audioAddr, videoAddr;
    int audioPort = 0, videoPort = 0;
    std::string primaryAddr;
    int primaryPort = 0;

    // ============================================================
    // Select transport method based on media transport type
    // ============================================================
    switch (mediaTransportType_) {
        case MediaTransportType::Custom:
            {
                // ============================================================
                // [Custom Transport Mode] Use independent UDP Socket
                //
                // Reference: V4.16 Chapter 2 Section 2.3.5
                //
                // Architecture comparison:
                // [Current Complex Architecture] PJSIP transport injection (not working)
                //   Yealink <--RTP--> PJSIP transport --rtp_cb--> MCU
                //
                // [Simplified Architecture] Independent Socket (recommended)
                //   Yealink <--UDP Socket--> SimpleRtpTransport <--> MCU
                //
                // Flow:
                // 1. PJSIP completes SDP negotiation, gets remote audio and video RTP address and port
                // 2. Create SimpleRtpTransport, bind local port
                // 3. Add remote destinations for audio and video separately
                // 4. Use independent socket to send RTP to remote (distinguish by payload type)
                // 5. Use independent socket to receive remote RTP
                // 6. Distribute RTP packets to Media module via callback
                // ============================================================
                VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Creating SimpleRtpTransport for callId=%d", callId);

                // Get audio and video remote addresses from SipCall
                call->GetAllMediaAddresses(audioAddr, audioPort, videoAddr, videoPort);

                VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Audio remote: %s:%d", audioAddr.c_str(), audioPort);
                VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Video remote: %s:%d", videoAddr.c_str(), videoPort);

                // Determine primary address (for backward compatibility)
                primaryAddr = !videoAddr.empty() ? videoAddr :
                                  (!audioAddr.empty() ? audioAddr : actualRemoteAddr);
                primaryPort = (videoPort > 0) ? videoPort :
                              (audioPort > 0) ? audioPort : actualRemotePort;

                if (primaryAddr.empty()) {
                    primaryAddr = "172.16.40.247";  // Default address
                }
                if (primaryPort == 0) {
                    primaryPort = 50094;
                }

                VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Primary remote: %s:%d", primaryAddr.c_str(), primaryPort);

                // Create SimpleRtpTransport
                SimpleRtpConfig config;
                config.localAddress = "0.0.0.0";  // Listen on all addresses
                config.localPort = 0;               // Auto-assign port
                config.ssrc = 0;                    // Obtained from SDP negotiation
                config.defaultPayloadType = (videoPayloadType > 0) ? videoPayloadType : 97;
                config.enableRtcp = false;

                transport = std::make_shared<SimpleRtpTransport>(
                    session->sessionId,
                    config
                );

                // Open transport (create UDP socket)
                auto simpleTransport = std::dynamic_pointer_cast<SimpleRtpTransport>(transport);
                if (simpleTransport) {
                    if (!simpleTransport->Open()) {
                        VCS_GATEWAY_LOG_ERROR("[CUSTOM-TRANSPORT] Failed to open SimpleRtpTransport");
                        NotifyMediaReady(callId, primaryAddr, primaryPort);
                        return;
                    }

                    // Get locally assigned port
                    uint16_t localPort = simpleTransport->GetLocalPort();
                    VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Local port allocated: %d", localPort);

                    // Set primary remote address (default address)
                    simpleTransport->SetRemoteAddress(primaryAddr, static_cast<uint16_t>(primaryPort));

                    // Add audio destination
                    if (!audioAddr.empty() && audioPort > 0) {
                        RtpDestination audioDest;
                        audioDest.address = audioAddr;
                        audioDest.port = static_cast<uint16_t>(audioPort);
                        audioDest.payloadType = audioPayloadType > 0 ? audioPayloadType : 0;  // PCMU=0, PCMA=8
                        simpleTransport->AddDestination(audioDest);
                        VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Audio destination added: %s:%d (pt=%u)",
                                           audioAddr.c_str(), audioPort, audioDest.payloadType);
                    }

                    // Add video destination
                    if (!videoAddr.empty() && videoPort > 0) {
                        RtpDestination videoDest;
                        videoDest.address = videoAddr;
                        videoDest.port = static_cast<uint16_t>(videoPort);
                        videoDest.payloadType = videoPayloadType > 0 ? videoPayloadType : 97;  // H264=97
                        simpleTransport->AddDestination(videoDest);
                        VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Video destination added: %s:%d (pt=%u)",
                                           videoAddr.c_str(), videoPort, videoDest.payloadType);
                    }

                    // [V5.5 Fix] Do NOT call StartReceiving() here
                    // Callback needs to be set in MediaControl::AddMemberStream
                    // Correct initialization order: Open() -> config address/dest -> NotifyMediaReady -> AddMemberStream -> SetCallback -> StartReceiving
                    VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Transport configured, waiting for AddMemberStream to set callback and start receiving");

                    // Update session info
                    // [V7.0 Phase-2 F] Address endpoint only; the transport
                    // object itself is registered below via the single write
                    // entry.
                    if (auto* sm = GetSessionManager()) {
                        sm->AttachTransport(session->sessionId,
                                            MediaStreamKind::Audio,
                                            nullptr, primaryAddr,
                                            static_cast<uint16_t>(primaryPort));
                    }

                    // Print all destinations
                    VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] SimpleRtpTransport created: sessionId=%s", session->sessionId.c_str());
                    VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT]   Local: %s:%d", simpleTransport->GetLocalAddress().c_str(), localPort);
                    VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT]   Remote: %s:%d", primaryAddr.c_str(), primaryPort);
                    for (const auto& dest : simpleTransport->GetDestinations()) {
                        VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT]   Dest: %s:%d (pt=%u)",
                                           dest.address.c_str(), dest.port, dest.payloadType);
                    }
                }
            }
            break;

        case MediaTransportType::Sip:
        case MediaTransportType::H323:
        case MediaTransportType::Webrtc:
        default:
            {
                // ============================================================
                // [Sip Transport Mode] Use PJSIP transport (RTP passthrough mode)
                //
                // PJSIP only does:
                // 1. Provide RTP socket and remote address
                // 2. Receive RTP packets (via call.getAudioMedia, etc.)
                // 3. Send RTP packets (via pjmedia_transport_send_rtp)
                //
                // Media module does:
                // 1. Encode/decode (AudioProcessor, VideoProcessor)
                // 2. Mix audio (AudioProcessor)
                // 3. Compose video (VideoProcessor)
                //
                // Media flow:
                //   Yealink <--RTP--> PJSIP transport --rtp_cb--> Media module (encode/decode+mix)
                //                <-- MCU ---> send_rtp --> Yealink
                // ============================================================
                VCS_GATEWAY_LOG_INFO("[SIP-TRANSPORT] Creating SipMediaTransport for callId=%d", callId);

                // Use remote info from SDP negotiation (parsed from INVITE)
                std::string sipAddr = actualRemoteAddr;
                uint16_t sipPort = static_cast<uint16_t>(actualRemotePort);

                // Validate remote address and port
                if (sipAddr.empty() || sipPort == 0) {
                    VCS_GATEWAY_LOG_ERROR("[SIP-TRANSPORT] Invalid remote address/port: addr='%s', port=%d - cannot create transport",
                                        sipAddr.c_str(), sipPort);
                    NotifyMediaReady(callId, actualRemoteAddr, actualRemotePort);
                    return;
                }

                // Create SipMediaTransport (RTP passthrough mode)
                transport = std::make_shared<SipMediaTransport>(
                    session->sessionId,
                    sipAddr,
                    sipPort
                );

                // Get PJSIP's pjmedia_transport and set to SipMediaTransport
                // Note: Need to set transport for both audio (mediaIndex=0) and video (mediaIndex=1)
                auto sipTransport = std::dynamic_pointer_cast<SipMediaTransport>(transport);

                // Set audio transport (mediaIndex=0)
                void* audioTransport = call->GetPjMediaTransport(0);
                if (audioTransport && sipTransport) {
                    sipTransport->SetPjMediaTransport(audioTransport);
                    VCS_GATEWAY_LOG_INFO("[SIP-TRANSPORT] Audio transport (mediaIndex=0) attached");
                } else {
                    VCS_GATEWAY_LOG_WARNING("[SIP-TRANSPORT] Failed to get audio transport");
                }

                // Set video transport (mediaIndex=1) - for H264 video
                void* videoTransport = call->GetPjMediaTransport(1);
                if (videoTransport && sipTransport) {
                    sipTransport->SetPjVideoTransport(videoTransport);
                    VCS_GATEWAY_LOG_INFO("[SIP-TRANSPORT] Video transport (mediaIndex=1) attached for H264");
                } else {
                    VCS_GATEWAY_LOG_WARNING("[SIP-TRANSPORT] Failed to get video transport");
                }

                if (sipTransport) {
                    if (!sipTransport->Connect()) {
                        VCS_GATEWAY_LOG_ERROR("[SIP-TRANSPORT] Failed to connect SipMediaTransport");
                        NotifyMediaReady(callId, actualRemoteAddr, actualRemotePort);
                        return;
                    }

                    // [docs/09 §2.2] 不在此处 StartReceiving（与上方 Simple 模式
                    // 约定对齐）：收包门由 gateway 的 StartTransport 在媒体就绪
                    // 后开启，门开前的包静默丢弃（关键帧缺口由 media 的 FIR 补偿）。

                    // Update session info
                    // [V7.0 Phase-2 F] Address endpoint only; the transport
                    // object itself is registered below via the single write
                    // entry.
                    if (auto* sm = GetSessionManager()) {
                        sm->AttachTransport(session->sessionId,
                                            MediaStreamKind::Audio,
                                            nullptr, sipAddr, sipPort);
                    }
                }

                VCS_GATEWAY_LOG_INFO("[SIP-TRANSPORT] SipMediaTransport created: sessionId=%s, remote=%s:%d",
                                    session->sessionId.c_str(),
                                    session->GetPrimaryRemoteAddress().c_str(),
                                    session->GetPrimaryRemotePort());
            }
            break;
    }

    // Check if transport creation succeeded
    if (!transport) {
        VCS_GATEWAY_LOG_ERROR("Failed to create media transport for callId=%d", callId);
        NotifyMediaReady(callId, actualRemoteAddr, actualRemotePort);
        return;
    }

    // ============================================================
    // Video processing pipeline
    //
    // According to design document, VideoProcessor should be in media module.
    // Current implementation uses RTP passthrough mode, video directly forwarded without MCU composition.
    // Future needs to integrate media::VideoProcessor via MediaControl interface.
    // ============================================================
    if (!videoCodecName.empty() && videoPayloadType > 0) {
        VCS_GATEWAY_LOG_INFO("Video enabled: codec=%s(pt=%u)",
                            videoCodecName.c_str(), videoPayloadType);
        // TODO: Integrate media::VideoProcessor for MCU video composition
        // VideoProcessor should be called via MediaControl interface, not directly instantiated in gateway module
    } else {
        VCS_GATEWAY_LOG_WARNING("No video codec negotiated");
    }

    // Set session's media transport
    SetSessionMediaTransport(callId, transport);

    // ============================================================
    // Notify upper layer media ready, trigger MemberJoin
    // ============================================================
    NotifyMediaReady(callId, session->GetPrimaryRemoteAddress(), session->GetPrimaryRemotePort());

    // ============================================================
    // Important: Request Yealink to send key frame
    // Yealink may wait for MCU request before starting video transmission
    // This solves the problem of not receiving video stream
    //
    // [V5.5 Critical Fix] Select sending method based on transport type:
    // - Custom mode (SimpleRtpTransport): Use transport->SendFir() via independent UDP socket
    // - Other modes (SipMediaTransport): Use call->RequestKeyFrame() via PJSIP
    //
    // Reason: In Custom mode, PJSIP's RTCP path is separate from SimpleRtpTransport,
    //         PJSIP's RTCP may not be correctly received by Yealink
    // ============================================================
    {
        // Delay a bit for media to be fully ready
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Try to convert transport to SimpleRtpTransport
        auto simpleTransport = std::dynamic_pointer_cast<SimpleRtpTransport>(transport);

        if (simpleTransport) {
            // [Custom mode] Send FIR via SimpleRtpTransport's independent UDP socket
            // SSRC params: remoteSsrc=0 (unknown, let Yealink decide), localSsrc=0 (unknown)
            // Note: SendFir internally tries to find correct destination address
            VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] Sending FIR via SimpleRtpTransport UDP socket");
            if (simpleTransport->SendFir(0, 0)) {
                VCS_GATEWAY_LOG_INFO("[CUSTOM-TRANSPORT] FIR sent successfully via UDP socket to trigger video from Yealink");
            } else {
                VCS_GATEWAY_LOG_WARNING("[CUSTOM-TRANSPORT] Failed to send FIR via SimpleRtpTransport");
            }
        } else {
            // [Sip/H323/Webrtc mode] Send via PJSIP's RequestKeyFrame
            VCS_GATEWAY_LOG_INFO("[SIP-TRANSPORT] Sending keyframe request via PJSIP");
            if (call->RequestKeyFrame()) {
                VCS_GATEWAY_LOG_INFO("Keyframe request sent via PJSIP to trigger video from Yealink");
            } else {
                VCS_GATEWAY_LOG_WARNING("Failed to send keyframe request via PJSIP");
            }
        }
    }

    VCS_GATEWAY_LOG_INFO("OnCallMediaReady completed for callId=%d, transport_type=%s",
                        callId, transport->GetType().c_str());
}

void SipSignalingHandler::OnCallDisconnected(int callId, uint32_t code, const std::string& reason) {
    VCS_GATEWAY_LOG_INFO("[SipSignalingHandler] Call %d disconnected: %s (code=%u)",
                         callId, reason.c_str(), code);
    NotifyCallTerminated(callId, reason);
}

void SipSignalingHandler::OnMediaStatistics(int callId, uint32_t bytesSent, uint32_t bytesReceived,
                                           uint32_t packetsSent, uint32_t packetsReceived) {
    (void)callId;
    (void)bytesSent;
    (void)bytesReceived;
    (void)packetsSent;
    (void)packetsReceived;
    // Media statistics handling can be added here
}

// ============== PJSIP Initialization ==============

bool SipSignalingHandler::initPjsip() {
    if (pjsipInitialized_) {
        return true;
    }

    try {
        // Get pj::Endpoint from SipAdapter (created by SipAdapter)
        // Verify Endpoint is available
        GetPjsipEndpoint();

        VCS_GATEWAY_LOG_INFO("[SIP] Using pj::Endpoint from SipAdapter");

        // Create account - use -1 to let PJSIP auto-assign account ID
        VCS_GATEWAY_LOG_DEBUG("[Signaling] Creating SipAccount...");
        sipAccount_.reset(new SipAccount(this, -1));

        // Set SRTP configuration first (configured by application layer via SipAdapter)
        sipAccount_->SetSrtpConfig(srtpConfig_);

        sipAccount_->Configure(localUri_, server_, username_, password_);

        // 让 pjmedia 从配置的音频端口开始绑定 RTP（视频落在 port+2），
        // 首通呼叫通告的端口才和 legacy 配置一致；之后的呼叫由
        // SipCall::GetLocalMediaPorts() 读取实际端口写入 SDP。
        pj::AccountConfig& accountConfig = sipAccount_->GetPendingConfig();

        // 媒体归属在适配层一次性判定：当前所有传输都由应用侧发 RTP，协议栈
        // 只做信令。pjsua 对视频的处置是"start 后先暂停编码，再按
        // videoConfig.autoTransmitOutgoing 用 500ms 定时器恢复"，应用侧任何
        // 一次性停止都活不过这个定时器，所以必须在这里关掉。
        if (!UsesStackMediaEngine()) {
            accountConfig.videoConfig.autoTransmitOutgoing = false;
        }

        if (localAudioPort_ > 0) {
            accountConfig.mediaConfig.transportConfig.port = localAudioPort_;
            accountConfig.mediaConfig.transportConfig.portRange = kPjsuaRtpPortRange;

            // 转发 INVITE 的端口池移出 pjmedia 的占用块，两条路径各占一段。
            portAllocator_.SetRangeStart(
                static_cast<uint16_t>(localAudioPort_ + kPjsuaRtpPortBlock));

            VCS_GATEWAY_LOG_INFO(
                "[Signaling] pjmedia RTP ports: base=%u, range=%u; "
                "per-call pool starts at %u",
                (unsigned)localAudioPort_, (unsigned)kPjsuaRtpPortRange,
                (unsigned)(localAudioPort_ + kPjsuaRtpPortBlock));
        }
        else {
            VCS_GATEWAY_LOG_WARNING(
                "[Signaling] Local audio port is unset; pjmedia will pick ephemeral "
                "RTP ports and the SDP will advertise whatever was bound");
        }

        VCS_GATEWAY_LOG_DEBUG("[Signaling] Calling sipAccount_->create()...");
        sipAccount_->create(accountConfig);
        VCS_GATEWAY_LOG_DEBUG("[Signaling] sipAccount_->create() done");

        // 确认应用自管理媒体真的下发到了协议栈（否则 pjsua 仍会建媒体通道）。
        const int accId = sipAccount_->getId();
        VCS_GATEWAY_LOG_INFO("[Signaling] account %d mediaAppManaged=%d",
                             accId, pjsua_var.acc[accId].cfg.media_app_managed ? 1 : 0);

        pjsipInitialized_ = true;
        VCS_GATEWAY_LOG_INFO("[SIP] SipSignalingHandler initialized");
        return true;

    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("[SIP] Failed to initialize: %s", err.info().c_str());
        return false;
    }
}

void SipSignalingHandler::cleanupPjsip() {
    // ============================================================
    // Important: Do NOT destroy account!
    // Account needs to remain to handle subsequent inbound INVITE requests
    // Account lifecycle managed by SignalingHandler
    // ============================================================

    {
        std::lock_guard<std::mutex> lock(callsMutex_);
        activeCalls_.clear();
    }

    // sipAccount_.reset();  // Do NOT destroy account!
    // PJSIP Endpoint managed by SipAdapter, no cleanup needed here
    pjsipInitialized_ = false;
}

SipCall* SipSignalingHandler::GetActiveCall(int callId) {
    std::lock_guard<std::mutex> lock(callsMutex_);
    auto it = activeCalls_.find(callId);
    if (it != activeCalls_.end()) {
        return it->second.get();
    }
    return nullptr;
}

void SipSignalingHandler::RemoveActiveCall(int callId) {
    // Destroy calls retired by a previous hangup first: their callback stacks
    // have already unwound. Must run before taking callsMutex_ (see
    // DrainRetiredCalls) and must never run on the retiring call's own stack.
    DrainRetiredCalls();

    std::lock_guard<std::mutex> lock(callsMutex_);

    // 从端口池分配的每呼叫端口随呼叫结束归还，避免下一通呼叫无端口可用。
    auto it = callPorts_.find(callId);
    if (it != callPorts_.end()) {
        if (it->second.first != 0 && it->second.second != 0) {
            portAllocator_.Release(it->second.first, it->second.second);
            VCS_GATEWAY_LOG_INFO("[SIP] Released per-call ports for callId=%d: audio=%u, video=%u",
                                 callId, it->second.first, it->second.second);
        }
        callPorts_.erase(it);
    }

    // Park instead of destroying: this runs on the retiring SipCall's own
    // callback stack (SipCall::onCallState -> callback_->OnCallState ->
    // TerminateSession -> RemoveActiveCall). Erasing here destroys the object
    // whose method is still executing, so the remaining statements of
    // onCallState (logging callInfo_) read freed memory.
    auto callIt = activeCalls_.find(callId);
    if (callIt != activeCalls_.end()) {
        retiredCalls_.push_back(std::move(callIt->second));
        activeCalls_.erase(callIt);
    }
}

void SipSignalingHandler::DrainRetiredCalls() {
    std::vector<std::unique_ptr<SipCall>> doomed;
    {
        std::lock_guard<std::mutex> lock(callsMutex_);
        doomed.swap(retiredCalls_);
    }
    // 锁外析构：SipCall 的析构会回调 handler（释放端口/注销传输）。
    doomed.clear();
}

/**
 * @brief Extract number (user part) from SIP URI
 *
 * Parse SIP URI format and extract number part.
 * Supported formats:
 *   - sip:1001@192.168.1.100:5060
 *   - sip:1001@192.168.1.100
 *   - 1001@192.168.1.100:5060
 *   - 1001@192.168.1.100
 *   - 1001
 *
 * @param sipUri SIP URI string
 * @return Extracted number, empty string if parsing fails
 */
static std::string ExtractSipNumber(const std::string& sipUri) {
    if (sipUri.empty()) {
        return "";
    }

    std::string uri = sipUri;

    // Remove angle brackets (if any), e.g. <sip:1001@host>
    if (uri.front() == '<') {
        size_t closePos = uri.find('>');
        if (closePos != std::string::npos) {
            uri = uri.substr(1, closePos - 1);
        } else {
            uri = uri.substr(1);
        }
    }

    // Remove "sip:" prefix (if any)
    if (uri.find("sip:") == 0) {
        uri = uri.substr(4);
    }

    // Find "@" symbol, extract part before @
    size_t atPos = uri.find('@');
    if (atPos != std::string::npos) {
        uri = uri.substr(0, atPos);
    }

    // Remove any whitespace
    while (!uri.empty() && (uri[0] == ' ' || uri[0] == '\t')) {
        uri = uri.substr(1);
    }
    while (!uri.empty() && (uri.back() == ' ' || uri.back() == '\t')) {
        uri.pop_back();
    }

    return uri;
}

// ============================================================================
// INVITE Interception Module Implementation (merged from SipExternalModule)
// ============================================================================

// Get PJSIP endpoint
// pjsua_var declared in pjsua-lib/pjsua_internal.h
static pjsip_endpoint* getPjsipEndpt() {
    return pjsua_var.endpt;
}

bool SipSignalingHandler::initModule() {
    if (this == nullptr) {
        return false;
    }

    VCS_GATEWAY_LOG_DEBUG("[Signaling] initModule START, this=%p", (void*)this);

    // Set static instance pointer
    SetInviteModuleInstance(this);

    // [Important] When using PJSUA2 mode, pjsip_inv_usage_init() has already been called by PJSUA internally
    VCS_GATEWAY_LOG_INFO("[Signaling] Skipping pjsip_inv_usage_init() - PJSUA2 has already initialized it");

    // Get endpoint
    pjsip_endpoint* endpt = getPjsipEndpt();
    if (endpt == nullptr) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed: pjsua not initialized");
        return false;
    }
    VCS_GATEWAY_LOG_DEBUG("[Signaling] Got SIP endpoint: %p", (void*)endpt);

    // Debug: confirm this and module_ addresses
    VCS_GATEWAY_LOG_DEBUG("[Signaling] this=%p, &module_=%p", (void*)this, (void*)&module_);

    // Initialize module - use direct assignment
    module_.id = -1;
    module_.name = module_name_;
    // [V5.5 Critical Fix] PJSIP priority: SMALLER number = HIGHER priority
    // PJSIP module processing order (smaller number = higher priority):
    //   7 (mod-pjsua-log) -> 16 (mod-tsx-layer) -> 32 (mod-ua) -> 48 (mod-invite) -> 64 (mod-pjsua)
    //
    // INVITE handling: Need priority < 32 (e.g., 16 or 1) to intercept BEFORE mod-ua
    // BYE handling: Need priority > 32 to ensure dialog is associated
    //
    // Solution: Use very HIGH priority (very small number like 1) to intercept INVITE first.
    // For BYE, we use fallback methods (iterate activeInvites_) since dialog may not be available.
    module_.priority = PJSIP_MOD_PRIORITY_TRANSPORT_LAYER - 1;  // 15 - intercept BEFORE mod-ua
    //ZGB
   // module_.on_rx_request = &SipSignalingHandler::on_rx_request;
    // Note: on_tsx_state not set - BYE handling done in on_rx_request via fallback methods

    VCS_GATEWAY_LOG_DEBUG("[Signaling] Module initialized, priority=%d", module_.priority);

    // Register module
    pj_status_t status = pjsip_endpt_register_module(endpt, &module_);

    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to register module: %d", status);
        return false;
    }

    VCS_GATEWAY_LOG_DEBUG("[Signaling] Module registered, id=%d", module_.id);
    return true;
}

void SipSignalingHandler::deinitModule() {
    if (module_.id >= 0) {
        // Get endpoint
        pjsip_endpoint* endpt = getPjsipEndpt();

        // Cleanup all active invite sessions
        // [V5.5 Fix] Cast uintptr_t back to pjsip_inv_session* before use
        {
            std::lock_guard<std::mutex> lock(invitesMutex_);
            for (auto& pair : activeInvites_) {
                pjsip_inv_session* inv = reinterpret_cast<pjsip_inv_session*>(pair.first);
                if (inv) {
                    VCS_GATEWAY_LOG_INFO("[Signaling] Force terminating active invite");
                    pjsip_inv_terminate(inv, 603, PJ_FALSE);
                }
            }
            activeInvites_.clear();
        }

        // [V7.0 Fix] Clear the Call-ID secondary index as well
        {
            std::lock_guard<std::mutex> lock(callIdMutex_);
            callIdToInvite_.clear();
        }

        if (endpt) {
            pjsip_endpt_unregister_module(endpt, &module_);
        } else {
            VCS_GATEWAY_LOG_WARNING("[Signaling] endpoint is NULL during deinit");
        }
        module_.id = -1;
    }

    // Clear static instance pointer
    if (GetInviteModuleInstance() == this) {
        SetInviteModuleInstance(nullptr);
    }
}

RemoteMediaInfo SipSignalingHandler::parseRemoteMediaInfo(pjsip_rx_data* rdata) {
    RemoteMediaInfo info;

    // [V7.0 Fix] Parse SIP Call-ID for BYE matching
    pjsip_cid_hdr* cid_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CALL_ID, nullptr);
    if (cid_hdr) {
        char cidBuf[256];
        snprintf(cidBuf, sizeof(cidBuf), "%.*s", (int)cid_hdr->id.slen, cid_hdr->id.ptr);
        info.callId = cidBuf;
        VCS_GATEWAY_LOG_INFO("[Signaling] Parsed Call-ID: %s", cidBuf);
    }

    // Parse conference ID from INVITE's To header
    pjsip_to_hdr* to_hdr = (pjsip_to_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_TO, nullptr);
    if (to_hdr && to_hdr->uri) {
        void* generic_uri = pjsip_uri_get_uri(to_hdr->uri);
        pjsip_sip_uri* sip_uri = (pjsip_sip_uri*)generic_uri;
        if (sip_uri && sip_uri->user.ptr) {
            char user[64];
            snprintf(user, sizeof(user), "%.*s", (int)sip_uri->user.slen, sip_uri->user.ptr);
            info.conferenceId = user;
            VCS_GATEWAY_LOG_INFO("[Signaling] Parsed conference ID from To header: %s", user);
        }
    }

    // Get message body (SDP)
    pjsip_msg_body* body = rdata->msg_info.msg->body;
    if (!body || !body->data) {
        VCS_GATEWAY_LOG_WARNING("[Signaling] No message body in INVITE");
        return info;
    }

    // Parse SDP
    pjmedia_sdp_session* sdp = nullptr;
    pj_status_t status = pjmedia_sdp_parse(
        rdata->tp_info.pool,
        (char*)body->data,
        body->len,
        &sdp);

    if (status != PJ_SUCCESS || !sdp) {
        VCS_GATEWAY_LOG_WARNING("[Signaling] Failed to parse SDP");
        return info;
    }

    // Parse connection address (c= line)
	pj_str_t* hostAddr = nullptr;
	if (sdp->conn) {
		// 会话级别的 c= 行
		hostAddr = &sdp->conn->addr;
	}
	else if (sdp->media_count > 0 && sdp->media[0]->conn) {
		// 媒体级别的 c= 行（终端常用）
		hostAddr = &sdp->media[0]->conn->addr;
	}

	if (hostAddr && hostAddr->ptr) {
		char addr[64];
		snprintf(addr, sizeof(addr), "%.*s", (int)hostAddr->slen, hostAddr->ptr);
		info.address = addr;
	}

    // Traverse media lines
    for (unsigned i = 0; i < sdp->media_count; i++) {
        pjmedia_sdp_media* m = sdp->media[i];
        if (!m) continue;

        // Get port
        uint16_t port = m->desc.port;

        // Get first payload type
        unsigned pt = 0;
        if (m->desc.fmt_count > 0) {
            char pt_str[8];
            pj_str_t* fmt = &m->desc.fmt[0];
            snprintf(pt_str, sizeof(pt_str), "%.*s", (int)fmt->slen, fmt->ptr);
            pt = atoi(pt_str);
        }

        // Parse media type
        char media_type[16];
        pj_str_t* media_str = &m->desc.media;
        snprintf(media_type, sizeof(media_type), "%.*s", (int)media_str->slen, media_str->ptr);

        // [RFC 5761] Detect a=rtcp-mux on this media line. It tells us whether
        // the peer expects RTCP on the RTP port (mux) or on RTP port + 1.
        bool rtcpMux = false;
        for (unsigned j = 0; j < m->attr_count; j++) {
            pjmedia_sdp_attr* attr = m->attr[j];
            if (attr && attr->name.ptr && pj_strcmp2(&attr->name, "rtcp-mux") == 0) {
                rtcpMux = true;
                break;
            }
        }

        if (strcmp(media_type, "audio") == 0 && port > 0) {
            info.audioPort = port;
            info.audioPayloadType = pt;
            info.audioRtcpMux = rtcpMux;
            VCS_GATEWAY_LOG_INFO("[Signaling] Found audio: port=%u, pt=%u, rtcp-mux=%d",
                                 port, pt, rtcpMux ? 1 : 0);
        } else if (strcmp(media_type, "video") == 0 && port > 0) {
            info.videoPort = port;
            info.videoPayloadType = pt;
            info.videoRtcpMux = rtcpMux;
            VCS_GATEWAY_LOG_INFO("[Signaling] Found video: port=%u, pt=%u, rtcp-mux=%d",
                                 port, pt, rtcpMux ? 1 : 0);

            // Parse H.264 fmtp attribute
            for (unsigned j = 0; j < m->attr_count; j++) {
                pjmedia_sdp_attr* attr = m->attr[j];
                if (!attr || !attr->name.ptr) continue;

                // Check if it's fmtp attribute
                if (pj_strcmp2(&attr->name, "fmtp") != 0) continue;

                // Parse fmtp value. [docs/11 §3.2] sprop-parameter-sets can be
                // long, so keep enough room to avoid truncating it.
                char fmtp_buf[1024];
                snprintf(fmtp_buf, sizeof(fmtp_buf), "%.*s",
                        (int)attr->value.slen, attr->value.ptr);

                VCS_GATEWAY_LOG_INFO("[Signaling] Found H.264 fmtp: %s", fmtp_buf);

                // Extract profile-level-id
                char* pos = strstr(fmtp_buf, "profile-level-id=");
                if (pos) {
                    pos += 17;
                    char* end = strchr(pos, ';');
                    if (end) *end = '\0';
                    // [V5.5 Critical Fix] Must use assign() to copy string, not direct pointer assignment
                    info.h264ProfileLevelId.assign(pos);
                    if (end) *end = ';';  // Restore the semicolon for subsequent parsing
                }

                // Extract max-mbps
                pos = strstr(fmtp_buf, "max-mbps=");
                if (pos) {
                    pos += 9;
                    char* end = strchr(pos, ';');
                    if (end) *end = '\0';
                    // [V5.5 Critical Fix] Must use assign() to copy string, not direct pointer assignment
                    // The pos pointer points to fmtp_buf which will be destroyed when function returns
                    info.h264MaxMbps.assign(pos);
                    if (end) *end = ';';  // Restore the semicolon for subsequent parsing
                }

                // Extract max-fs
                pos = strstr(fmtp_buf, "max-fs=");
                if (pos) {
                    pos += 7;
                    char* end = strchr(pos, ';');
                    if (end) *end = '\0';
                    // [V5.5 Critical Fix] Must use assign() to copy string, not direct pointer assignment
                    info.h264MaxFs.assign(pos);
                    if (end) *end = ';';  // Restore the semicolon for subsequent parsing
                }

                // Extract packetization-mode
                pos = strstr(fmtp_buf, "packetization-mode=");
                if (pos) {
                    // [V5.5 Critical Fix] Off-by-one: "packetization-mode=" is 19 chars, not 18
                    pos += 19;  // Skip past the entire "packetization-mode=" prefix including '='
                    char* end = strchr(pos, ';');
                    if (end) *end = '\0';
                    info.h264PacketizationMode = (atoi(pos) == 1);
                }

                // Extract sprop-parameter-sets (out-of-band SPS/PPS)
                pos = strstr(fmtp_buf, "sprop-parameter-sets=");
                if (pos) {
                    pos += 21;  // "sprop-parameter-sets=" = 21 chars
                    char* end = strchr(pos, ';');
                    if (end) *end = '\0';
                    info.h264SpropParameterSets.assign(pos);
                }

                break;  // Only parse first fmtp
            }
        }
    }

    return info;
}

pj_status_t SipSignalingHandler::sendErrorResponse(pjsip_rx_data* rdata, int code, const char* reason) {
    pjsip_tx_data* tdata = nullptr;
    pj_status_t status = pjsip_endpt_create_response(
        pjsua_var.endpt, rdata, code, pj_cstr(&tdata->msg->line.status.reason, reason), &tdata);

    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to create error response %d: %d", code, status);
        return status;
    }

    status = pjsip_endpt_send_response(pjsua_var.endpt, nullptr, tdata, nullptr, nullptr);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to send error response %d: %d", code, status);
    }

    return status;
}

void SipSignalingHandler::UpdateInviteSessionId(pjsip_inv_session* inv, const std::string& sessionId) {
    // [V5.5 Fix] Cast inv to uintptr_t for map lookup
    std::lock_guard<std::mutex> lock(invitesMutex_);
    auto it = activeInvites_.find(reinterpret_cast<uintptr_t>(inv));
    if (it != activeInvites_.end()) {
        it->second.sessionId = sessionId;
        VCS_GATEWAY_LOG_INFO("[Signaling] Updated activeInvites_[%p].sessionId = '%s'",
                           (void*)inv, sessionId.c_str());
    } else {
        VCS_GATEWAY_LOG_WARNING("[Signaling] UpdateInviteSessionId: inv not found in activeInvites_");
    }
}

bool SipSignalingHandler::GetPerCallLocalPorts(pjsip_inv_session* inv,
                                                uint16_t& outAudioPort,
                                                uint16_t& outVideoPort) const {
    if (!inv) {
        outAudioPort = 0;
        outVideoPort = 0;
        return false;
    }
    std::lock_guard<std::mutex> lock(invitesMutex_);
    auto it = activeInvites_.find(reinterpret_cast<uintptr_t>(inv));
    if (it == activeInvites_.end()) {
        outAudioPort = 0;
        outVideoPort = 0;
        return false;
    }
    if (!it->second.portsAllocatedFromPool ||
        it->second.allocatedAudioPort == 0 ||
        it->second.allocatedVideoPort == 0) {
        outAudioPort = 0;
        outVideoPort = 0;
        return false;
    }
    outAudioPort = it->second.allocatedAudioPort;
    outVideoPort = it->second.allocatedVideoPort;
    return true;
}

std::string SipSignalingHandler::BuildLocalContactUri(pjsip_rx_data* rdata) const {
    // Host: prefer the address we advertise in SDP so the peer's ACK/BYE
    // reach the same interface that carries media. Fall back to the local
    // address of the transport that received the INVITE (handles explicit
    // bind addresses), then loopback.
    std::string host;
    {
        std::lock_guard<std::mutex> lock(mediaMutex_);
        if (!localMediaAddress_.empty() &&
            localMediaAddress_ != "0.0.0.0" && localMediaAddress_ != "::") {
            host = localMediaAddress_;
        }
    }
    uint16_t port = localPort_;
    if (rdata && rdata->tp_info.transport) {
        const pjsip_transport* tp = rdata->tp_info.transport;
        if (host.empty() && tp->local_name.host.slen > 0) {
            std::string tpHost(tp->local_name.host.ptr, tp->local_name.host.slen);
            if (tpHost != "0.0.0.0" && tpHost != "::") {
                host = tpHost;
            }
        }
        if (port == 0) {
            port = static_cast<uint16_t>(tp->local_name.port);
        }
    }
    if (host.empty()) {
        host = "127.0.0.1";
    }
    if (port == 0) {
        port = 5060;
    }

    std::ostringstream oss;
    oss << "<sip:" << host << ":" << port << ">";
    return oss.str();
}

void SipSignalingHandler::AddContactHeader(pjsip_tx_data* tdata, pjsip_rx_data* rdata) const {
    if (!tdata || !tdata->msg) {
        return;
    }
    if (pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CONTACT, nullptr) != nullptr) {
        return;
    }

    const std::string contactUri = BuildLocalContactUri(rdata);

    // pjsip_parse_uri() parses in place and keeps pointers into the buffer,
    // so the URI text must live as long as the URI object (tx-data pool),
    // not in this function's frame.
    char* uriBuf = static_cast<char*>(pj_pool_alloc(tdata->pool, contactUri.size() + 1));
    memcpy(uriBuf, contactUri.c_str(), contactUri.size() + 1);

    pjsip_contact_hdr* contact = pjsip_contact_hdr_create(tdata->pool);
    contact->uri = pjsip_parse_uri(tdata->pool, uriBuf, contactUri.size(), 0);
    if (!contact->uri) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to parse Contact URI '%s'",
                              contactUri.c_str());
        return;
    }
    pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)contact);
    VCS_GATEWAY_LOG_INFO("[Signaling] Contact: %s", contactUri.c_str());
}

pj_status_t SipSignalingHandler::handleInvite(pjsip_rx_data* rdata) {
    pj_status_t status;
    pjsip_dialog* dlg = nullptr;
    pjsip_tx_data* tdata_err = nullptr;
    pj_pool_t* pool = nullptr;

    // First parse remote media info (includes H.264 parameters and conference ID)
    RemoteMediaInfo remoteInfo = parseRemoteMediaInfo(rdata);
    if (!remoteInfo.address.empty()) {
        VCS_GATEWAY_LOG_INFO("[Signaling] Remote media info parsed: addr=%s, audio=%u, video=%u",
                            remoteInfo.address.c_str(), remoteInfo.audioPort, remoteInfo.videoPort);
    }

    // Create UAS dialog. The dialog's local Contact is pinned to the
    // configured signaling port: pjsip_dlg_create_response() does not copy
    // it into responses (see AddContactHeader), but re-INVITE answers built
    // through pjsip_inv_answer rely on dlg->local.contact.
    const std::string localContact = BuildLocalContactUri(rdata);
    pj_str_t localContactStr = pj_str(const_cast<char*>(localContact.c_str()));
    status = pjsip_dlg_create_uas_and_inc_lock(pjsip_ua_instance(), rdata,
                                               &localContactStr, &dlg);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] pjsip_dlg_create_uas_and_inc_lock failed: %d", status);
        status = pjsip_endpt_create_response(pjsua_var.endpt, rdata, 500, nullptr, &tdata_err);
        if (status == PJ_SUCCESS && tdata_err) {
            pjsip_endpt_send_response(pjsua_var.endpt, nullptr, tdata_err, nullptr, nullptr);
        }
        return status;
    }
    VCS_GATEWAY_LOG_INFO("[Signaling] Dialog created: %p", dlg);

    // Create memory pool
    pool = pjsip_endpt_create_pool(pjsua_var.endpt, "invite_pool", 2560, 256);
    if (!pool) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to create pool");
        pjsip_dlg_dec_lock(dlg);
        status = pjsip_endpt_create_response(pjsua_var.endpt, rdata, 500, nullptr, &tdata_err);
        if (status == PJ_SUCCESS && tdata_err) {
            pjsip_endpt_send_response(pjsua_var.endpt, nullptr, tdata_err, nullptr, nullptr);
        }
        return PJ_ENOMEM;
    }

    // ====================================================================
    // [V7.1 Fix 2026-08-13] Allocate a fresh local RTP port pair for THIS
    // call BEFORE building the answer SDP. Without this every concurrent
    // terminal would try to bind 4004/4006 simultaneously, the second bind
    // would succeed (SO_REUSEADDR), and the kernel would demultiplex UDP
    // datagrams between the two sockets in an implementation-defined way
    // (SSRC oscillation observed in RTPEngine::HandleParsedRtp).
    //
    // We allocate a pair eagerly, but only commit the ports to the
    // InviteSessionInfo (and therefore only Release() them in
    // DISCONNECTED) if we make it past pjsip_inv_create_uas successfully.
    // If the invite-session creation fails we Release() inline to keep
    // the pool balanced.
    // ====================================================================
	std::string localIp;
	uint16_t    audioPort = 0;
	uint16_t    videoPort = 0;
    uint16_t allocatedAudioPort = 0;
    uint16_t allocatedVideoPort = 0;
    bool     portsFromPool      = portAllocator_.Allocate(allocatedAudioPort, allocatedVideoPort);
    if (portsFromPool) {
        audioPort = allocatedAudioPort;
        videoPort = allocatedVideoPort;
        VCS_GATEWAY_LOG_INFO(
            "[V7.1] Allocated per-call RTP ports for inv=%p: audio=%u, video=%u",
            (void*)nullptr, audioPort, videoPort);
    } else {
        // Pool exhausted fall back to the global ports. This will fail
        // loudly at bind() time (because we also turned SO_REUSEADDR off
        // in SimpleRtpTransport), which is the right behaviour: a soft
        // "reuse the same port" is exactly the bug we are fixing.
        getMediaInfo(localIp, audioPort, videoPort);
        VCS_GATEWAY_LOG_WARNING(
            "[V7.1] Port pool exhausted, falling back to global ports "
            "audio=%u, video=%u (subsequent bind may fail)",
            audioPort, videoPort);
    }

    // Generate external SDP
    //std::string localIp;
    if (portsFromPool) {
        std::lock_guard<std::mutex> lock(mediaMutex_);
        localIp = localMediaAddress_;
    } else {
        getMediaInfo(localIp, audioPort, videoPort);
    }

    // SDP 协商/校验已移到 pjsua 的呼叫模型（SipCall::Answer / onCallRxOffer）。
    // 本函数只在 INVITE 拦截模块启用时作为旁路路径存在，保持原有构造。
    pjmedia_sdp_session* localSdp = buildExternalSdp(pool, localIp, audioPort, videoPort, &remoteInfo);
    if (!localSdp) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to build external SDP");
        if (portsFromPool) {
            portAllocator_.Release(allocatedAudioPort, allocatedVideoPort);
            VCS_GATEWAY_LOG_INFO("[V7.1] Released per-call ports after SDP build failure");
        }
        pjsip_endpt_create_response(pjsua_var.endpt, rdata, 488, nullptr, &tdata_err);
        if (tdata_err) {
            pjsip_endpt_send_response(pjsua_var.endpt, nullptr, tdata_err, nullptr, nullptr);
        }
        pjsip_dlg_dec_lock(dlg);
        pjsip_endpt_release_pool(pjsua_var.endpt, pool);
        return PJ_EINVALIDOP;
    }

    VCS_GATEWAY_LOG_INFO("[Signaling] External SDP prepared");

    // Create UAS invite session
    // Note: PJSIP 2.17 - third parameter is local_sdp (SDP offer), not callback
    // Callback is set globally via pjsip_inv_usage_init() or stored in dialog
    pjsip_inv_session* inv = nullptr;
    unsigned inv_options = 0;

    // Pass local SDP as third parameter (can be NULL if we don't have SDP yet)
    status = pjsip_inv_create_uas(dlg, rdata, localSdp, inv_options, &inv);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] pjsip_inv_create_uas failed: %d", status);
        // [V7.1 Fix] Return the per-call ports to the pool on early failure
        // so the next INVITE can grab them.
        if (portsFromPool) {
            portAllocator_.Release(allocatedAudioPort, allocatedVideoPort);
            VCS_GATEWAY_LOG_INFO("[V7.1] Released per-call ports after pjsip_inv_create_uas failure");
        }
        status = pjsip_endpt_create_response(pjsua_var.endpt, rdata, 500, nullptr, &tdata_err);
        if (status == PJ_SUCCESS && tdata_err) {
            pjsip_endpt_send_response(pjsua_var.endpt, nullptr, tdata_err, nullptr, nullptr);
        }
        pjsip_dlg_dec_lock(dlg);
        pjsip_endpt_release_pool(pjsua_var.endpt, pool);
        return status;
    }

    VCS_GATEWAY_LOG_INFO("[Signaling] Invite session created with state callback: %p", inv);

    // [V5.5 Critical Fix] Store INVITE session in module data, used for BYE request handling
    // Use mod_data[module_id] to store inv pointer, so inv can be found when handling BYE
    if (module_.id >= 0) {
        inv->mod_data[module_.id] = inv;
        VCS_GATEWAY_LOG_INFO("[Signaling] INVITE session stored in mod_data[module_id=%d]", module_.id);
    }

    // Add to active invite list
    // Note: In module mode, sessionId and conferenceId will be set in onExternalCallEstablished
    // [V5.5 Fix] Use uintptr_t as key to avoid dangling pointer issues
    {
        std::lock_guard<std::mutex> lock(invitesMutex_);
        InviteSessionInfo info;
        info.sessionId = "";  // Will be set in onExternalCallEstablished
        info.conferenceId = remoteInfo.conferenceId.empty() ? "local" : remoteInfo.conferenceId;
        info.callId = remoteInfo.callId;  // [V7.0 Fix] Store Call-ID for precise BYE matching

        // [V7.1 Fix 2026-08-13] Stash the per-call port pair so the
        // transport builder and the DISCONNECTED-cleanup path can find it
        // without re-running the allocator.
        info.allocatedAudioPort     = allocatedAudioPort;
        info.allocatedVideoPort     = allocatedVideoPort;
        info.portsAllocatedFromPool = portsFromPool;

        activeInvites_[reinterpret_cast<uintptr_t>(inv)] = info;
        VCS_GATEWAY_LOG_INFO(
            "[Signaling] Added to activeInvites_: inv=%p, confId=%s, callId='%s', "
            "ports=(audio=%u, video=%u, fromPool=%d), size=%zu",
            (void*)inv, info.conferenceId.c_str(), info.callId.c_str(),
            info.allocatedAudioPort, info.allocatedVideoPort,
            (int)info.portsAllocatedFromPool, activeInvites_.size());
        // Print all saved INVITE sessions
        for (const auto& pair : activeInvites_) {
            VCS_GATEWAY_LOG_INFO(
                "[Signaling]   activeInvites_[0x%llx] = {sessionId='%s', confId='%s', "
                "callId='%s', ports=(%u,%u)}",
                (unsigned long long)pair.first, pair.second.sessionId.c_str(),
                pair.second.conferenceId.c_str(), pair.second.callId.c_str(),
                pair.second.allocatedAudioPort, pair.second.allocatedVideoPort);
        }
    }

    // [V7.0 Fix] Add to Call-ID secondary index for O(1) BYE lookup
    if (!remoteInfo.callId.empty()) {
        std::lock_guard<std::mutex> lock(callIdMutex_);
        callIdToInvite_[remoteInfo.callId] = reinterpret_cast<uintptr_t>(inv);
        VCS_GATEWAY_LOG_INFO("[Signaling] Added to callIdToInvite_: callId='%s' -> inv=%p",
                            remoteInfo.callId.c_str(), (void*)inv);
    }

    // Store remote media info
    if (!remoteInfo.address.empty()) {
        std::lock_guard<std::mutex> lock(remoteInfoMutex_);
        remoteMediaInfos_[reinterpret_cast<uintptr_t>(inv)] = remoteInfo;
    }

    // Send 180 Ringing
    pjsip_tx_data* tdata = nullptr;
    status = pjsip_inv_initial_answer(inv, rdata, 180, nullptr, nullptr, &tdata);
    if (status == PJ_SUCCESS) {
        AddContactHeader(tdata, rdata);
        pjsip_inv_send_msg(inv, tdata);
        VCS_GATEWAY_LOG_INFO("[Signaling] 180 Ringing sent");
    }

    // ============================================================
    // [V7.0 Phase-2 G] CRITICAL BACKPRESSURE FIX
    // ============================================================
    // Problem (legacy flow):
    //   The 200 OK used to be sent HERE, BEFORE the establishedCallback_ was
    //   invoked. The peer, on receiving 200 OK, immediately sends ACK and
    //   starts streaming RTP. But at that moment the MCU has not yet
    //   created a media transport, registered a CallSession, added the
    //   member to RTPEngine, or joined the conference. The result is a
    //   race condition in which the first keyframe (and many subsequent
    //   packets) is silently dropped because no consumer is attached to
    //   the local UDP socket.
    //
    // Fix (new flow):
    //   1. Run the full backend setup (transport, session, conference join)
    //      via the establishedCallback_ BEFORE sending 200 OK.
    //   2. Only when the callback returns `true` (setup succeeded) do we
    //      send 200 OK. From the peer's perspective, 200 OK is now the
    //      explicit "the MCU is ready" signal: by the time it arrives, the
    //      entire receive pipeline is already wired up.
    //   3. If the callback returns `false` (setup failed), we send a 5xx
    //      error response so the peer knows the call was rejected, rather
    //      than tearing the call down after the peer has already started
    //      sending media.
    // ============================================================

    bool backendOk = true;
    if (establishedCallback_) {
        VCS_GATEWAY_LOG_INFO("[Signaling] Invoking CallEstablishedCallback BEFORE 200 OK "
                             "(backend setup must complete before peer is told to send media)");
        // The remoteInfo passed to the callback is the one parsed from the
        // original INVITE (saved earlier in activeInvites_ via remoteMediaInfos_).
        // Re-read it under remoteInfoMutex_ to avoid a use-after-free if the
        // PJSIP-managed SDP is released between handleInvite's parse step
        // and now.
        RemoteMediaInfo callbackInfo;
        {
            std::lock_guard<std::mutex> rLock(remoteInfoMutex_);
            auto rIt = remoteMediaInfos_.find(reinterpret_cast<uintptr_t>(inv));
            if (rIt != remoteMediaInfos_.end()) {
                callbackInfo = rIt->second;
            } else {
                // Fallback: use the one we just parsed in this function.
                callbackInfo = remoteInfo;
            }
        }
        backendOk = establishedCallback_(inv, callbackInfo);
        VCS_GATEWAY_LOG_INFO("[Signaling] CallEstablishedCallback returned: %s",
                             backendOk ? "OK (backend ready)" : "FAILED");
    } else {
        VCS_GATEWAY_LOG_WARNING("[Signaling] No CallEstablishedCallback registered; "
                                "sending 200 OK without backend setup (legacy mode)");
    }

    // Persist the result on the InviteSessionInfo so the post-200 OK
    // state-change callback and any later diagnostics see a consistent
    // view of what happened.
    {
        std::lock_guard<std::mutex> iLock(invitesMutex_);
        auto iIt = activeInvites_.find(reinterpret_cast<uintptr_t>(inv));
        if (iIt != activeInvites_.end()) {
            iIt->second.backendSetupDone = true;
            iIt->second.backendSetupOk   = backendOk;
        }
    }

    if (!backendOk) {
        // Backend setup failed; do NOT tell the peer to start streaming.
        // Send 500 Server Error so the peer can cleanly give up.
        VCS_GATEWAY_LOG_ERROR("[Signaling] Backend setup FAILED, sending 500 Server Error "
                              "instead of 200 OK to avoid the peer streaming into a "
                              "half-initialized receiver");
        pjsip_tx_data* errTdata = nullptr;
        pj_status_t errStatus = pjsip_endpt_create_response(
            pjsua_var.endpt, rdata, 500, nullptr, &errTdata);
        if (errStatus == PJ_SUCCESS && errTdata) {
            pjsip_inv_send_msg(inv, errTdata);
        } else if (errTdata) {
            pjsip_endpt_send_response(pjsua_var.endpt, nullptr, errTdata, nullptr, nullptr);
        }
        pjsip_dlg_dec_lock(dlg);
        pjsip_endpt_release_pool(pjsua_var.endpt, pool);
        return PJ_EINVAL;
    }

    // Send 200 OK - the peer's "start streaming" signal.
    status = create200OkWithSdp(dlg, rdata, inv, localSdp, &tdata);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] create200OkWithSdp failed: %d", status);
        pjsip_endpt_create_response(pjsua_var.endpt, rdata, 500, nullptr, &tdata_err);
        if (tdata_err) {
            pjsip_endpt_send_response(pjsua_var.endpt, nullptr, tdata_err, nullptr, nullptr);
        }
        pjsip_dlg_dec_lock(dlg);
        pjsip_endpt_release_pool(pjsua_var.endpt, pool);
        return status;
    }

    // ============================================================
    // [CRITICAL FIX 2026-08-11] Use pjsip_dlg_send_response, NOT
    // pjsip_inv_send_msg.
    //
    // Why this matters (the actual root cause of "call fails" symptom):
    //   - create200OkWithSdp() built the response with
    //     pjsip_dlg_create_response(), which sits at the DIALOG layer.
    //   - pjsip_inv_send_msg() sits at the INVITE-SESSION layer and
    //     expects the response to have been created by the invite
    //     session itself (e.g. via pjsip_inv_initial_answer /
    //     pjsip_inv_answer). When the invite session didn't author the
    //     tdata, pjsip_inv_send_msg silently does NOT complete the
    //     state machine: the call stays in EARLY, the peer's ACK never
    //     triggers the CONFIRMED transition, and the peer eventually
    //     tears the call down.
    //   - The correct primitive for "I built the response via
    //     pjsip_dlg_create_response and now want to ship it" is
    //     pjsip_dlg_send_response(dlg, invite_tsx, tdata). This is
    //     what SipCall.cpp does for the legacy 200-OK path and it
    //     works.
    //
    // After pjsip_dlg_send_response returns, the invite session
    // state-machine will fire onInviteStateChanged(.. CONFIRMED ..)
    // AUTOMATICALLY when the peer's ACK comes in. We therefore no
    // longer need to manually drive the state machine.
    // ============================================================
    if (!inv->invite_tsx) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] inv->invite_tsx is NULL; cannot send 200 OK");
        if (tdata) pjsip_tx_data_dec_ref(tdata);
        pjsip_dlg_dec_lock(dlg);
        pjsip_endpt_release_pool(pjsua_var.endpt, pool);
        return PJ_EINVALIDOP;
    }
    status = pjsip_dlg_send_response(inv->dlg, inv->invite_tsx, tdata);
    if (status != PJ_SUCCESS) {
        char errbuf[256];
        pj_strerror(status, errbuf, sizeof(errbuf));
        VCS_GATEWAY_LOG_ERROR("[Signaling] pjsip_dlg_send_response(200) failed: %s (%d)",
                              errbuf, status);
        if (tdata) pjsip_tx_data_dec_ref(tdata);
        pjsip_dlg_dec_lock(dlg);
        pjsip_endpt_release_pool(pjsua_var.endpt, pool);
        return status;
    }

    VCS_GATEWAY_LOG_INFO("[Signaling] ========== CALL CONNECTED ==========");
    VCS_GATEWAY_LOG_INFO("[Signaling] 200 OK sent with external SDP (backend was already up)");

    // Note: We used to manually call onInviteStateChanged(.., CONFIRMED, ..)
    // here. With pjsip_dlg_send_response the PJSIP state machine will
    // fire it AUTOMATICALLY when the peer's ACK arrives, so we do not
    // call it here. A defensive log line below makes the state-machine
    // transition observable from the log.
    VCS_GATEWAY_LOG_INFO("[Signaling] Waiting for peer's ACK to fire CONFIRMED transition...");

    // Release dialog lock
    pjsip_dlg_dec_lock(dlg);
    // [V7.0 Phase-2 H CRITICAL FIX 2026-08-11] DO NOT release the invite pool here.
    //
    // Why: pjsip_dlg_send_response() is asynchronous. The 200 OK (and its
    // SDP body) is shipped by PJSIP's background worker thread AFTER this
    // function returns. If we free the pool right now, the tdata that was
    // built from pool-allocated memory is left with dangling pointers by
    // the time the worker thread actually walks the message, and the
    // stack traces it as:
    //
    //   pjsip_dlg_send_response(200) failed:
    //     Invalid request (PJ_EINVALIDREQ) (171039)
    //
    // followed by the peer tearing the call down because the 200 OK
    // never reached it (or reached it malformed).
    //
    // The pool is owned by the dialog/invite session: PJSIP will release
    // it when the dialog is destroyed (i.e. on BYE or on full teardown).
    // We therefore intentionally LEAK it from this function's frame and
    // let PJSIP's normal dialog lifetime take care of it. This matches
    // what SipCall.cpp's 200-OK path does in the legacy code.

    return PJ_SUCCESS;
}

pjmedia_sdp_session* SipSignalingHandler::buildExternalSdp(pj_pool_t* pool) {
    std::string localIp;
    uint16_t audioPort, videoPort;
    getMediaInfo(localIp, audioPort, videoPort);
    return buildExternalSdp(pool, localIp, audioPort, videoPort);
}

pjmedia_sdp_session* SipSignalingHandler::buildExternalSdp(pj_pool_t* pool,
                                                            const std::string& localIp,
                                                            uint16_t audioPort,
                                                            uint16_t videoPort,
                                                            const RemoteMediaInfo* remoteInfo) {
    // 端口以入参为准：并发呼叫用的是端口池分配的 per-call 端口，
    // 不一定是全局配置的那一对。
    if (localIp.empty() || audioPort == 0 || videoPort == 0) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Media not configured! "
                              "(ip='%s', audio=%u, video=%u)",
                              localIp.c_str(), audioPort, videoPort);
        return nullptr;
    }

    VCS_GATEWAY_LOG_INFO("[Signaling] Building external SDP: IP=%s, Audio=%u, Video=%u",
                         localIp.c_str(), audioPort, videoPort);

    // Use ExternalSdpBuilder to generate SDP
    pj::SdpSession sdpSession = ExternalSdpBuilder::buildOfferSdp(
        localIp, audioPort, videoPort, remoteInfo);

    pjmedia_sdp_session* sdp = static_cast<pjmedia_sdp_session*>(sdpSession.pjSdpSession);
    if (!sdp) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] ExternalSdpBuilder returned NULL");
        return nullptr;
    }

    VCS_GATEWAY_LOG_INFO("[Signaling] SDP built: media_count=%u", sdp->media_count);
    return sdp;
}

pj_status_t SipSignalingHandler::create200OkWithSdp(pjsip_dialog* dlg, pjsip_rx_data* rdata,
                                                    pjsip_inv_session* inv, pjmedia_sdp_session* sdp,
                                                    pjsip_tx_data** p_tdata) {
    pj_status_t status;

    status = pjsip_dlg_create_response(dlg, rdata, 200, nullptr, p_tdata);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to create 200 response: %d", status);
        return status;
    }

    if (sdp) {
        pjsip_msg_body* body = nullptr;
        status = pjsip_create_sdp_body((*p_tdata)->pool, sdp, &body);
        if (status != PJ_SUCCESS) {
            VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to create SDP body: %d", status);
            return status;
        }
        (*p_tdata)->msg->body = body;
    }

    // RFC 3261 12.1.1: a 2xx to INVITE MUST carry Contact (the peer routes
    // its ACK there). pjsip_dlg_create_response() does not add it.
    AddContactHeader(*p_tdata, rdata);

    VCS_GATEWAY_LOG_INFO("[Signaling] 200 OK response created with SDP");
    return PJ_SUCCESS;
}

pj_status_t SipSignalingHandler::handleReinvite(pjsip_inv_session* inv, pjsip_rx_data* /*rdata*/, pjmedia_sdp_session* /*offer*/) {
    pj_status_t status;
    pjmedia_sdp_session* answer = nullptr;

    VCS_GATEWAY_LOG_INFO("[Signaling] Handling re-INVITE...");

    // Get dialog pool
    pj_pool_t* pool = inv->dlg->pool;

    // If SDP offer callback is registered, call it to get answer
    if (sdpOfferCallback_) {
        answer = sdpOfferCallback_(pool, inv, nullptr);
    } else {
        // re-INVITE 的答案必须继续用这通呼叫自己的端口；全局配置端口
        // 只属于首通呼叫，用它会让对端在 re-INVITE 后把 RTP 发错地方。
        std::string localIp;
        uint16_t audioPort, videoPort;
        getMediaInfo(localIp, audioPort, videoPort);
        uint16_t perCallAudio = 0, perCallVideo = 0;
        if (GetPerCallLocalPorts(inv, perCallAudio, perCallVideo)) {
            audioPort = perCallAudio;
            videoPort = perCallVideo;
        }
        answer = buildExternalSdp(pool, localIp, audioPort, videoPort);
    }

    if (!answer) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] Failed to build re-INVITE answer SDP");
        return PJ_EINVALIDOP;
    }

    status = pjsip_inv_answer(inv, 200, nullptr, answer, nullptr);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[Signaling] pjsip_inv_answer(200) for re-INVITE failed: %d", status);
        return status;
    }

    VCS_GATEWAY_LOG_INFO("[Signaling] Re-INVITE answered with 200 OK (auto-sent)");
    return PJ_SUCCESS;
}

void SipSignalingHandler::onInviteStateChanged(pjsip_inv_session* inv, int old_state, int new_state) {
    VCS_GATEWAY_LOG_INFO("[Signaling] Invite state changed: %d -> %d", old_state, new_state);

    // Detect if entering CONFIRMED state
    if (new_state == PJSIP_INV_STATE_CONFIRMED) {
        VCS_GATEWAY_LOG_INFO("[Signaling] ========== CALL CONFIRMED ==========");

        RemoteMediaInfo remoteInfo;
        // [V5.5 Fix] Cast inv to uintptr_t for map lookup
        {
            std::lock_guard<std::mutex> lock(remoteInfoMutex_);
            auto it = remoteMediaInfos_.find(reinterpret_cast<uintptr_t>(inv));
            if (it != remoteMediaInfos_.end()) {
                remoteInfo = it->second;
                VCS_GATEWAY_LOG_INFO("[Signaling] Remote media: addr=%s, audio=%u, video=%u",
                                     remoteInfo.address.c_str(), remoteInfo.audioPort, remoteInfo.videoPort);
                if (remoteInfo.videoPort > 0) {
                    // [docs/11 §3.2] Codec evidence for inbound (INVITE-intercepted)
                    // calls; never print the base64 payload itself.
                    const std::string spropDesc = remoteInfo.h264SpropParameterSets.empty()
                        ? std::string("absent")
                        : ("present(" + std::to_string(remoteInfo.h264SpropParameterSets.size()) +
                           " chars)");
                    VCS_GATEWAY_LOG_INFO("[SDP] video: H264 pt=%u, sprop=%s, profile=%s, "
                                         "packetization-mode=%d",
                                         (unsigned)remoteInfo.videoPayloadType,
                                         spropDesc.c_str(),
                                         remoteInfo.h264ProfileLevelId.empty()
                                             ? "(none)"
                                             : remoteInfo.h264ProfileLevelId.c_str(),
                                         remoteInfo.h264PacketizationMode ? 1 : 0);
                }
            }
        }

        // [V7.0 Phase-2 G] NOTE: establishedCallback_ is NO LONGER invoked
        // here. The legacy code did so AFTER sending 200 OK, which was the
        // root cause of the join-flow race (peer began streaming RTP before
        // the MCU had any receiver state). handleInvite now invokes
        // establishedCallback_ BEFORE sending 200 OK, so by the time we
        // reach this point the backend is already up.
        //
        // docs/07 decision C: CallConnected = 信令接通. This CONFIRMED
        // callback is the authoritative signaling-connected point for
        // external (INVITE-intercepted) calls, so MarkConnected is invoked
        // here; it is idempotent (re-INVITE re-CONFIRMs are no-ops).
        {
            std::string inviteSessionId;
            {
                std::lock_guard<std::mutex> iLock(invitesMutex_);
                auto iIt = activeInvites_.find(reinterpret_cast<uintptr_t>(inv));
                if (iIt != activeInvites_.end()) {
                    inviteSessionId = iIt->second.sessionId;
                    if (iIt->second.backendSetupDone) {
                        VCS_GATEWAY_LOG_INFO("[Signaling] Call CONFIRMED with backend setup %s "
                                             "(inv=%p, sessionId='%s')",
                                             iIt->second.backendSetupOk ? "OK" : "FAILED",
                                             (void*)inv,
                                             inviteSessionId.c_str());
                    } else {
                        VCS_GATEWAY_LOG_WARNING(
                            "[Signaling] Call CONFIRMED but backendSetupDone=false; "
                            "establishedCallback_ was not invoked. This is unexpected "
                            "for INVITE-driven calls and may indicate a re-INVITE path. "
                            "(inv=%p, sessionId='%s')",
                            (void*)inv,
                                            inviteSessionId.c_str());
                    }
                }
            }

            if (!inviteSessionId.empty() && GetSessionManager()) {
                ConnectedInfo connectedInfo;
                connectedInfo.remoteAddress = remoteInfo.address;
                connectedInfo.hasVideo = remoteInfo.videoPort > 0;

                // Inbound: we are the answerer, so the selected codec lives in
                // OUR active local SDP (the answer), not in the remote offer's
                // fmt order.
                const pjmedia_sdp_session* activeLocal = nullptr;
                if (inv->neg &&
                    pjmedia_sdp_neg_get_active_local(inv->neg, &activeLocal) == PJ_SUCCESS &&
                    activeLocal) {
                    sdp::ParseNegotiatedCodecs(activeLocal, connectedInfo.audioCodec,
                                               connectedInfo.videoCodec);
                }
                GetSessionManager()->MarkConnected(inviteSessionId, connectedInfo);
            }
        }
    }

    // Detect if entering DISCONNECTED state
    if (new_state == PJSIP_INV_STATE_DISCONNECTED) {
        VCS_GATEWAY_LOG_INFO("[Signaling] Invite disconnected: inv=%p", (void*)inv);

        // [V5.5 Critical Fix] Get session info before cleanup
        // We need sessionId and conferenceId to call HandleExternalBye
        std::string sessionId;
        std::string conferenceId;
        uint16_t    releasedAudioPort = 0;
        uint16_t    releasedVideoPort = 0;
        bool        releasePorts      = false;
        {
            std::lock_guard<std::mutex> lock(invitesMutex_);
            auto it = activeInvites_.find(reinterpret_cast<uintptr_t>(inv));
            if (it != activeInvites_.end()) {
                sessionId        = it->second.sessionId;
                conferenceId     = it->second.conferenceId;
                releasePorts     = it->second.portsAllocatedFromPool;
                releasedAudioPort = it->second.allocatedAudioPort;
                releasedVideoPort = it->second.allocatedVideoPort;
                VCS_GATEWAY_LOG_INFO(
                    "[Signaling] Found session info: sessionId=%s, confId=%s, "
                    "ports=(audio=%u, video=%u, release=%d)",
                    sessionId.c_str(), conferenceId.c_str(),
                    releasedAudioPort, releasedVideoPort, (int)releasePorts);
            }
        }

        // [V7.1 Fix 2026-08-13] Return per-call RTP ports to the allocator
        // so the next INVITE can reuse them. Done BEFORE the activeInvites_
        // erase so we can read the ports from the map.
        if (releasePorts && releasedAudioPort != 0 && releasedVideoPort != 0) {
            portAllocator_.Release(releasedAudioPort, releasedVideoPort);
            VCS_GATEWAY_LOG_INFO(
                "[V7.1] Released per-call ports for inv=%p on DISCONNECTED: audio=%u, video=%u",
                (void*)inv, releasedAudioPort, releasedVideoPort);
        }

        // [V5.5 Critical Fix] Cleanup activeInvites_ when session is disconnected
        // This prevents crashes from dangling inv pointers when iterating over activeInvites_
        // [V5.5 Fix] Use uintptr_t key
        {
            std::lock_guard<std::mutex> lock(invitesMutex_);
            auto it = activeInvites_.find(reinterpret_cast<uintptr_t>(inv));
            if (it != activeInvites_.end()) {
                VCS_GATEWAY_LOG_INFO("[Signaling] Removing disconnected inv from activeInvites_: inv=%p", (void*)inv);
                activeInvites_.erase(it);
            }
        }

        // [V7.0 Fix] Also remove from callIdToInvite_ index
        {
            std::lock_guard<std::mutex> lock(callIdMutex_);
            for (auto it = callIdToInvite_.begin(); it != callIdToInvite_.end(); ) {
                if (it->second == reinterpret_cast<uintptr_t>(inv)) {
                    it = callIdToInvite_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Cleanup remoteMediaInfos_
        {
            std::lock_guard<std::mutex> lock(remoteInfoMutex_);
            remoteMediaInfos_.erase(reinterpret_cast<uintptr_t>(inv));
        }

        // [V5.5 Critical Fix] Call HandleExternalBye to notify conference layer
        // This ensures proper cleanup of RTP transport and conference member
        if (!sessionId.empty()) {
            VCS_GATEWAY_LOG_INFO("[Signaling] Calling HandleExternalBye: sessionId=%s, confId=%s",
                               sessionId.c_str(), conferenceId.c_str());
            sipAdapter_.HandleExternalBye(sessionId, conferenceId, "Session disconnected");
        }
    }
}

vcs::Response SipSignalingHandler::SendDTMF(int callId, const std::string& digits) {
	SipCall* call = GetActiveCall(callId);
	if (!call) {
		return vcs::Response::Error(vcs::ResponseCode::NotFound, "Call not found");
	}
	return call->SendDTMF(digits)
		? vcs::Response::Success("DTMF sent")
		: vcs::Response::Error(vcs::ResponseCode::InternalError, "SendDTMF failed");
}

// PJSIP end

}} // namespace vcs::gateway
