#include "SipAdapter.h"
#include "SipSignalingHandler.h"
#include "SipMediaTransport.h"
#include "SimpleRtpTransport.h"
#include "SessionManager.h"  // [V5.6] Needed for method calls on sessionManager_ pointer
#include "ILogger.h"
#include "SipCall.h"
#include "TimeUtils.h"
#include "Optional.h"
#include <thread>
#include <chrono>

// PJSIP - External SDP mode (media handled externally)
#include <pjsip.h>
#include <pjsua2.hpp>
#include <pjsua2/media.hpp>
#include <pjmedia.h>
#include <pjmedia/master_port.h>
#include <pjmedia/null_port.h>
#include <pjsua-lib/pjsua.h>
#include <pjsua-lib/pjsua_internal.h>   // pjsua_var.log_cfg for runtime re-config

namespace vcs { namespace gateway {

namespace {

// Read one "key=value" entry from an SDP a=fmtp value (entries separated by ';').
std::string FmtpParam(const std::string& fmtp, const std::string& key) {
    size_t pos = 0;
    while (pos <= fmtp.size()) {
        size_t end = fmtp.find(';', pos);
        if (end == std::string::npos) end = fmtp.size();
        std::string token = fmtp.substr(pos, end - pos);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.erase(token.begin());
        }
        const size_t eq = token.find('=');
        if (eq != std::string::npos && token.compare(0, eq, key) == 0) {
            return token.substr(eq + 1);
        }
        if (end == fmtp.size()) break;
        pos = end + 1;
    }
    return std::string();
}

// One INFO line per call. Never dumps base64 parameter sets or the raw SDP.
void LogNegotiatedCodec(const char* kind, const vcs::NegotiatedCodec& codec) {
    if (codec.codecName.empty()) {
        VCS_GATEWAY_LOG_INFO("[SDP] %s: no negotiated codec", kind);
        return;
    }
    if (std::string(kind) == "video") {
        const std::string sprop = FmtpParam(codec.fmtp, "sprop-parameter-sets");
        const std::string profile = FmtpParam(codec.fmtp, "profile-level-id");
        const std::string pmode = FmtpParam(codec.fmtp, "packetization-mode");
        const std::string spropDesc = sprop.empty()
            ? std::string("absent")
            : ("present(" + std::to_string(sprop.size()) + " chars)");
        VCS_GATEWAY_LOG_INFO("[SDP] video: %s pt=%u clock=%u, sprop=%s, profile=%s, "
            "packetization-mode=%s",
            codec.codecName.c_str(), (unsigned)codec.payloadType, codec.clockRate,
            spropDesc.c_str(),
            profile.empty() ? "(none)" : profile.c_str(),
            pmode.empty() ? "(none)" : pmode.c_str());
    }
    else {
        VCS_GATEWAY_LOG_INFO("[SDP] audio: %s pt=%u clock=%u",
            codec.codecName.c_str(), (unsigned)codec.payloadType, codec.clockRate);
    }
}

} // anonymous namespace

// ===================================================================
// SipAdapter Implementation
// ===================================================================

SipAdapter::SipAdapter(const std::string& listenAddress, uint16_t port,
                       const PjsipLogSettings& logSettings)
    : type_("SIP")
    , listenAddress_(listenAddress)
    , port_(port)
    , running_(false)
    , callback_(nullptr)
    , videoConfigValid_(false)
    , logSettings_(logSettings) {
    logSettings_.level = ClampPjsipLogLevel(logSettings_.level);

    // [V5.6] SessionManager is injected by GatewayManager via SetSessionManager().
    // We no longer create a SessionManager instance here. This allows the same
    // SessionManager to be shared across SIP, H.323, and WebRTC adapters.

    // Construct local SIP URI
    std::string localUri = "sip:" + listenAddress + ":" + std::to_string(port);

// PJSIP always enabled
    // ============================================================
    // Create PJSIP Endpoint
    // ============================================================
    VCS_GATEWAY_LOG_INFO("[SIP] ============================================");
    VCS_GATEWAY_LOG_INFO("[SIP] Creating PJSIP Endpoint");
    VCS_GATEWAY_LOG_INFO("[SIP] ============================================");



    pjsipEndpoint_.reset(new pj::Endpoint());
    if (!pjsipEndpoint_) {
        VCS_GATEWAY_LOG_ERROR("[SIP] Failed to create pj::Endpoint");
        return;
    }

    // Configuration
    pj::EpConfig epConfig;

    // pjsip 日志独立落盘到自己的文件（默认 siplog.log），不再汇入 Gateway.log。
    // 级别来自上层配置（gateway.sip.pjsip_log_level，-1 时跟随 log.level）。
    // consoleLevel=0 表示不再把同一条日志转给 console / 其它 writer，
    // 保证 pjsip 只写文件。
    epConfig.logConfig.level = logSettings_.level;
    epConfig.logConfig.consoleLevel = 0;
    epConfig.logConfig.msgLogging = logSettings_.msgLogging ? 1 : 0;
    // 独立文件需要可读的格式：时间 + 级别 + 换行（decor=0 时整份日志会连成一行）。
    epConfig.logConfig.decor =
        PJ_LOG_HAS_TIME | PJ_LOG_HAS_LEVEL_TEXT | PJ_LOG_HAS_NEWLINE;
    epConfig.logConfig.filename = logSettings_.logFile;
    epConfig.logConfig.fileFlags = PJ_O_APPEND;  // 追加写，重启不清空

    VCS_GATEWAY_LOG_INFO("[SIP] pjsip log: level=%d (0=off..6=trace), msgLogging=%d, file=%s",
                         logSettings_.level, logSettings_.msgLogging ? 1 : 0,
                         logSettings_.logFile.c_str());

    epConfig.medConfig.quality = 8;
    epConfig.medConfig.audioFramePtime = 20;  // 20ms for G.722
    epConfig.medConfig.maxMediaPorts = 100;
    epConfig.medConfig.noVad = true;
    epConfig.medConfig.threadCnt = 1;
    epConfig.medConfig.hasIoqueue = true;
    epConfig.medConfig.sndRecLatency = 60;
    epConfig.medConfig.sndPlayLatency = 60;

    // ============================================================
    // PJSIP 2.17 Video Configuration Notes:
    // - Video codecs are managed by pjsua_vid_codec_* series functions
    // - No need to explicitly set vidApiUse, already enabled by default
    // - PJSIP includes H264/VP8/VP9 codecs built-in
    // - Video negotiation triggered by CallOpParam.opt.videoCount=1
    //
    // Video capability enablement:
    // Use pjsua_vid_codec_set_priority() after libInit() to enable video codecs.
    // This makes PJSIP declare video capability in SDP answer (m=video <port> instead of m=video 0).
    // ============================================================

    VCS_GATEWAY_LOG_INFO("[SIP] Video support configured (video codecs will be enabled after libInit)");

    // ============================================================
    // SRTP Configuration Notes:
    // - SRTP is configured in AccountConfig.mediaConfig.srtpUse (see SipAccount.cpp)
    // - No SRTP field in EpConfig.medConfig
    // - Account-level SRTP configuration ensures a=crypto line in SDP
    // ============================================================
    VCS_GATEWAY_LOG_INFO("[SIP] SRTP configured in AccountConfig");

    try {
        // pjsua_create() runs before libInit() installs our writer, so pjlib's
        // default console writer would print startup banners regardless of
        // LogLevel. Open the gate wide first (or shut it when logging is off);
        // pjsua_init() re-applies epConfig.logConfig.level once our writer is
        // installed.
        pj_log_set_level(logSettings_.level > 0 ? 5 : 0);

        VCS_GATEWAY_LOG_DEBUG("[SIP] Calling libCreate()...");
        pjsipEndpoint_->libCreate();
        VCS_GATEWAY_LOG_INFO("[SIP] libCreate() OK");

		
        VCS_GATEWAY_LOG_DEBUG("[SIP] Calling libInit()...");
        // 不安装 LogWriter：pjsip 日志只由 pjsua 自己写入 logConfig.filename。
        pjsipEndpoint_->libInit(epConfig);
        VCS_GATEWAY_LOG_INFO("[SIP] libInit() OK");

		try {
			pjsipEndpoint_->vidDevManager().refreshDevs();
			if (pjsipEndpoint_->vidDevManager().getDevCount() > 0) {
				VCS_GATEWAY_LOG_INFO("[SIP] Video devices found: %u", pjsipEndpoint_->vidDevManager().getDevCount());
			}
			else {
				VCS_GATEWAY_LOG_WARNING("[SIP] No video devices found!");
			}
		}
		catch (const pj::Error& err) {
			VCS_GATEWAY_LOG_ERROR("[SIP] Failed to refresh video devs: %s", err.info().c_str());
		}

        // ============================================================
        // Video Codec Configuration Notes:
        //
        // [IMPORTANT] No need to call pjsua_vid_enum_codecs() or pjsua_vid_codec_set_priority()
        // PJSIP internally considers no video encoder available (since video device not enabled).
        // Video capability will be declared directly in SDP in onCallSdpCreated() callback.
        //
        // Video capability declaration is fully handled at SDP level:
        // - Change m=video line port from 0 to non-zero (e.g., 4002)
        // - Add rtpmap, fmtp and other attributes
        // - Set sendonly direction
        // - External RTP handling module takes over actual video sending/receiving
        // ============================================================

        VCS_GATEWAY_LOG_INFO("[SIP] Video capability declared via SDP manipulation in onCallSdpCreated()");

        // ============================================================
        // List all codecs and their priorities (for debugging)
        // Note: pjsua_enum_codecs only lists audio codecs.
        // Video codecs need pjsua_vid_enum_codecs to enumerate.
        // ============================================================

        // List all audio codecs and their priorities
        pjsua_codec_info all_codec_list[48];
        unsigned all_count = 48;
        pj_status_t all_status = pjsua_enum_codecs(all_codec_list, &all_count);
        if (all_status == PJ_SUCCESS) {
            VCS_GATEWAY_LOG_INFO("[SIP] Registered audio codecs:");
            for (unsigned i = 0; i < all_count; i++) {
                char buf[128];
                pj_str_t tmp = all_codec_list[i].codec_id;
                snprintf(buf, sizeof(buf), "%.*s", (int)tmp.slen, tmp.ptr);
                if (all_codec_list[i].priority > 0 && buf != NULL) {
                    VCS_GATEWAY_LOG_INFO("[SIP]   %s (priority=%d)", buf, all_codec_list[i].priority);
                }
            }
        }

        // ============================================================
        // [KEY] Enumerate and enable video codecs
        //
        // PJSIP includes H264, VP8, VP9 and other video codecs built-in.
        // Use pjsua_vid_enum_codecs() to enumerate and enable them.
        // This makes PJSIP declare video capability during SDP negotiation.
        // ============================================================

        // Enumerate video codecs (using pjsua_codec_info type)
        pjsua_codec_info vid_codec_list[16];
        unsigned vid_count = 16;
        pj_status_t vid_status = pjsua_vid_enum_codecs(vid_codec_list, &vid_count);
        if (vid_status == PJ_SUCCESS) {
            VCS_GATEWAY_LOG_INFO("[SIP] Registered video codecs:");
            for (unsigned i = 0; i < vid_count; i++) {
                char buf[128];
                pj_str_t tmp = vid_codec_list[i].codec_id;
                snprintf(buf, sizeof(buf), "%.*s", (int)tmp.slen, tmp.ptr);
                VCS_GATEWAY_LOG_INFO("[SIP]   Video codec: %s", buf);

                // [KEY] Enable all video codecs
                pj_str_t codec_id = vid_codec_list[i].codec_id;
                pj_status_t set_status = pjsua_vid_codec_set_priority(&codec_id, 255);  // 255 = highest priority
                if (set_status == PJ_SUCCESS) {
                    VCS_GATEWAY_LOG_INFO("[SIP]   Enabled video codec: %s", buf);
                } else {
                    VCS_GATEWAY_LOG_WARNING("[SIP]   Failed to enable video codec: %s", buf);
                }
            }
        } else {
            VCS_GATEWAY_LOG_WARNING("[SIP] No video codecs found (vid_status=%d)", vid_status);
        }

        // ============================================================
        // PJSIP 2.17 built-in codecs are enabled by default.
        // PJSIP includes: PCMU, PCMA, G722, H264, VP8, etc.
        // Codec parameters are configured by Conference module via ConfigureVideoCodec().
        // Video negotiation is triggered by CallOpParam.opt.videoCount=1.
        // ============================================================

        VCS_GATEWAY_LOG_INFO("[SIP] Audio and video codecs configured");

        // Create UDP transport
        pj::TransportConfig transportConfig;
        transportConfig.port = (unsigned)port;
        pjsipEndpoint_->transportCreate(PJSIP_TRANSPORT_UDP, transportConfig);
        VCS_GATEWAY_LOG_INFO("[SIP] UDP transport on port %d", port);

        // Create TCP transport (for compatibility with SIP clients that don't support UDP)
        pj::TransportConfig tcpConfig;
        tcpConfig.port = (unsigned)port;  // Shared port with UDP
        pjsipEndpoint_->transportCreate(PJSIP_TRANSPORT_TCP, tcpConfig);
        VCS_GATEWAY_LOG_INFO("[SIP] TCP transport on port %d", port);

		VCS_GATEWAY_LOG_INFO("[SIP] Real device mode enabled (camera/mic available)");

        // ============================================================
        // [Important Note] External SDP mode does not require enabling PJSIP video codecs.
        //
        // Current approach: PJSIP only handles SIP signaling, no media processing.
        // - SDP is fully generated by ExternalSdpBuilder
        // - RTP is transmitted externally by SimpleRtpTransport
        // - PJSIP built-in video codecs are not needed at all
        // ============================================================
        VCS_GATEWAY_LOG_INFO("[SIP] External SDP mode - using SipSignalingHandler for INVITE interception");

        VCS_GATEWAY_LOG_INFO("[SIP] ============================================");

    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("[SIP] PJSIP init failed: %s", err.info().c_str());
        pjsipEndpoint_.reset();
        return;
    }

    // Initialize SipSignalingHandler, inject pjsipEndpoint_
    VCS_GATEWAY_LOG_DEBUG("[SIP] Creating SipSignalingHandler...");
    signalingHandler_.reset(new SipSignalingHandler(localUri, port, *this));
    VCS_GATEWAY_LOG_DEBUG("[SIP] SipSignalingHandler created: %p", (void*)signalingHandler_.get());

    // Set signaling handler callback for synchronizing session mapping
    signalingHandler_->SetCallback(this);

    // [V5.6] If SetSessionManager() was already called before signalingHandler_ was created,
    // forward the SessionManager to the newly created SipSignalingHandler now.
    if (sessionManager_) {
        signalingHandler_->SetSessionManager(sessionManager_);
        VCS_GATEWAY_LOG_INFO("[SIP] SessionManager %p forwarded to SipSignalingHandler after construction",
                             (void*)sessionManager_);
    }

    // ============================================================
    // [CORE] Initialize INVITE interception module
    //
    // SipSignalingHandler intercepts INVITE before PJSUA,
    // using low-level pjsip_inv_session API to bypass PJSUA media subsystem,
    // implementing true separation of signaling and media.
    // Note: Must be called after signalingHandler_ is created!
    // ============================================================

    // Debug: confirm signalingHandler_ is already created
    VCS_GATEWAY_LOG_DEBUG("[SIP] Before initModule, signalingHandler_=%p",
                          (void*)signalingHandler_.get());

    VCS_GATEWAY_LOG_DEBUG("[SIP] Calling initModule()...");
    if (!signalingHandler_->initModule()) {
        VCS_GATEWAY_LOG_ERROR("[SIP] Failed to register INVITE interception module");
    } else {
        VCS_GATEWAY_LOG_INFO("[SIP] INVITE interception module registered successfully");

        // If media ports are already configured, set them to the module
        if (!localMediaAddress_.empty() && localAudioPort_ > 0 && localVideoPort_ > 0) {
            signalingHandler_->SetLocalMediaAddress(localMediaAddress_);
            signalingHandler_->SetLocalMediaPorts(localAudioPort_, localVideoPort_);
        }

        // App-managed media: SDP is created/validated by the application layer.
        if (sdpNegotiator_) {
            signalingHandler_->SetMediaSdpNegotiator(sdpNegotiator_);
            VCS_GATEWAY_LOG_INFO("[SIP] SDP negotiator forwarded to SipSignalingHandler");
        }

        // [CORE] Register call established callback, create external RTP transport when INVITE enters CONFIRMED state
        // [V7.0 Phase-2 G] The callback now returns bool: the SIP signaling handler
        // will only forward 200 OK to the peer after this returns true. Returning
        // false causes a 5xx error response, so we MUST return true only when the
        // entire backend (transport, CallSession, conference join) is wired up.
        signalingHandler_->setCallEstablishedCallback(
            [this](pjsip_inv_session* inv, const RemoteMediaInfo& remoteInfo) -> bool {
                return this->onExternalCallEstablished(inv, remoteInfo);
            });
        VCS_GATEWAY_LOG_INFO("[SIP] CallEstablishedCallback registered");

        // [Key Fix] Register conference validation callback, validate conference before sending 200 OK.
        // Ensure: Conference validation passed -> send 200 OK + set media channel.
        //         Conference validation failed -> send 4xx rejection.
        signalingHandler_->setConferenceValidationCallback(
            [this](const std::string& conferenceId) -> bool {
                VCS_GATEWAY_LOG_INFO("[SIP] ====== ValidationCallback START ======");
                VCS_GATEWAY_LOG_INFO("[SIP] ValidationCallback: conferenceId='%s'", conferenceId.c_str());
                return true;
            });
        VCS_GATEWAY_LOG_INFO("[SIP] ConferenceValidationCallback registered");
    }
}

SipAdapter::~SipAdapter() {
    // Stop all sessions and PJSIP first
    // Stop() already called libDestroy(), so no need to call again in destructor
    if (running_) {
        Stop();
    } else {
        // Even if not running, PJSIP cleanup may still be needed
        // (if Start() failed but libCreate() succeeded)
// PJSIP always enabled
        if (pjsipEndpoint_) {
            try {
                pjsipEndpoint_->hangupAllCalls();
                pjsipEndpoint_->libStopWorkerThreads();
                pjsipEndpoint_->libDestroy();
            } catch (...) {
                // Ignore shutdown errors
            }
            pjsipEndpoint_.reset();
        }
    }

// PJSIP always enabled
    // Cleanup signalingHandler
    signalingHandler_.reset();
    VCS_GATEWAY_LOG_INFO("[SIP] SignalingHandler destroyed");
}

// SipAdapter.cpp

vcs::Response SipAdapter::Start() {
	if (running_) {
		return vcs::Response::Error(vcs::ResponseCode::Conflict, "SIP adapter already running");
	}

	if (!pjsipEndpoint_) {
		return vcs::Response::Error(vcs::ResponseCode::InternalError, "PJSIP Endpoint not initialized");
	}

	// ============================================================
	// 1. 真正启动 PJSIP 底层线程和网络监听
	// ============================================================
	try {
		VCS_GATEWAY_LOG_INFO("[SIP] Calling libStart()...");
		pjsipEndpoint_->libStart();
		VCS_GATEWAY_LOG_INFO("[SIP] libStart() OK");
	}
	catch (const pj::Error& err) {
		VCS_GATEWAY_LOG_ERROR("[SIP] libStart failed: %s", err.info().c_str());
		return vcs::Response::Error(vcs::ResponseCode::InternalError, "libStart failed");
	}

	// ============================================================
	// 2. 启动信令处理器的状态机
	// ============================================================
	if (signalingHandler_) {
		if (!signalingHandler_->Start()) {
			VCS_GATEWAY_LOG_ERROR("[SIP] Failed to start SipSignalingHandler");
			pjsipEndpoint_->libStopWorkerThreads();
			return vcs::Response::Error(vcs::ResponseCode::InternalError, "Failed to start SIP signaling");
		}
	}

	// 3. 标记 Adapter 已运行
	running_ = true;
	VCS_GATEWAY_LOG_INFO("[SIP] SIP Adapter started successfully");

	return vcs::Response::Success("SIP adapter started");
}

vcs::Response SipAdapter::Stop() {
	if (!running_) {
		return vcs::Response::Success("SIP adapter already stopped");
	}

	VCS_GATEWAY_LOG_INFO("[SIP] Stopping SIP Adapter...");
	running_ = false;

	// 1. 停止信令处理器（停止接收新的会话事件，清理内部状态）
	if (signalingHandler_) {
		// 1a. [关键] 在 libDestroy 之前，先停所有 SipMediaTransport 的发送线程
		auto activeCallIds = signalingHandler_->GetActiveCallIds();
		for (int callId : activeCallIds) {
			// Shared-ptr lookup: the raw CallSession* had no lifetime guard,
			// and shutdown runs concurrently with the event thread.
			auto session = sessionManager_ ? sessionManager_->GetSessionByCallId(callId) : nullptr;
			if (!session) continue;
			ShutdownSessionTransports(session->sessionId, "Stop");
		}

		signalingHandler_->Stop();
		VCS_GATEWAY_LOG_INFO("[SIP] Signaling handler stopped");
	}

	// 2. 严谨的 PJSIP 资源回收流程
	if (pjsipEndpoint_) {
		try {
			// 2.1 挂断所有正在进行的呼叫
			pjsipEndpoint_->hangupAllCalls();
			VCS_GATEWAY_LOG_INFO("[SIP] All active calls hung up");

			// 2.2 停止 PJSIP 的工作线程（等待线程退出）
			pjsipEndpoint_->libStopWorkerThreads();
			VCS_GATEWAY_LOG_INFO("[SIP] PJSIP worker threads stopped");

			// 2.3 销毁 PJSIP 库（释放底层 socket、内存池等所有资源）
			pjsipEndpoint_->libDestroy();
			VCS_GATEWAY_LOG_INFO("[SIP] PJSIP library destroyed");
		}
		catch (const pj::Error& err) {
			VCS_GATEWAY_LOG_ERROR("[SIP] Exception during PJSIP shutdown: %s", err.info().c_str());
		}
		catch (...) {
			VCS_GATEWAY_LOG_ERROR("[SIP] Unknown exception during PJSIP shutdown");
		}

		// 2.4 重置指针，防止析构函数中重复调用 libDestroy() 导致崩溃
		pjsipEndpoint_.reset();
	}

	VCS_GATEWAY_LOG_INFO("[SIP] SIP Adapter stopped successfully");
	return vcs::Response::Success("SIP adapter stopped");
}

bool SipAdapter::IsRunning() const {
    return running_;
}

void SipAdapter::SetEventBus(std::shared_ptr<vcs::IEventBus> bus) {
    eventBus_ = bus;
}

std::shared_ptr<vcs::IEventBus> SipAdapter::GetEventBus() const {
    return eventBus_;
}

void SipAdapter::SetCallback(IProtocolAdapterCallback* callback) {
    callback_ = callback;
}

IProtocolAdapterCallback* SipAdapter::GetCallback() const {
    return callback_;
}

// [V5.6] Inject the shared SessionManager (owned by GatewayManager).
// This must be called before Start(). We forward the pointer to the
// internal SipSignalingHandler so that all session CRUD operations
// use the same shared SessionManager instance.
void SipAdapter::SetSessionManager(SessionManager* sm) {
    sessionManager_ = sm;
    if (signalingHandler_) {
        signalingHandler_->SetSessionManager(sm);
        VCS_GATEWAY_LOG_INFO("[SIP] SetSessionManager: SessionManager %p injected into SipSignalingHandler",
                             (void*)sm);
    } else {
        VCS_GATEWAY_LOG_INFO("[SIP] SetSessionManager: stored SessionManager %p (signalingHandler_ not yet created)",
                             (void*)sm);
    }
}

ISignalingHandler* SipAdapter::GetSignalingHandler() {
    return signalingHandler_.get();
}

void SipAdapter::SetServer(const std::string& server,
                            const std::string& username,
                            const std::string& password) {
    signalingHandler_->SetServer(server, username, password);
}

// ===================================================================
// PJSIP Access Methods
// ===================================================================

void* SipAdapter::GetPjsipEndpoint() const {
    return pjsipEndpoint_.get();
}

pj::Endpoint& SipAdapter::GetPjsipEndpointRef() const {
    if (!pjsipEndpoint_) {
        throw std::runtime_error("PJSIP not initialized");
    }
    return *pjsipEndpoint_;
}

// ===================================================================
// Session Query
// ===================================================================

std::vector<std::string> SipAdapter::GetActiveSessionIds() const {
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

CallSessionInfo* SipAdapter::GetSessionInfo(const std::string& sessionId) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session) {
        return nullptr;
    }

