/**
 * @file GatewayManager.cpp
 * @brief Gateway manager - adapter lifecycle + single SessionManager owner
 *
 * docs/07 会话层施工图落地：
 * - 每 conference 一个 SessionManager 的 V5.9/V6.0 机制（EAGER/LAZY/MANUAL、
 *   延迟销毁 timer、OnConferenceStart/End、OnSessionAdded/Removed）整体移除。
 *   会话隔离由 SM 内部的 L1 conference 分片承担，不再靠多个 SM 实例。
 * - GatewayManager 持有唯一 SessionManager；SetEventBus 同时注入 SM（SM 是
 *   呼叫事件唯一发布点）。
 */

#define _GNU_SOURCE 1  // Must be before any #include

#include <time.h>     // For CLOCK_MONOTONIC
#include "GatewayManager.h"
#include "IProtocolAdapter.h"
#include "PjThreadGuard.h"
#include "ILogger.h"
#include <algorithm>
#include <memory>
#include <thread>

namespace vcs { namespace gateway {

GatewayManager::GatewayManager()
    : running_(false)
{
    sessionManager_ = std::make_unique<SessionManager>();
    VCS_GATEWAY_LOG_INFO("[GatewayManager] Created (single SessionManager model)");
}

GatewayManager::~GatewayManager() {
    Stop();
}

// ============================================================
// SessionView 值快照查询
// ============================================================

bool GatewayManager::GetSessionView(const std::string& sessionId, SessionView& out) const {
    return sessionManager_ && sessionManager_->GetSessionView(sessionId, out);
}

std::vector<SessionView> GatewayManager::GetActiveSessions() const {
    if (!sessionManager_) {
        return {};
    }
    return sessionManager_->GetSessionViews();
}

// ============================================================
// IGatewayManager implementation
// ============================================================

Response GatewayManager::Start() {
	// 不在此处登记 pj 线程：pj 尚未初始化（adapter->Start 里才 init），
	// 登记点在各 adapter 启动之后 / 其余调用入口。
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_) {
		return Response::Error(ResponseCode::Conflict, "Gateway already running");
	}

	// 依次启动所有的协议适配器
	for (auto& pair : adapters_) {
		if (pair.second) {
			Response r = pair.second->Start();
			if (!r.IsSuccess()) {
				VCS_GATEWAY_LOG_ERROR("[GatewayManager] Failed to start adapter: %s", pair.first.c_str());
				// 如果其中一个启动失败，应当回滚已经启动的 Adapter
				for (auto& startedPair : adapters_) {
					if (startedPair.second && startedPair.first != pair.first) {
						startedPair.second->Stop();
					}
				}
				return r;
			}
			VCS_GATEWAY_LOG_INFO("[GatewayManager] Adapter started: %s", pair.first.c_str());
		}
	}

	running_ = true;
	return Response::Success("Gateway started");
}

Response GatewayManager::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return Response::Success("Gateway already stopped");
        }
        running_ = false;
    }

    // 只有 running_ 期间 pjlib 才存活（adapter Start/Stop 之间）。析构函数会再
    // 调一次 Stop，此时 pj_shutdown 已释放 pjlib 线程 TLS，再碰 pj 即断言。
    EnsurePjThreadRegistered();

    // 会话清理必须先于 adapter.Stop：残留会话的传输 StopReceiving/Disconnect
    // 会进 pjmedia，而 adapter.Stop 内 libDestroy 会 pj_shutdown 释放 pjlib 的
    // 线程 TLS（thread_tls_id=-1），此后再碰 pj 即断言。
    if (sessionManager_) {
        // 停机路径批量清理，不发布事件（订阅者也在拆除中）
        sessionManager_->CleanupAllSessions("Gateway stopped");
    }

	for (auto& pair : adapters_) {
		if (pair.second) {
			pair.second->Stop();
			VCS_GATEWAY_LOG_INFO("[GatewayManager] Adapter stopped: %s", pair.first.c_str());
		}
	}

    return Response::Success("Gateway stopped");
}

bool GatewayManager::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

// ============================================================
// Adapter management
// ============================================================