    static CallSessionInfo info;
    info.sessionId = session->sessionId;
    info.caller = session->caller;
    info.callee = session->callee;
    info.conferenceId = session->conferenceId;
    info.protocol = "SIP";
    info.state = session->state;
    info.startTime = session->startTime;
    info.connectTime = session->connectTime;

    return &info;
}

size_t SipAdapter::GetActiveSessionCount() const {
    return signalingHandler_->GetActiveSessionCount();
}

// ===================================================================
// Call Control
// ===================================================================

vcs::Response SipAdapter::InitiateCall(const std::string& callee,
                                       const Params& params,
                                       std::string& sessionId) {
    if (!running_) {
        return vcs::Response::Error(vcs::ResponseCode::InternalError, "Adapter not running");
    }

    // Initiate call through SignalingHandler
    std::string newSessionId;
    vcs::Response resp = signalingHandler_->InitiateCall(callee, params, newSessionId);
    if (!resp.IsSuccess()) {
        return resp;
    }

    sessionId = newSessionId;
    notifySessionCountChanged();

    return vcs::Response::Success("Call initiated");
}

vcs::Response SipAdapter::AcceptCall(const std::string& sessionId, const Params& params) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId < 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    return signalingHandler_->AcceptCall(session->callId, params);
}

vcs::Response SipAdapter::RejectCall(const std::string& sessionId, const std::string& reason) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId < 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    vcs::Response resp = signalingHandler_->RejectCall(session->callId, reason);
    if (resp.IsSuccess()) {
        notifySessionCountChanged();
    }

    return resp;
}

vcs::Response SipAdapter::TerminateCall(const std::string& sessionId, const std::string& reason) {
	VCS_GATEWAY_LOG_INFO("TerminateCall: sessionId=%s, reason=%s", sessionId.c_str(), reason.c_str());

	auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
	if (!session || session->callId < 0) {
		VCS_GATEWAY_LOG_ERROR("TerminateCall: session not found, sessionId=%s", sessionId.c_str());
		return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
	}

	// [关键修复] pjsua_call_hangup 会同步销毁 media transport。
	// 必须在它之前停掉 SipMediaTransport 的发送线程与栈侧收包回调。
	ShutdownSessionTransports(sessionId, "TerminateCall");

	vcs::Response resp = signalingHandler_->TerminateCall(session->callId, reason);
	if (resp.IsSuccess()) {
		notifySessionCountChanged();
		VCS_GATEWAY_LOG_INFO("TerminateCall: success, session removed");
	}
	else {
		VCS_GATEWAY_LOG_ERROR("TerminateCall: failed, resp=%s", resp.GetMsg().c_str());
	}

	return resp;
}

vcs::Response SipAdapter::TerminateAllCalls() {
    auto activeCallIds = signalingHandler_->GetActiveCallIds();

    for (int callId : activeCallIds) {
        auto session = sessionManager_ ? sessionManager_->GetSessionByCallId(callId) : nullptr;
        if (session) {
            TerminateCall(session->sessionId, "Terminated by adapter");
        }
    }

    notifySessionCountChanged();

    return vcs::Response::Success("All calls terminated");
}

vcs::Response SipAdapter::SendDTMF(const std::string& sessionId, const std::string& digits) {
    if (digits.empty()) {
        return vcs::Response::Error(vcs::ResponseCode::InvalidParams, "Empty DTMF digits");
    }

    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId < 0) {
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }
    const int callId = session->callId;

    pj_str_t digitStr;
    digitStr.ptr = const_cast<char*>(digits.c_str());
    digitStr.slen = static_cast<pj_ssize_t>(digits.size());

    pj_status_t status = pjsua_call_dial_dtmf(callId, &digitStr);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[SIP] SendDTMF failed: sessionId=%s, digits=%s, status=%d",
                              sessionId.c_str(), digits.c_str(), status);
        return vcs::Response::Error(vcs::ResponseCode::InternalError,
                                    "pjsua_call_dial_dtmf failed");
    }

    VCS_GATEWAY_LOG_INFO("[SIP] SendDTMF: sessionId=%s, digits=%s", sessionId.c_str(), digits.c_str());
    return vcs::Response::Success();
}



// ===================================================================
// Inbound Call Handling
// ===================================================================

void SipAdapter::HandleIncomingCall(const std::string& sessionId,
                                   const std::string& caller,
                                   const Params& params) {
    (void)sessionId;
    (void)caller;
    (void)params;
    // Inbound calls are handled by SignalingHandler callbacks
    // This method is for adapter-level processing
}

// ===================================================================
// SIP-specific Methods
// ===================================================================

vcs::Response SipAdapter::HandleInvite(const std::string& sessionId, const Params& params) {
    std::string caller;
    auto optCaller = params.GetString("caller");
    if (optCaller) {
        caller = *optCaller;
    }

    HandleIncomingCall(sessionId, caller, params);
    return vcs::Response::Success("INVITE processed");
}

vcs::Response SipAdapter::HandleBye(const std::string& sessionId) {
    VCS_GATEWAY_LOG_INFO("HandleBye: sessionId=%s", sessionId.c_str());
    return TerminateCall(sessionId, "BYE received");
}