void GatewayManager::RegisterAdapter(std::shared_ptr<IProtocolAdapter> adapter) {
    if (!adapter) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // Inject the single SM; adapters delegate all session CRUD to it.
    if (sessionManager_) {
        adapter->SetSessionManager(sessionManager_.get());
        VCS_GATEWAY_LOG_INFO(
            "[GatewayManager] RegisterAdapter: injected SM %p into %s adapter",
            (void*)sessionManager_.get(), adapter->GetType().c_str());
    } else {
        VCS_GATEWAY_LOG_ERROR("[GatewayManager] RegisterAdapter: SM is null!");
    }

    // Adapters registered after the negotiator was injected must still see it.
    if (sdpNegotiator_) {
        adapter->SetMediaSdpNegotiator(sdpNegotiator_);
        VCS_GATEWAY_LOG_INFO(
            "[GatewayManager] RegisterAdapter: injected SDP negotiator into %s adapter",
            adapter->GetType().c_str());
    }

    adapters_[adapter->GetType()] = adapter;
}

void GatewayManager::SetMediaSdpNegotiator(
    std::shared_ptr<vcs::media_config::IMediaSdpNegotiator> negotiator) {
    std::lock_guard<std::mutex> lock(mutex_);

    sdpNegotiator_ = std::move(negotiator);
    for (auto& pair : adapters_) {
        if (pair.second) {
            pair.second->SetMediaSdpNegotiator(sdpNegotiator_);
        }
    }
    VCS_GATEWAY_LOG_INFO("[GatewayManager] SDP negotiator %s",
                         sdpNegotiator_ ? "injected into all adapters" : "cleared");
}

void GatewayManager::SetStackLogLevel(int level) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : adapters_) {
        if (pair.second) {
            pair.second->SetStackLogLevel(level);
        }
    }
    VCS_GATEWAY_LOG_INFO("[GatewayManager] Stack log level set to %d", level);
}

void GatewayManager::UnregisterAdapter(const std::string& type) {
    std::lock_guard<std::mutex> lock(mutex_);
    adapters_.erase(type);
}

std::shared_ptr<IProtocolAdapter> GatewayManager::GetAdapter(const std::string& type) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = adapters_.find(type);
    if (it != adapters_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> GatewayManager::GetRegisteredAdapterTypes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> types;
    for (const auto& pair : adapters_) {
        types.push_back(pair.first);
    }
    return types;
}

// ============================================================
// Outbound call
// ============================================================

Response GatewayManager::InitiateCall(const std::string& protocol,
	const std::string& callee,
	const Params& params,
	std::string& connectionId) {
	if (!running_) {
		return Response::Error(ResponseCode::InternalError, "Gateway not running");
	}
	EnsurePjThreadRegistered();

	std::string type = protocol;
	std::transform(type.begin(), type.end(), type.begin(), ::toupper);

	// 根据协议类型动态获取 Adapter
	std::shared_ptr<IProtocolAdapter> adapter = nullptr;
	if (type == "SIP") {
		adapter = GetAdapter("SIP");
	}
	else if (type == "H323") {
		adapter = GetAdapter("H.323");
	}

	if (!adapter) {
		VCS_GATEWAY_LOG_ERROR("[GatewayManager] InitiateCall: No adapter for protocol %s", protocol.c_str());
		return Response::Error(ResponseCode::NotFound, "Protocol adapter not found");
	}

	VCS_GATEWAY_LOG_INFO("[GatewayManager] InitiateCall: protocol=%s, callee=%s", protocol.c_str(), callee.c_str());
	return adapter->InitiateCall(callee, params, connectionId);
}

// ============================================================
// 会话控制（docs/08 §2.1）
// ============================================================

namespace {

const char* AdapterTypeForProtocol(ProtocolType protocol) {
    switch (protocol) {
        case ProtocolType::SIP:    return "SIP";
        case ProtocolType::H323:   return "H.323";
        case ProtocolType::WEBRTC: return "WebRTC";
        default:                   return nullptr;
    }
}

} // namespace

std::shared_ptr<IProtocolAdapter> GatewayManager::FindAdapterForSession(
    const std::string& sessionId, Response& error) {
    if (!running_) {
        error = Response::Error(ResponseCode::InternalError, "Gateway not running");
        return nullptr;
    }
    EnsurePjThreadRegistered();
    if (!sessionManager_) {
        error = Response::Error(ResponseCode::InternalError, "no session manager");
        return nullptr;
    }

    SessionView view;
    if (!sessionManager_->GetSessionView(sessionId, view)) {
        VCS_GATEWAY_LOG_ERROR("[GatewayManager] session not found: %s", sessionId.c_str());
        error = Response::Error(ResponseCode::NotFound, "session not found");
        return nullptr;
    }

    const char* type = AdapterTypeForProtocol(view.protocol);
    if (!type) {
        VCS_GATEWAY_LOG_ERROR("[GatewayManager] session %s has unknown protocol=%d",
            sessionId.c_str(), static_cast<int>(view.protocol));
        error = Response::Error(ResponseCode::InvalidOperation,
                                "unknown session protocol");
        return nullptr;
    }

    auto adapter = GetAdapter(type);
    if (!adapter) {
        VCS_GATEWAY_LOG_ERROR("[GatewayManager] adapter not registered: %s (session=%s)",
            type, sessionId.c_str());
        error = Response::Error(ResponseCode::InternalError,
                                std::string("adapter not registered: ") + type);
        return nullptr;
    }
    return adapter;
}