vcs::Response SipAdapter::HandleExternalBye(const std::string& sessionId, const std::string& conferenceId, const std::string& reason) {
    // [Diag 2026-08-12] Timestamp + thread-id tracer for the BYE teardown.
    // We need this to pinpoint exactly which step is deadlocking the PJSIP
    // worker when the same terminal re-calls and gets no SIP response.
    // If the worker is stuck, the last "ENTER" line without a matching
    // "EXIT" tells us where the hang is.
    auto _t0 = std::chrono::steady_clock::now();
    auto _ms_since = [&](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    };
    auto _log_step = [&](const char* step, std::chrono::steady_clock::time_point t0,
                         const char* status = "ENTER") {
        VCS_GATEWAY_LOG_DEBUG(
            "[BYE-TRACE][tid=%lu] step=%-32s status=%-5s +%lldms total",
            (unsigned long)std::hash<std::thread::id>{}(std::this_thread::get_id()),
            step, status,
            (long long)_ms_since(t0));
    };

    VCS_GATEWAY_LOG_INFO("HandleExternalBye: sessionId=%s, conferenceId=%s, reason=%s",
                         sessionId.c_str(), conferenceId.c_str(), reason.c_str());
    _log_step("HandleExternalBye.entry", _t0, "ENTER");

    if (sessionId.empty()) {
        VCS_GATEWAY_LOG_ERROR("[SIP] HandleExternalBye: sessionId is empty, cannot cleanup");
        return vcs::Response::Error(vcs::ResponseCode::InvalidParams, "sessionId is empty");
    }

    if (!sessionManager_) {
        VCS_GATEWAY_LOG_ERROR("[SIP] HandleExternalBye: no SessionManager available, aborting");
        return vcs::Response::Error(vcs::ResponseCode::InternalError, "no SessionManager");
    }

    // [V5.5 Key Fix] Teardown order: disconnect the pjmedia-bound transports
    // first (stop send threads, uninstall stack receive callbacks), then let
    // SessionManager publish CallTerminated. Media cleanup for the conference
    // runs in GatewayManager's CallTerminated subscriber (event-driven, see
    // GatewayManager::SetEventBus).
    {
        _log_step("Step5.TeardownSession", _t0, "ENTER");
        VCS_GATEWAY_LOG_INFO("[SIP] HandleExternalBye: tearing down session: %s", sessionId.c_str());
        TeardownSession(sessionId, "External BYE received: " + reason, "HandleExternalBye");
        _log_step("Step5.TeardownSession", _t0, "EXIT");
        VCS_GATEWAY_LOG_INFO("[SIP] HandleExternalBye: Session terminated");
    }

    VCS_GATEWAY_LOG_INFO("HandleExternalBye: completed for sessionId=%s", sessionId.c_str());
    _log_step("HandleExternalBye.exit", _t0, "EXIT");
    notifySessionCountChanged();
    return vcs::Response::Success("External BYE handled");
}

vcs::Response SipAdapter::HandleAck(const std::string& sessionId, const Params& params) {
    VCS_GATEWAY_LOG_INFO("HandleAck: sessionId=%s", sessionId.c_str());

    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session || session->callId < 0) {
        VCS_GATEWAY_LOG_ERROR("HandleAck: session not found, sessionId=%s", sessionId.c_str());
        return vcs::Response::Error(vcs::ResponseCode::NotFound, "Session not found");
    }

    {
        const int callId = session->callId;
        VCS_GATEWAY_LOG_DEBUG("HandleAck: callId=%d, sessionState=%d", callId, static_cast<int>(session->state));

        // Get remote address and port from params, create media transport
        std::string remoteAddress;
        uint16_t remotePort = 0;

        auto optAddr = params.GetString("remoteAddress");
        if (optAddr) {
            remoteAddress = *optAddr;
        }

        auto optPort = params.GetInt("remotePort");
        if (optPort) {
            remotePort = static_cast<uint16_t>(*optPort);
        }

        VCS_GATEWAY_LOG_DEBUG("HandleAck: remoteAddress=%s, remotePort=%d", remoteAddress.c_str(), remotePort);

        // Create media transport (if remote info provided)
        if (!remoteAddress.empty() && remotePort > 0) {
            CreateMediaTransport(callId, remoteAddress, remotePort);
        }

        signalingHandler_->UpdateSessionState(callId, SessionState::Active);
        VCS_GATEWAY_LOG_INFO("HandleAck: session state updated to Active");
    }

    (void)params;
    return vcs::Response::Success("ACK processed");
}

vcs::Response SipAdapter::HandleCancel(const std::string& sessionId) {
    return RejectCall(sessionId, "CANCEL received");
}

// ===================================================================
// Private Methods
// ===================================================================

void SipAdapter::notifySessionCountChanged() {
    if (callback_) {
        size_t count = GetActiveSessionCount();
        callback_->OnAdapterSessionCountChanged(type_, count);
    }
}

size_t SipAdapter::ShutdownSessionTransports(const std::string& sessionId, const char* callerTag) {
	if (!sessionManager_ || sessionId.empty()) {
		return 0;
	}

	// Snapshot the transport handles first: Disconnect() can run callbacks that
	// re-enter the session, and we must not iterate the live table while the
	// transport side is shutting down. SessionManager takes the shard lock.
	MemberMediaTransports snapshot;
	std::vector<std::shared_ptr<IRtpTransport>> transports;
	if (sessionManager_->GetTransportSnapshot(sessionId, snapshot)) {
		transports.reserve(snapshot.size());
		for (const auto& kv : snapshot) {
			if (kv.second) {
				transports.push_back(kv.second);
			}
		}
	}

	size_t count = 0;
	for (auto& t : transports) {
		auto sipT = std::dynamic_pointer_cast<SipMediaTransport>(t);
		if (!sipT) {
			continue;
		}
		VCS_GATEWAY_LOG_INFO("[SIP] %s: Disconnecting SipMediaTransport %p (session=%s)",
			callerTag, (void*)sipT.get(), sessionId.c_str());
		sipT->Disconnect();
		++count;
	}
	return count;
}

void SipAdapter::TeardownSession(const std::string& sessionId, const std::string& reason,
	const char* callerTag) {
	if (!sessionManager_ || sessionId.empty()) {
		return;
	}
	ShutdownSessionTransports(sessionId, callerTag);
	sessionManager_->MarkTerminated(sessionId, { reason, 0 });
}

// ===================================================================
// Media Transport Creation Methods
// ===================================================================
// Note: SipAdapter does not directly implement IRtpTransportCallback.
// Media callbacks are handled by SipAdapterMediaCallback (created in onExternalCallEstablished).
// ===================================================================

void SipAdapter::CreateMediaTransportWithInject(int callId, const std::string& remoteAddress,
                                                 uint16_t remotePort, pj::Call& call) {
// PJSIP always enabled
    // Resolve through SessionManager (shared_ptr): a raw CallSession* could
    // dangle if the call is torn down between lookup and use.
    auto session = sessionManager_ ? sessionManager_->GetSessionByCallId(callId) : nullptr;
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("[SIP] Session not found for callId=%d", callId);
        return;
    }

    VCS_GATEWAY_LOG_INFO("[SIP] Creating media transport (RTP passthrough): remote=%s:%d", remoteAddress.c_str(), remotePort);

    // Create SipMediaTransport (RTP passthrough mode, no conference injection)
    auto transport = std::make_shared<SipMediaTransport>(
        session->sessionId,
        remoteAddress,
        remotePort
    );

    // Note: Callbacks are set by caller (via returned transport or subsequent SetCallback call)
    // SipAdapter does not set callbacks here

    // RTP passthrough mode: transport acts as bridge, upper layer (Member) sets RTP callback
    // No longer injecting into PJSIP conference, inject directly into session.
    // [Single write entry] SessionManager owns the transport table; the Audio
    // slot is registered together with its endpoint in one call.
    if (sessionManager_) {
        sessionManager_->AttachTransport(session->sessionId, MediaStreamKind::Audio,
                                         transport, remoteAddress, remotePort);
    }

    VCS_GATEWAY_LOG_INFO("[SIP] Media transport created (RTP passthrough): sessionId=%s", session->sessionId.c_str());
    (void)call;  // Unused parameter
}

void SipAdapter::CreateMediaTransport(int callId, const std::string& remoteAddress, uint16_t remotePort) {
    // Get session's sessionId (shared_ptr: no dangling raw pointer window)
    auto session = sessionManager_ ? sessionManager_->GetSessionByCallId(callId) : nullptr;
    if (!session) {
        return;
    }

    // Create SipMediaTransport
    auto transport = std::make_shared<SipMediaTransport>(
        session->sessionId,
        remoteAddress,
        remotePort
    );

    // Note: Callbacks are set by caller (via returned transport or subsequent SetCallback call)
    // SipAdapter does not set callbacks here

    // Inject into session through the single write entry (Audio/primary slot).
    if (sessionManager_) {
        sessionManager_->AttachTransport(session->sessionId, MediaStreamKind::Audio,
                                         transport, remoteAddress, remotePort);
    }
}

// ===================================================================
// ISignalingHandlerCallback Implementation - For Synchronizing Session Mapping
// ===================================================================

void SipAdapter::OnCallIncoming(int callId,
                                 const std::string& caller,
                                 const vcs::Params& params) {
    // [docs/07] 呼叫事件唯一发布点 = SessionManager（MarkIncoming）。
    // 适配层只负责把信令侧信息登记进来，不再自行 Publish。
    if (!sessionManager_) {
        return;
    }
    auto session = sessionManager_->GetSessionByCallId(callId);
    if (!session || session->sessionId.empty()) {
        return;
    }

    IncomingInfo info;
    info.callerUri = caller;
    const vcs::Optional<bool> videoEnabled = params.GetBool("video_enabled");
    info.hasVideo = videoEnabled ? videoEnabled.value() : false;
    sessionManager_->MarkIncoming(session->sessionId, info);

    (void)params;
}

void SipAdapter::OnCallStateChanged(int callId, SessionState state) {
    (void)callId;
    (void)state;
    // State changes can be handled here
}

std::shared_ptr<vcs::IRtpTransport> SipAdapter::BindOrReuseTransport(
    const std::shared_ptr<CallSession>& session,
    vcs::MediaStreamKind kind,
    const std::string& address,
    int port,
    pjmedia_transport* pjTp) {

    if (!session || !pjTp) {
        return nullptr;
    }

    const StreamTransport* existing = session->GetTransportByKind(kind);
    if (existing) {
        std::shared_ptr<vcs::IRtpTransport> bound = existing->transportObj;
        auto* sip = dynamic_cast<SipMediaTransport*>(bound.get());
        if (sip && sip->IsConnected() && sip->GetBoundPjMediaTransport()) {
            if (sip->GetBoundPjMediaTransport() != static_cast<void*>(pjTp)) {
                // pjsua replaced the transport under a confirmed call. The
                // media layer pinned this object and cannot follow a swap, and
                // the old pjmedia pointer may be freed. Detach it (loudly) so
                // sends fail visibly instead of touching freed memory.
                VCS_GATEWAY_LOG_ERROR(
                    "[SIP] stack replaced the %s transport mid-call: bound=%p, stack=%p, "
                    "session=%s - detaching the stale object",
                    vcs::MediaStreamKindToString(kind),
                    sip->GetBoundPjMediaTransport(), static_cast<void*>(pjTp),
                    session->sessionId.c_str());
                sip->Disconnect();
                return nullptr;
            }
            // Metadata refresh happens when the caller registers this object
            // through SessionManager::AttachTransport; the table is not
            // writable here.
            VCS_GATEWAY_LOG_INFO(
                "[SIP] re-CONFIRMED: reusing %s transport %p, session=%s",
                vcs::MediaStreamKindToString(kind), static_cast<void*>(sip),
                session->sessionId.c_str());
            return bound;
        }
        // Session entry exists but is not a live SipMediaTransport (legacy
        // object or already detached): build a fresh one below.
        VCS_GATEWAY_LOG_WARNING(
            "[SIP] replacing unusable %s session transport (sip=%d, connected=%d), session=%s",
            vcs::MediaStreamKindToString(kind), sip ? 1 : 0,
            (sip && sip->IsConnected()) ? 1 : 0, session->sessionId.c_str());
    }

    const std::string connectionId = session->sessionId +
        (kind == vcs::MediaStreamKind::Audio ? "_audio" : "_video");
    auto transport = std::make_shared<SipMediaTransport>(connectionId, address, port, kind);
    if (kind == vcs::MediaStreamKind::Audio) {
        transport->SetPjMediaTransport(pjTp);
    } else {
        transport->SetPjVideoTransport(pjTp);
    }
    transport->Connect();
    return transport;
}

void SipAdapter::OnCallConnected(int callId) {
	VCS_GATEWAY_LOG_INFO("[SIP] OnCallConnected: callId=%d", callId);

	if (!sessionManager_) {
		return;
	}
	auto session = sessionManager_->GetSessionByCallId(callId);
	if (!session || session->sessionId.empty()) {
		return;
	}
	const std::string sessionId = session->sessionId;
	const std::string conferenceId = session->conferenceId.empty()
		? std::string(SessionManager::kDefaultConferenceId)
		: session->conferenceId;

	// 从 SDP 解析远端地址
	std::string audioAddr, videoAddr;
	int audioPort = 0, videoPort = 0;
	std::map<MediaStreamKind, std::shared_ptr<IRtpTransport>> transports;

	// [docs/11 §3.2] 协商结果要提交给 SessionManager（事实源），不只是日志。
	vcs::NegotiatedCodec audioCodec;
	vcs::NegotiatedCodec videoCodec;

	SipCall* call = signalingHandler_->GetActiveCall(callId);
	if (call) {
		call->GetAllMediaAddresses(audioAddr, audioPort, videoAddr, videoPort);
		VCS_GATEWAY_LOG_INFO("[SIP] Remote media from SDP: audio=%s:%d, video=%s:%d",
			audioAddr.c_str(), audioPort, videoAddr.c_str(), videoPort);

		// [docs/11 §3.2] 记录远端 SDP 协商出的编解码参数：判断 SPS 是
		// 对端没发（sprop=absent 且首帧无 SPS）还是我们没解析（present 却
		// 解不出画面）。每次通话各一条，不打印 base64 与完整 SDP。
		if (call->GetRemoteCodecs(audioCodec, videoCodec)) {
			LogNegotiatedCodec("audio", audioCodec);
			LogNegotiatedCodec("video", videoCodec);
		}
		else {
			VCS_GATEWAY_LOG_INFO("[SDP] no codec parsed from remote SDP");
		}

		// ============================================================
		// mediaAppManaged：pjsua 不再创建媒体通道，GetPjMediaTransport()
		// 返回空。此时 RTP 必须由应用侧在本端 offer 通告的端口上自建，
		// 与入呼路径（onExternalCallEstablished）保持同一套传输实现。
		// ============================================================
		auto createAppManagedTransport =
			[this, &session](MediaStreamKind kind, uint16_t localPort,
				const std::string& remoteAddr, int remotePort,
				uint8_t payloadType) -> std::shared_ptr<IRtpTransport> {
			if (localPort == 0 || remotePort <= 0 || remoteAddr.empty()) {
				return nullptr;
			}
			// re-CONFIRMED 会二次进入：复用首次建立的传输对象。
			auto existing = session->GetTransportObjectByKind(kind);
			if (existing) {
				return existing;
			}

			SimpleRtpConfig cfg;
			cfg.localAddress = localMediaAddress_;
			cfg.localPort = localPort;
			cfg.defaultPayloadType = payloadType;
			cfg.enableRtcp = false;

			RtpDestination dest;
			dest.address = remoteAddr;
			dest.port = static_cast<uint16_t>(remotePort);
			dest.payloadType = payloadType;
			dest.ssrc = 0;
			cfg.destinations.push_back(dest);

			auto transport = std::make_shared<SimpleRtpTransport>(
				session->sessionId +
					(kind == MediaStreamKind::Audio ? "_audio" : "_video"),
				cfg);
			if (!transport->Open()) {
				VCS_GATEWAY_LOG_ERROR("[SIP] Failed to open app-managed %s transport "
					"(local=%u, remote=%s:%d)",
					vcs::MediaStreamKindToString(kind), localPort,
					remoteAddr.c_str(), remotePort);
				return nullptr;
			}
			transport->SetRtcpMux(false);
			VCS_GATEWAY_LOG_INFO("[SIP] App-managed %s transport created: local=%s:%u, "
				"remote=%s:%d (pt=%u)",
				vcs::MediaStreamKindToString(kind),
				transport->GetLocalAddress().c_str(), transport->GetLocalPort(),
				remoteAddr.c_str(), remotePort, payloadType);
			return transport;
		};

		// 音频：用 SipMediaTransport 包 PJSIP 的 transport（media index 0）。
		// re-CONFIRMED 会二次进入本函数：已存在的传输对象必须复用，媒体层
		// 握着首次 CONFIRMED 时拿到的那个 shared_ptr（见 BindOrReuseTransport）。
		if (!audioAddr.empty() && audioPort > 0) {
			pjmedia_transport* pjTp = call->GetPjMediaTransport(0);
			if (pjTp) {
				auto t = BindOrReuseTransport(session, MediaStreamKind::Audio,
					audioAddr, audioPort, pjTp);
				if (t) {
					transports[MediaStreamKind::Audio] = t;
					VCS_GATEWAY_LOG_INFO("[SIP] Audio SipMediaTransport bound to PJSIP transport %p", (void*)pjTp);
				}
				else {
					VCS_GATEWAY_LOG_WARNING("[SIP] Audio transport unusable for callId=%d", callId);
				}
			}
			else {
				VCS_GATEWAY_LOG_INFO("[SIP] No PJSIP audio transport (app-managed media); "
					"building application transport for callId=%d", callId);
				auto t = createAppManagedTransport(
					MediaStreamKind::Audio,
					call->GetLocalAudioPort(),
					audioAddr, audioPort,
					static_cast<uint8_t>(audioCodec.codecName == "PCMA" ? 8 : audioCodec.payloadType));
				if (t) {
					transports[MediaStreamKind::Audio] = t;
				}
				else {
					VCS_GATEWAY_LOG_WARNING("[SIP] App-managed audio transport unavailable for callId=%d",
						callId);
				}
			}
		}

		// 视频：用 SipMediaTransport 包 PJSIP 的 transport（media index 1）
		if (!videoAddr.empty() && videoPort > 0) {
			pjmedia_transport* pjTp = call->GetPjMediaTransport(1);
			if (pjTp) {
				auto t = BindOrReuseTransport(session, MediaStreamKind::Video,
					videoAddr, videoPort, pjTp);
				if (t) {
					transports[MediaStreamKind::Video] = t;
					VCS_GATEWAY_LOG_INFO("[SIP] Video SipMediaTransport bound to PJSIP transport %p", (void*)pjTp);
				}
				else {
					VCS_GATEWAY_LOG_WARNING("[SIP] Video transport unusable for callId=%d", callId);
				}
			}
			else {
				VCS_GATEWAY_LOG_INFO("[SIP] No PJSIP video transport (app-managed media); "
					"building application transport for callId=%d", callId);
				auto t = createAppManagedTransport(
					MediaStreamKind::Video,
					call->GetLocalVideoPort(),
					videoAddr, videoPort,
					static_cast<uint8_t>(videoCodec.payloadType > 0 ? videoCodec.payloadType : 97));
				if (t) {
					transports[MediaStreamKind::Video] = t;
				}
				else {
					VCS_GATEWAY_LOG_WARNING("[SIP] App-managed video transport unavailable for callId=%d",
						callId);
				}
			}
		}
	}

	// [docs/07] 传输登记进 CallSession：CallTerminated 快照的 remoteAddress 与
	// detached-transports 停止都依赖会话内的传输表。re-CONFIRMED 会二次进入，
	// 但 BindOrReuseTransport 保证同一路只有一个对象：AttachTransport 收到同一
	// 对象是 Reused（只刷新端点），一路永远只有一条登记项。
	auto audioIt = transports.find(MediaStreamKind::Audio);
	if (audioIt != transports.end()) {
		sessionManager_->AttachTransport(
			sessionId, MediaStreamKind::Audio, audioIt->second,
			audioAddr, static_cast<uint16_t>(audioPort));
	}
	auto videoIt = transports.find(MediaStreamKind::Video);
	if (videoIt != transports.end()) {
		sessionManager_->AttachTransport(
			sessionId, MediaStreamKind::Video, videoIt->second,
			videoAddr, static_cast<uint16_t>(videoPort));
	}

	// Yealink only starts streaming video after an explicit FIR - the same
	// behaviour the external UDP path compensates for. The peer's SSRC is not
	// known until the first inbound packet, so bootstrap with SSRC 0. Send on
	// the transport the session actually registered (a re-CONFIRMED pass may
	// have kept the earlier video entry).
	std::shared_ptr<IRtpTransport> videoTransport =
		session->GetTransportObjectByKind(MediaStreamKind::Video);
	if (!videoTransport && videoIt != transports.end()) {
		videoTransport = videoIt->second;
	}
	if (videoTransport && !videoTransport->SendFir(0, 0)) {
		VCS_GATEWAY_LOG_WARNING("[SIP] Bootstrap FIR failed: session=%s", sessionId.c_str());
	}

	// 媒体起停不在 gateway：CallConnected 发布后由会议层统一编排
	//（docs/08 §2.3）。

	// [docs/07 决策 C] CallConnected = 信令接通。媒体启动结果只影响日志，
	// 不影响该事实事件的发布；MarkConnected 内部保证恰好一次。
	ConnectedInfo info;
	info.remoteUri = call ? call->GetRemoteUri() : std::string();
	info.remoteAddress = !audioAddr.empty() ? audioAddr : session->GetPrimaryRemoteAddress();
	info.hasVideo = transports.count(MediaStreamKind::Video) > 0;
	info.audioCodec = audioCodec;
	info.videoCodec = videoCodec;
	sessionManager_->MarkConnected(sessionId, info);
}