Response GatewayManager::AnswerCall(const std::string& sessionId,
                                    bool accept,
                                    const std::string& reason) {
    Response error;
    auto adapter = FindAdapterForSession(sessionId, error);
    if (!adapter) {
        return error;
    }
    VCS_GATEWAY_LOG_INFO("[GatewayManager] AnswerCall: session=%s, protocol=%s, accept=%d",
        sessionId.c_str(), adapter->GetType().c_str(), accept ? 1 : 0);
    if (accept) {
        return adapter->AcceptCall(sessionId, Params());
    }
    return adapter->RejectCall(sessionId, reason);
}

Response GatewayManager::HangupCall(const std::string& sessionId,
                                    const std::string& reason) {
    Response error;
    auto adapter = FindAdapterForSession(sessionId, error);
    if (!adapter) {
        return error;
    }
    VCS_GATEWAY_LOG_INFO("[GatewayManager] HangupCall: session=%s, protocol=%s, reason=%s",
        sessionId.c_str(), adapter->GetType().c_str(), reason.c_str());
    return adapter->TerminateCall(sessionId, reason);
}

Response GatewayManager::SendDTMF(const std::string& sessionId,
                                  const std::string& digits) {
    Response error;
    auto adapter = FindAdapterForSession(sessionId, error);
    if (!adapter) {
        return error;
    }
    return adapter->SendDTMF(sessionId, digits);
}

bool GatewayManager::GetSessionTransports(const std::string& sessionId,
                                          MemberMediaTransports& out) const {
    out.clear();
    return sessionManager_ && sessionManager_->GetTransportSnapshot(sessionId, out);
}

// ============================================================
// Business layer integration
// ============================================================

void GatewayManager::SetEventBus(std::shared_ptr<vcs::IEventBus> bus) {
    std::lock_guard<std::mutex> lock(mutex_);

    eventBus_ = bus;

    // SM is the single publish point for call events — it gets the bus first.
    if (sessionManager_) {
        sessionManager_->SetEventBus(bus);
    }

    for (auto& pair : adapters_) {
        if (pair.second) {
            pair.second->SetEventBus(bus);
        }
    }

    // 媒体清理不再由 gateway 兜底：CallTerminated 的媒体停止由 conference
    // 在事件栈内执行（docs/08 §2.3）。
}

// ============================================================
// 收包门（docs/09 §2.2）
// ============================================================

Response GatewayManager::StartTransport(const std::string& sessionId) {
    if (!running_) {
        return Response::Error(ResponseCode::InternalError, "Gateway not running");
    }
    // 会议层在事件分发线程（T2）调用本接口，先登记 pj 线程再碰传输。
    EnsurePjThreadRegistered();
    if (!sessionManager_) {
        return Response::Error(ResponseCode::InternalError, "no session manager");
    }
    std::shared_ptr<CallSession> session = sessionManager_->GetSession(sessionId);
    if (!session) {
        // 门没开就没有媒体，不静默（docs/09 §0 口径）。
        VCS_GATEWAY_LOG_ERROR("[GatewayManager] StartTransport: session not found: %s",
            sessionId.c_str());
        return Response::Error(ResponseCode::NotFound, "session not found");
    }

    // Locked snapshot: the transport table is owned by SessionManager and is
    // mutated by teardown / re-CONFIRMED. Iterating the session's own vector
    // here would race those writers. The snapshot keeps the shared_ptrs alive
    // for the duration of the gate operation.
    MemberMediaTransports transports;
    if (!sessionManager_->GetTransportSnapshot(sessionId, transports)) {
        VCS_GATEWAY_LOG_ERROR("[GatewayManager] StartTransport: session vanished: %s",
            sessionId.c_str());
        return Response::Error(ResponseCode::NotFound, "session not found");
    }
    if (transports.empty()) {
        VCS_GATEWAY_LOG_ERROR("[GatewayManager] StartTransport: no transports registered: %s",
            sessionId.c_str());
        return Response::Error(ResponseCode::InvalidParams, "no transports");
    }

    size_t opened = 0;
    for (auto& kv : transports) {
        if (!kv.second) {
            continue;
        }
        if (!kv.second->StartReceiving()) {
            VCS_GATEWAY_LOG_ERROR("[GatewayManager] StartTransport failed: session=%s, kind=%s",
                sessionId.c_str(), MediaStreamKindToString(kv.first));
            // 部分开启也按错误上报；StopTransport 可回滚已开传输。
            StopTransport(sessionId);
            return Response::Error(ResponseCode::InternalError, "StartReceiving failed");
        }
        opened++;
    }
    VCS_GATEWAY_LOG_INFO("[GatewayManager] StartTransport: session=%s, opened=%zu",
        sessionId.c_str(), opened);
    return Response::Success("transport started");
}