void SipAdapter::OnCallTerminated(int callId, const std::string& reason) {
	VCS_GATEWAY_LOG_INFO("[SIP] OnCallTerminated: callId=%d, reason=%s", callId, reason.c_str());

	if (!sessionManager_) {
		return;
	}
	auto session = sessionManager_->GetSessionByCallId(callId);
	if (!session) {
		// 会话已不存在 = 已被 MarkTerminated（幂等），无需重复处理
		VCS_GATEWAY_LOG_INFO("[SIP] OnCallTerminated: session already gone for callId=%d", callId);
		return;
	}
	const std::string sessionId = session->sessionId;

	// [docs/07] 媒体清理已归会议层：MarkTerminated 发布事实后由会议层
	// 订阅方按 (conferenceId, sessionId) 停传输、停媒体。gateway 此处不碰媒体。

	// [docs/07] MarkTerminated：remoteAddress/durationSec 快照在 SM 内计算，
	// CallTerminated 由 SM 统一发布，此处不再自行 Publish。
	sessionManager_->MarkTerminated(sessionId, { reason, 0 });

	VCS_GATEWAY_LOG_INFO("[SIP] OnCallTerminated: callId=%d cleanup complete, reason=%s", callId, reason.c_str());
	notifySessionCountChanged();
}

void SipAdapter::OnSignalingError(int callId, int errorCode, const std::string& errorMsg) {
    VCS_GATEWAY_LOG_ERROR("Signaling error: callId=%d, code=%d, msg=%s", callId, errorCode, errorMsg.c_str());
    if (callback_) {
        callback_->OnAdapterError(type_, errorCode, errorMsg);
    }
    (void)callId;
}

// ===================================================================
// Codec Configuration Implementation (Called by Conference Module)
//
// Unified interface call chain:
// 1. ABsPhoneEngine::WireDependencies() sets conferenceControl->SetVideoCodecInfo(sipAdapter)
// 2. Conference.Create command triggers ConferenceService::HandleCreate()
// 3. HandleCreate() calls adapter->ConfigureVideoCodec() to configure PJSIP video codecs
// 4. SipAdapter saves config and calls PJSIP API to set priority and parameters
// ===================================================================

vcs::Response SipAdapter::ConfigureVideoCodec(const vcs::media_config::VideoCodecConfig& config) {
    // Get codec name
    std::string codecName;
    switch (config.codecType) {
        case vcs::media_config::VideoCodecType::H264:
            codecName = "H264";
            break;
        case vcs::media_config::VideoCodecType::VP8:
            codecName = "VP8";
            break;
        case vcs::media_config::VideoCodecType::VP9:
            codecName = "VP9";
            break;
        case vcs::media_config::VideoCodecType::H265:
            codecName = "H265";
            break;
        default:
            return vcs::Response::Error(vcs::ResponseCode::InvalidParams, "Unsupported video codec type");
    }

    // Save config for SDP negotiation
    videoConfig_ = config;
    videoConfigValid_ = true;

    // ============================================================
    // [IMPORTANT] No need to call pjsua_vid_codec_set_priority() or pjsua_vid_codec_set_param().
    //
    // PJSIP internally considers no video encoder available (since no video device enabled).
    // Video capability will be declared directly in SDP in onCallSdpCreated() callback.
    //
    // Video capability declaration is fully handled at SDP level:
    // - Change m=video line port from 0 to non-zero (e.g., 4002)
    // - Add rtpmap, fmtp and other attributes
    // - Set sendonly direction
    // ============================================================

    VCS_GATEWAY_LOG_INFO("[SIP] Video codec config saved for SDP negotiation: %s %dx%d@%dfps (via SDP manipulation in onCallSdpCreated)",
        codecName.c_str(), config.width, config.height, config.framerate);

    return vcs::Response::Success("Video codec config saved for SDP manipulation");
}

vcs::Response SipAdapter::ConfigureAudioCodec(const vcs::media_config::AudioCodecConfig& config) {
    // Get codec name
    std::string codecName;
    switch (config.codecType) {
        case vcs::media_config::AudioCodecType::G711ulaw:
            codecName = "PCMU";
            break;
        case vcs::media_config::AudioCodecType::G711alaw:
            codecName = "PCMA";
            break;
        case vcs::media_config::AudioCodecType::G722:
            codecName = "G722";
            break;
        case vcs::media_config::AudioCodecType::G729:
            codecName = "G729A";
            break;
        case vcs::media_config::AudioCodecType::OPUS:
            codecName = "opus";
            break;
        default:
            return vcs::Response::Error(vcs::ResponseCode::InvalidParams, "Unsupported audio codec type");
    }

    // Audio codec configuration is handled by Media module.
    // Gateway layer only records config info for signaling negotiation.
    VCS_GATEWAY_LOG_INFO("[SIP] Audio codec config recorded (actual codec handled by Media module): %s %dHz",
        codecName.c_str(), config.sampleRate);

    return vcs::Response::Success("Audio codec config forwarded to Media module");
}

// ===================================================================
// SRTP Configuration Implementation (Configured by Application Layer)
// ===================================================================

vcs::Response SipAdapter::ConfigureSrtp(const vcs::media_config::SrtpConfig& config) {
    // Save config for SipSignalingHandler to use when creating accounts
    srtpConfig_ = config;

    // Pass config to SignalingHandler (if initialized)
    if (signalingHandler_) {
        signalingHandler_->SetSrtpConfig(config);
    }

    // Log config source
    const char* modeStr = "Unknown";
    switch (config.mode) {
        case vcs::media_config::SrtpMode::Disabled:
            modeStr = "Disabled";
            break;
        case vcs::media_config::SrtpMode::Optional:
            modeStr = "Optional";
            break;
        case vcs::media_config::SrtpMode::Mandatory:
            modeStr = "Mandatory";
            break;
    }

    VCS_GATEWAY_LOG_INFO("[SIP] SRTP configured by application layer: mode=%s, secureSignaling=%s",
                         modeStr, config.requireSecureSignaling ? "true" : "false");

    return vcs::Response::Success("SRTP configured");
}

// ===================================================================
// Local Media Configuration Implementation (For External SDP Generation)
// ===================================================================

void SipAdapter::SetLocalMediaAddress(const std::string& address) {
    localMediaAddress_ = address;
    VCS_GATEWAY_LOG_INFO("[SIP] Local media address configured: %s", address.c_str());

    // Pass config to SignalingHandler (if initialized)
    if (signalingHandler_) {
        signalingHandler_->SetLocalMediaAddress(address);
    }
}

void SipAdapter::SetStackLogLevel(int level) {
    const int clamped = ClampPjsipLogLevel(level);
    logSettings_.level = clamped;

    // pjsua 还没起来时只记录配置，libInit 会应用它。
    if (!pjsua_var.endpt || !pjsua_var.pool) {
        VCS_GATEWAY_LOG_INFO("[SIP] pjsip log level queued: %d", clamped);
        return;
    }

    // 复制当前配置（保留日志文件名/追加标志），只调整级别门槛。
    // console_level 固定为 0：pjsip 日志只落 siplog.log，不进 Gateway.log。
    pjsua_logging_config cfg = pjsua_var.log_cfg;
    cfg.level = clamped;
    cfg.console_level = 0;

    pj_status_t status = pjsua_reconfigure_logging(&cfg);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR("[SIP] pjsua_reconfigure_logging(%d) failed: %d",
                              clamped, status);
        return;
    }

    VCS_GATEWAY_LOG_INFO("[SIP] pjsip log level changed to %d", clamped);
}

void SipAdapter::SetMediaSdpNegotiator(
    std::shared_ptr<vcs::media_config::IMediaSdpNegotiator> negotiator) {
    sdpNegotiator_ = std::move(negotiator);
    if (signalingHandler_) {
        signalingHandler_->SetMediaSdpNegotiator(sdpNegotiator_);
    }
    VCS_GATEWAY_LOG_INFO("[SIP] SDP negotiator %s",
                         sdpNegotiator_ ? "set on adapter" : "cleared on adapter");
}

void SipAdapter::SetLocalMediaPorts(uint16_t audioPort, uint16_t videoPort) {
    localAudioPort_ = audioPort;
    localVideoPort_ = videoPort;
    VCS_GATEWAY_LOG_INFO("[SIP] Local media ports configured: audio=%u, video=%u", audioPort, videoPort);

    // Pass config to SignalingHandler (if initialized)
    if (signalingHandler_) {
        signalingHandler_->SetLocalMediaAddress(localMediaAddress_);
        signalingHandler_->SetLocalMediaPorts(audioPort, videoPort);
        VCS_GATEWAY_LOG_INFO("[SIP] Media info passed to signaling handler");
    }
}