Response GatewayManager::StopTransport(const std::string& sessionId) {
    if (!running_) {
        return Response::Success("already stopped");
    }
    EnsurePjThreadRegistered();
    if (!sessionManager_) {
        return Response::Success("no session manager");
    }
    MemberMediaTransports transports;
    if (!sessionManager_->GetTransportSnapshot(sessionId, transports) ||
        transports.empty()) {
        return Response::Success("already stopped");
    }
    for (auto& kv : transports) {
        if (kv.second) {
            kv.second->StopReceiving();
        }
    }
    VCS_GATEWAY_LOG_INFO("[GatewayManager] StopTransport: session=%s",
        sessionId.c_str());
    return Response::Success("transport stopped");
}

// ============================================================
// IProtocolAdapterCallback implementation
// ============================================================

void GatewayManager::OnAdapterSessionCountChanged(const std::string& adapterType,
                                                 size_t activeCount) {
    (void)adapterType;
    (void)activeCount;
}

void GatewayManager::OnAdapterError(const std::string& adapterType,
                                    int errorCode,
                                    const std::string& errorMsg) {
    (void)adapterType;
    (void)errorCode;
    (void)errorMsg;
}

// ============================================================
// [AbsPhone extension] Gateway registration interface
// ============================================================

Response GatewayManager::RegisterGW(std::string& ipAddr, int signalPort,
                                 int tcpVideoPort, int tcpAudioPort, int index) {
    VCS_GATEWAY_LOG_INFO("[GatewayManager] RegisterGW: index=%d", index);
    return Response::Success();
}

Response GatewayManager::UnregisterGW(int index) {
    VCS_GATEWAY_LOG_INFO("[GatewayManager] UnregisterGW: index=%d", index);
    return Response::Success();
}

Response GatewayManager::IsGWRegistered(int index, bool& registered) {
    registered = false;
    return Response::Success();
}

// ============================================================
// [AbsPhone extension] Gateway node management
// ============================================================

Response GatewayManager::AddGWNode(const std::string& ip, int signalPort,
                                int tcpVideoPort, int tcpAudioPort, int index) {
    VCS_GATEWAY_LOG_INFO("[GatewayManager] AddGWNode: ip=%s, index=%d", ip.c_str(), index);
    return Response::Success();
}

Response GatewayManager::DelGWNode(int index) {
    VCS_GATEWAY_LOG_INFO("[GatewayManager] DelGWNode: index=%d", index);
    return Response::Success();
}

// ============================================================
// [AbsPhone extension] Gateway endpoint info query
// ============================================================

Response GatewayManager::GetGWEPInfo(const std::string& serverId, AbsPhoneGWEPInfo& info) {
    info.serverId = serverId;
    info.index = 0;
    return Response::Success();
}

Response GatewayManager::GetGWEPInfoByIndex(int index, AbsPhoneGWEPInfo& info) {
    info.index = index;
    return Response::Success();
}

// ============================================================
// [AbsPhone extension] Gateway member invitation
// ============================================================

Response GatewayManager::InviteGWMember(const std::string& roomId,
                                      const std::string& callToken,
                                      const std::string& serverId,
                                      const std::string& deviceId) {
    VCS_GATEWAY_LOG_INFO("[GatewayManager] InviteGWMember: room=%s, device=%s",
                         roomId.c_str(), deviceId.c_str());
    return Response::Success();
}

// ============================================================
// [AbsPhone extension] Stream control
// ============================================================

Response GatewayManager::EnableStream(bool enable,
                                   const std::string& serverId,
                                   const std::string& deviceId,
                                   int streamType,
                                   const void* streamInfo) {
    VCS_GATEWAY_LOG_INFO("[GatewayManager] EnableStream: enable=%d, device=%s, type=%d",
                         enable, deviceId.c_str(), streamType);
    return Response::Success();
}

} // namespace gateway
} // namespace vcs