void SipAdapter::SetLocalMediaPorts(uint16_t audioPort,
                                    uint16_t videoPort,
                                    uint16_t secondaryVideoPort) {
    localAudioPort_          = audioPort;
    localVideoPort_          = videoPort;
    localSecondaryVideoPort_ = secondaryVideoPort;
    VCS_GATEWAY_LOG_INFO(
        "[SIP] Local media ports configured: audio=%u, video=%u, secondaryVideo=%u",
        audioPort, videoPort, secondaryVideoPort);

    // Forward to the SignalingHandler so external SDP generation uses the
    // same three-port layout.
    if (signalingHandler_) {
        signalingHandler_->SetLocalMediaAddress(localMediaAddress_);
        signalingHandler_->SetLocalMediaPorts(audioPort, videoPort, secondaryVideoPort);
        VCS_GATEWAY_LOG_INFO("[SIP] Media info passed to signaling handler (3-port)");
    }
}

// ===================================================================
// External Call Established Callback Implementation (Called by SipSignalingHandler on INVITE CONFIRMED)
// ===================================================================

bool SipAdapter::onExternalCallEstablished(pjsip_inv_session* inv, const RemoteMediaInfo& remoteInfo) {
    VCS_GATEWAY_LOG_INFO("[SIP] ========== External Call Established ==========");
    VCS_GATEWAY_LOG_INFO("[SIP] Remote address: %s", remoteInfo.address.c_str());
    VCS_GATEWAY_LOG_INFO("[SIP] Remote audio port: %u (pt=%u)", remoteInfo.audioPort, remoteInfo.audioPayloadType);
    VCS_GATEWAY_LOG_INFO("[SIP] Remote video port: %u (pt=%u)", remoteInfo.videoPort, remoteInfo.videoPayloadType);

    // Check if remote info is valid
    if (remoteInfo.address.empty() || (remoteInfo.audioPort == 0 && remoteInfo.videoPort == 0)) {
        VCS_GATEWAY_LOG_ERROR("[SIP] Invalid remote media info!");
        return false;
    }

    // Check if SessionManager is valid
    if (!sessionManager_) {
        VCS_GATEWAY_LOG_ERROR("[SIP] SessionManager is NULL!");
        return false;
    }

    // [Fix Issue 1] Determine conference ID to use.
    // Prefer parsed conference ID from INVITE, fallback to default if parsing fails.
    std::string conferenceIdToUse = remoteInfo.conferenceId.empty()
        ? std::string(SessionManager::kDefaultConferenceId)
        : remoteInfo.conferenceId;
    VCS_GATEWAY_LOG_INFO("[SIP] Conference ID to use: %s (parsed: '%s', default: '%s')",
                         conferenceIdToUse.c_str(),
                         remoteInfo.conferenceId.c_str(),
                         SessionManager::kDefaultConferenceId);

    // [Key Fix] Check if backend setup already completed for this inv, avoid
    // duplicate session creation. onExternalCallEstablished may be called
    // multiple times for the same dialog; a non-empty sessionId in
    // activeInvites_ means a previous attempt created the session.
    // (SipAdapter is a friend of SipSignalingHandler.)
    {
        std::lock_guard<std::mutex> lock(signalingHandler_->invitesMutex_);
        auto it = signalingHandler_->activeInvites_.find(reinterpret_cast<uintptr_t>(inv));
        if (it != signalingHandler_->activeInvites_.end() && !it->second.sessionId.empty()) {
            VCS_GATEWAY_LOG_WARNING("[SIP] Backend already set up for inv=%p (sessionId=%s), skipping duplicate creation",
                                    (void*)inv, it->second.sessionId.c_str());
            // Treat as success - previous attempt already set up the backend.
            return true;
        }
    }

    // ====================================================================
    // [V7.1 Fix 2026-08-13] Pick the LOCAL RTP port for this specific
    // call. handleInvite already reserved a fresh even/odd pair from
    // the SipSignalingHandler's port pool and stored it in activeInvites_
    // under our inv key. Fall back to the legacy global ports only if
    // the entry is missing (shouldn't happen, but stay defensive).
    // ====================================================================
    uint16_t perCallAudioPort = localAudioPort_;
    uint16_t perCallVideoPort = localVideoPort_;
    {
        uint16_t a = 0, v = 0;
        if (signalingHandler_->GetPerCallLocalPorts(inv, a, v)) {
            perCallAudioPort = a;
            perCallVideoPort = v;
            VCS_GATEWAY_LOG_INFO(
                "[V7.1] Using per-call RTP ports for inv=%p: audio=%u, video=%u",
                (void*)inv, a, v);
        } else {
            VCS_GATEWAY_LOG_WARNING(
                "[V7.1] No per-call port entry for inv=%p, "
                "falling back to global audio=%u, video=%u (port conflict risk)",
                (void*)inv, perCallAudioPort, perCallVideoPort);
        }
    }

    // ============================================================
    // [docs/07] CreateSession via SessionManager: sessionId 由 SM 生成，
    // 会话落库 + 索引一步完成；适配层不再自行编号。
    // ============================================================
    SessionConfig sessionConfig;
    sessionConfig.conferenceId = conferenceIdToUse;
    sessionConfig.callerUri = remoteInfo.address;  // Use remote address as identifier
    sessionConfig.callerIp = remoteInfo.address;
    // [V7.1 Fix 2026-08-13] Reflect the per-call port pair in the session
    // metadata so downstream consumers (statistics, RTCP, ...) see the
    // correct value too. localSecondaryVideoPort tracks the per-call
    // derived port used below.
    sessionConfig.localAudioPort          = perCallAudioPort;
    sessionConfig.localVideoPort          = perCallVideoPort;
    sessionConfig.localSecondaryVideoPort = remoteInfo.secondaryVideoPort > 0
                                            ? static_cast<uint16_t>(perCallVideoPort + 2)
                                            : 0;
    sessionConfig.protocol = ProtocolType::SIP;

    auto session = sessionManager_->CreateSession(sessionConfig);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("[SIP] Failed to create CallSession");
        return false;
    }
    const std::string sessionId = session->sessionId;
    VCS_GATEWAY_LOG_INFO("[SIP] CallSession created: id=%s", sessionId.c_str());

    // [V5.5 Key Fix] Update sessionId in activeInvites_ for BYE handling to find it
    signalingHandler_->UpdateInviteSessionId(inv, sessionId);

    // ============================================================
    // [V7.0 Phase-2 D/E] Three independent RTP transports
    //
    // Build one SimpleRtpTransport per media stream kind:
    //   - Audio            -> perCallAudioPort
    //   - Main video       -> perCallVideoPort
    //   - Secondary video  -> perCallVideoPort + 2 (content sharing)
    //
    // Each transport owns its own UDP socket and its own destination list,
    // so audio RTP is no longer routed to the terminal's main video port.
    // ============================================================
    std::map<MediaStreamKind, std::shared_ptr<SimpleRtpTransport>> perKindTransports;

    auto buildTransport =
        [&](MediaStreamKind kind,
            uint16_t localPort,
            const std::string& remoteAddr,
            uint16_t remotePort,
            uint8_t payloadType,
            bool rtcpMux,
            const char* tag) -> std::shared_ptr<SimpleRtpTransport>
    {
        if (remotePort == 0 || remoteAddr.empty()) {
            VCS_GATEWAY_LOG_INFO("[SIP] Skipping %s transport: remote info empty (addr='%s', port=%u)",
                                 tag, remoteAddr.c_str(), remotePort);
            return nullptr;
        }
        if (localPort == 0) {
            VCS_GATEWAY_LOG_ERROR("[SIP] Skipping %s transport: local port is 0", tag);
            return nullptr;
        }

        SimpleRtpConfig cfg;
        cfg.localAddress        = localMediaAddress_;
        cfg.localPort           = localPort;
        cfg.defaultPayloadType  = payloadType;
        cfg.ssrc                = 0;
        cfg.enableRtcp          = false;

        RtpDestination dest;
        dest.address     = remoteAddr;
        dest.port        = remotePort;
        dest.payloadType = payloadType;
        dest.ssrc        = 0;
        cfg.destinations.push_back(dest);

        auto t = std::make_shared<SimpleRtpTransport>(sessionId + "_" + tag, cfg);
        if (!t) {
            VCS_GATEWAY_LOG_ERROR("[SIP] Failed to create SimpleRtpTransport for %s", tag);
            return nullptr;
        }
        if (!t->Open()) {
            VCS_GATEWAY_LOG_ERROR("[SIP] Failed to open SimpleRtpTransport for %s", tag);
            return nullptr;
        }
        // [RFC 5761] Tell the transport where RTCP must go (RTP port when the
        // peer negotiated a=rtcp-mux, otherwise RTP port + 1).
        t->SetRtcpMux(rtcpMux);
        VCS_GATEWAY_LOG_INFO("[SIP] %s transport created: local=%s:%u, remote=%s:%u (pt=%u, rtcp-mux=%d)",
                             tag,
                             t->GetLocalAddress().c_str(), t->GetLocalPort(),
                             remoteAddr.c_str(), remotePort, payloadType,
                             rtcpMux ? 1 : 0);
        return t;
    };

    // 1) Audio transport (always first; flagged isPrimary for legacy compat)
    perKindTransports[MediaStreamKind::Audio] = buildTransport(
        MediaStreamKind::Audio,
        perCallAudioPort,
        remoteInfo.address,
        remoteInfo.audioPort,
        static_cast<uint8_t>(remoteInfo.audioPayloadType > 0 ? remoteInfo.audioPayloadType : 0),
        remoteInfo.audioRtcpMux,
        "audio");

    // 2) Main video transport
    perKindTransports[MediaStreamKind::Video] = buildTransport(
        MediaStreamKind::Video,
        perCallVideoPort,
        remoteInfo.address,
        remoteInfo.videoPort,
        static_cast<uint8_t>(remoteInfo.videoPayloadType > 0 ? remoteInfo.videoPayloadType : 97),
        remoteInfo.videoRtcpMux,
        "video");

    // 3) Secondary video transport (only if remote signaled content:slides/sub)
    if (remoteInfo.secondaryVideoPort > 0) {
        // [V7.1 Fix] Use per-call video port + 1 as a contiguous
        // secondary-video port (the pool only allocates audio/video, the
        // third port is derived). If somebody needs a dedicated
        // allocator for the secondary channel we can extend the pool to
        // stride=3 later.
        const uint16_t perCallSecondaryPort =
            (perCallVideoPort != 0) ? static_cast<uint16_t>(perCallVideoPort + 2) : 0;
        perKindTransports[MediaStreamKind::SecondaryVideo] = buildTransport(
            MediaStreamKind::SecondaryVideo,
            perCallSecondaryPort,
            remoteInfo.address,
            remoteInfo.secondaryVideoPort,
            remoteInfo.secondaryVideoPayloadType > 0
                ? remoteInfo.secondaryVideoPayloadType
                : static_cast<uint8_t>(97),
            remoteInfo.secondaryVideoRtcpMux,
            "secondaryVideo");
    }

    // Drop nullptr entries (skipped streams)
    for (auto it = perKindTransports.begin(); it != perKindTransports.end(); ) {
        if (!it->second) it = perKindTransports.erase(it);
        else             ++it;
    }
    if (perKindTransports.empty()) {
        VCS_GATEWAY_LOG_ERROR("[SIP] No transport could be created for inv=%p, aborting", (void*)inv);
        sessionManager_->MarkTerminated(sessionId, { "Transport setup failed", 0 });
        return false;
    }

    // Pick the primary transport (audio) for legacy single-transport callers
    auto primaryIt = perKindTransports.find(MediaStreamKind::Audio);
    std::shared_ptr<SimpleRtpTransport> primaryTransport =
        (primaryIt != perKindTransports.end())
            ? primaryIt->second
            : perKindTransports.begin()->second;

    // Register ALL streams on the CallSession through the single write entry
    // so the media layer can pick up the right transport per MediaStreamKind.
    // Map order is enum order (Audio first), so Audio becomes the primary
    // legacy entry.
    for (const auto& kv : perKindTransports) {
        const MediaStreamKind kind = kv.first;
        const auto& transport = kv.second;

        uint16_t remotePort = 0;
        switch (kind) {
            case MediaStreamKind::Audio:
                remotePort = static_cast<uint16_t>(remoteInfo.audioPort);
                break;
            case MediaStreamKind::Video:
                remotePort = static_cast<uint16_t>(remoteInfo.videoPort);
                break;
            case MediaStreamKind::SecondaryVideo:
                remotePort = static_cast<uint16_t>(remoteInfo.secondaryVideoPort);
                break;
            default:
                remotePort = 0;
                break;
        }

        sessionManager_->AttachTransport(sessionId, kind, transport,
                                         remoteInfo.address, remotePort);
        VCS_GATEWAY_LOG_INFO("[SIP] Registered %s transport on CallSession (remote=%s:%u)",
                             vcs::MediaStreamKindToString(kind),
                             remoteInfo.address.c_str(), (unsigned)remotePort);
    }

    VCS_GATEWAY_LOG_INFO(
        "[SIP] External RTP transports created and stored: count=%zu (inv=%p)",
        perKindTransports.size(), (void*)inv);

    // ============================================================
    // Wire the session: context, activation, keyframe
    // ============================================================
    {
        SessionContext ctx;
        ctx.sessionId = sessionId;
        ctx.memberId = sessionId;  // external calls: memberId == sessionId
        ctx.conferenceId = conferenceIdToUse;
        if (primaryTransport) {
            ctx.transportType = primaryTransport->GetType();
        }
        sessionManager_->SetContext(sessionId, ctx);
    }

    if (!sessionManager_->ActivateSession(sessionId)) {
        VCS_GATEWAY_LOG_ERROR("[SIP] Failed to activate session: %s", sessionId.c_str());
        sessionManager_->MarkTerminated(sessionId, { "Activation failed", 0 });
        return false;
    }

    if (primaryTransport) {
        // Yealink only starts sending video after an explicit FIR. Give the
        // receive socket a moment to settle, then request a keyframe.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!primaryTransport->SendFir(0, 0)) {
            VCS_GATEWAY_LOG_WARNING("[SIP] Failed to send FIR via SimpleRtpTransport");
        }
    }

    VCS_GATEWAY_LOG_INFO("[SIP] ========== External Call Setup Complete ==========");
    return true;
}

// ===================================================================
// Session Management Methods Implementation (Via SessionManager)
// ===================================================================

void SipAdapter::UpdateSessionState(const std::string& sessionId, SessionState state) {
    if (!sessionManager_) {
        return;
    }
    auto session = sessionManager_->GetSession(sessionId);
    if (!session) {
        return;
    }

    VCS_GATEWAY_LOG_INFO("[SipAdapter] CallSession state update: %s, state=%d",
        sessionId.c_str(), static_cast<int>(state));

    switch (state) {
        case SessionState::Active:
            sessionManager_->ActivateSession(sessionId);
            break;
        case SessionState::Disconnecting:
            sessionManager_->PauseSession(sessionId);
            break;
        case SessionState::Disconnected:
            // 终态一律走 MarkTerminated（幂等 + 发布 CallTerminated）
            sessionManager_->MarkTerminated(sessionId, { "Signaling disconnected", 0 });
            break;
        default:
            break;
    }

    // Notify SignalingHandler to update state (legacy PJSIP-call path only;
    // external-path sessions carry no callId)
    if (signalingHandler_ && session->callId >= 0) {
        signalingHandler_->UpdateSessionState(session->callId, state);
    }
}

void SipAdapter::CleanupSession(const std::string& sessionId) {
    if (!sessionManager_ || sessionId.empty()) {
        return;
    }

    VCS_GATEWAY_LOG_INFO("[SipAdapter] Cleaning up session: %s", sessionId.c_str());
    sessionManager_->MarkTerminated(sessionId, { "Cleanup requested", 0 });
    notifySessionCountChanged();
}

void SipAdapter::SendBye(const std::string& sessionId) {
    auto session = sessionManager_ ? sessionManager_->GetSession(sessionId) : nullptr;
    if (!session) {
        VCS_GATEWAY_LOG_WARNING("[SipAdapter] SendBye: session not found: %s", sessionId.c_str());
        return;
    }

    if (session->callId >= 0 && signalingHandler_) {
        VCS_GATEWAY_LOG_INFO("[SipAdapter] Sending BYE for callId=%d", session->callId);
        // TerminateCall 内部走 MarkTerminated（发布 CallTerminated）
        signalingHandler_->TerminateCall(session->callId, "Normal");
    } else {
        // External-path session (no callId): terminate directly in the SM
        sessionManager_->MarkTerminated(session->sessionId, { "BYE sent", 0 });
    }
}

SessionContext* SipAdapter::GetSessionContext(const std::string& sessionId) {
    return sessionManager_ ? sessionManager_->GetContext(sessionId) : nullptr;
}

// ===================================================================
// Helper Methods Implementation
// ===================================================================

std::string SipAdapter::ParseConferenceId(const Params& params) {
    // Parse conference ID from params.
    // Prefer conference_id, fallback to callee.
    auto conferenceId = params.GetString("conference_id");
    if (conferenceId.has_value() && !conferenceId.value().empty()) {
        return conferenceId.value();
    }

    auto callee = params.GetString("callee");
    if (callee.has_value() && !callee.value().empty()) {
        // Extract conference ID from SIP URI.
        // e.g., "conf:12345" -> "12345"
        std::string uri = callee.value();
        size_t colonPos = uri.find(':');
        if (colonPos != std::string::npos) {
            return uri.substr(colonPos + 1);
        }
        return uri;
    }

    // Default conference ID
    return SessionManager::kDefaultConferenceId;
}

int SipAdapter::MapConferenceErrorToSipCode(int errorCode) {
    return 0;
}

std::shared_ptr<SimpleRtpTransport> SipAdapter::CreateMediaTransportWithCallback(
    int callId,
    const SessionContext& context,
    const RemoteMediaInfo& remoteInfo) {

    // Configure SimpleRtpTransport
    SimpleRtpConfig config;
    config.localAddress = localMediaAddress_;
    config.localPort = localVideoPort_;
    config.defaultPayloadType = static_cast<uint8_t>(remoteInfo.videoPayloadType > 0 ? remoteInfo.videoPayloadType : 97);

    // Add video destination
    if (remoteInfo.videoPort > 0) {
        RtpDestination videoDest;
        videoDest.address = remoteInfo.address;
        videoDest.port = remoteInfo.videoPort;
        videoDest.payloadType = static_cast<uint8_t>(remoteInfo.videoPayloadType);
        videoDest.ssrc = 0;
        config.destinations.push_back(videoDest);
    }

    // Add audio destination
    if (remoteInfo.audioPort > 0) {
        RtpDestination audioDest;
        audioDest.address = remoteInfo.address;
        audioDest.port = remoteInfo.audioPort;
        audioDest.payloadType = static_cast<uint8_t>(remoteInfo.audioPayloadType);
        audioDest.ssrc = 0;
        config.destinations.push_back(audioDest);
    }

    // Create transport
    auto transport = std::make_shared<SimpleRtpTransport>(context.sessionId, config);
    if (!transport) {
        VCS_GATEWAY_LOG_ERROR("[SipAdapter] Failed to create SimpleRtpTransport");
        return nullptr;
    }

    // Set context
    transport->SetContext(context);

    // Open transport
    if (!transport->Open()) {
        VCS_GATEWAY_LOG_ERROR("[SipAdapter] Failed to open SimpleRtpTransport");
        return nullptr;
    }

    // [RFC 5761] Legacy single-transport path: RTCP follows the stream that
    // owns the primary destination (video when present, audio otherwise).
    transport->SetRtcpMux(remoteInfo.videoPort > 0 ? remoteInfo.videoRtcpMux
                                                   : remoteInfo.audioRtcpMux);

    VCS_GATEWAY_LOG_INFO("[SipAdapter] MediaTransport created: sessionId=%s",
        context.sessionId.c_str());

    return transport;
}

} // namespace gateway
} // namespace vcs
