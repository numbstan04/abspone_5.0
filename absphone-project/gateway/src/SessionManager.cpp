/**
 * @file SessionManager.cpp
 * @brief Session manager implementation - 唯一事实源 + 唯一呼叫事件发布点
 *
 * docs/07 会话层施工图落地：
 * - CreateSession 生成 sessionId（{proto}_{seq}），调用方不再自行编号
 * - MarkIncoming / MarkConnected / MarkTerminated 是状态提交 + 事件发布的
 *   唯一入口；CallIncoming / CallConnected / CallTerminated 全工程只在此发布
 * - MarkTerminated 顺序（钉死）：锁内取快照 -> Disconnected/拆传输/清索引 ->
 *   出锁停 transport -> 发 CallTerminated -> 移除 callId 全局索引
 * - 超时策略内聚：IncomingPending/OutgoingPending/Connecting 32s，
 *   Disconnecting 5s，Active 不超时；超时会话一律走 MarkTerminated
 */

#include "SessionManager.h"
#include "ILogger.h"
#include "TimeUtils.h"
#include "Json.h"
#include <algorithm>
#include <memory>
#include <sstream>

namespace vcs { namespace gateway {

namespace {

// Timeout policy (docs/07 §2.5). 0 = never expires.
uint32_t StateTimeoutSeconds(SessionState state) {
    switch (state) {
        case SessionState::IncomingPending: return 32;
        case SessionState::OutgoingPending: return 32;
        case SessionState::Connecting:      return 32;
        case SessionState::Disconnecting:   return 5;
        default:                            return 0;
    }
}

const char* ProtocolPrefix(ProtocolType protocol) {
    switch (protocol) {
        case ProtocolType::SIP:    return "sip";
        case ProtocolType::H323:   return "h323";
        case ProtocolType::WEBRTC: return "webrtc";
        default:                   return "sess";
    }
}

// memberId -> sessionId is a small per-shard map; a reverse scan is fine at
// terminal scale (a few hundred sessions max).
std::string FindMemberIdBySessionId(
    const std::unordered_map<std::string, std::string>& memberIdToSessionId,
    const std::string& sessionId) {
    for (const auto& kv : memberIdToSessionId) {
        if (kv.second == sessionId) {
            return kv.first;
        }
    }
    return "";
}

void FillSessionView(const std::shared_ptr<CallSession>& s,
                     const std::string& memberId,
                     SessionView& out) {
    out.hasSession = true;
    out.sessionId = s->sessionId;
    out.conferenceId = s->conferenceId;
    out.memberId = memberId;
    out.caller = s->caller;
    out.callee = s->callee;
    out.state = s->state;
    out.startTimeMs = s->startTime;
    out.connectTimeMs = s->connectTime;
    out.primaryRemoteAddress = s->GetPrimaryRemoteAddress();
    out.hasAudio = s->GetTransportByKind(MediaStreamKind::Audio) != nullptr;
    out.hasVideo = s->GetTransportByKind(MediaStreamKind::Video) != nullptr;
    out.protocol = s->protocol;
    out.audioCodec = s->audioCodec;
    out.videoCodec = s->videoCodec;
    out.transportGeneration = s->transportGeneration;
}

} // namespace

// ============================================================================
// Constructor and destructor
// ============================================================================

SessionManager::SessionManager() {
    VCS_GATEWAY_LOG_INFO("[SessionManager] Created (2-level sharding: per-conf + %d shards, protocol-agnostic)",
                         SHARDS_PER_CONFERENCE);
}

SessionManager::~SessionManager() {
    CleanupAllSessions("SessionManager destroyed");
    VCS_GATEWAY_LOG_INFO("[SessionManager] Destroyed");
}

void SessionManager::SetEventBus(const std::shared_ptr<vcs::IEventBus>& bus) {
    // Wiring happens once from GatewayManager before any call traffic; Mark*
    // readers therefore never race this assignment.
    eventBus_ = bus;
    VCS_GATEWAY_LOG_INFO("[SessionManager] EventBus injected (%s)",
                         bus ? "set" : "cleared");
}

// ============================================================================
// Internal helpers
// ============================================================================

std::shared_ptr<SessionManager::ConferenceData>
SessionManager::GetOrCreateConferenceData(const std::string& conferenceId) {
    if (conferenceId.empty()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(confRegistryMutex_);
    auto it = confRegistry_.find(conferenceId);
    if (it != confRegistry_.end()) {
        return it->second;
    }
    auto data = std::make_shared<ConferenceData>();
    confRegistry_[conferenceId] = data;
    return data;
}

std::shared_ptr<SessionManager::ConferenceData>
SessionManager::GetConferenceData(const std::string& conferenceId) const {
    if (conferenceId.empty()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(confRegistryMutex_);
    auto it = confRegistry_.find(conferenceId);
    if (it != confRegistry_.end()) {
        return it->second;
    }
    return nullptr;
}

size_t SessionManager::DestroyConferenceData(const std::string& conferenceId) {
    std::shared_ptr<ConferenceData> confData;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        auto it = confRegistry_.find(conferenceId);
        if (it == confRegistry_.end()) {
            return 0;
        }
        confData = it->second;
        confRegistry_.erase(it);
    }

    // Detach transports under the shard locks; StopReceiving happens AFTER
    // every shard lock is released (the receive thread's callback path may
    // need the same lock to deliver a packet — joining under the lock
    // self-deadlocks).
    std::vector<std::shared_ptr<IRtpTransport>> allDetached;
    size_t destroyedCount = 0;
    for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
        std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
        for (auto& pair : confData->shards[i]->bySessionId) {
            auto& session = pair.second;
            auto detached = session->DetachAllTransports();
            for (auto& t : detached) {
                allDetached.push_back(std::move(t));
            }
            session->state = SessionState::Disconnected;
        }
        destroyedCount += confData->shards[i]->bySessionId.size();
        confData->shards[i]->bySessionId.clear();
        confData->shards[i]->memberIdToSessionId.clear();
        confData->shards[i]->contexts.clear();
        confData->shards[i]->stateIndex.clear();
        confData->shards[i]->connectedPublished.clear();
    }

    for (auto& t : allDetached) {
        if (t && t->IsReceiving()) {
            t->StopReceiving();
        }
    }
    return destroyedCount;
}

std::vector<std::shared_ptr<IRtpTransport>>
SessionManager::DestroySessionInShard(ConferenceData& confData,
                                      size_t shardIdx,
                                      const std::string& sessionId,
                                      const std::string& reason) {
    auto& shard = confData.shards[shardIdx];
    // Caller must already hold shard->lock
    auto it = shard->bySessionId.find(sessionId);
    if (it == shard->bySessionId.end()) {
        return {};
    }

    std::shared_ptr<CallSession> session = it->second;

    // Detach under the lock; the caller stops the transports after releasing
    // it (StopReceiving joins the receive thread, whose callback may need
    // this very shard lock — stopping under the lock self-deadlocks).
    std::vector<std::shared_ptr<IRtpTransport>> detached =
        session->DetachAllTransports();

    // State index: erase from the OLD state's set BEFORE flipping the state.
    // (Erasing after the flip would remove from the Disconnected set, which
    // was never populated, and leak the original state's entry forever.)
    shard->stateIndex[session->state].erase(sessionId);
    session->state = SessionState::Disconnected;

    if (!session->conferenceId.empty()) {
        for (auto mapIt = shard->memberIdToSessionId.begin();
             mapIt != shard->memberIdToSessionId.end(); ) {
            if (mapIt->second == sessionId) {
                mapIt = shard->memberIdToSessionId.erase(mapIt);
            } else {
                ++mapIt;
            }
        }
    }

    shard->contexts.erase(sessionId);
    shard->connectedPublished.erase(sessionId);
    shard->outgoingPublished.erase(sessionId);
    shard->bySessionId.erase(it);

    totalDestroyed_++;
    VCS_GATEWAY_LOG_INFO("Session destroyed: id=%s, reason=%s, detached=%zu transports",
                         sessionId.c_str(), reason.c_str(), detached.size());
    return detached;
}

void SessionManager::RemoveFromCallIdIndex(const std::vector<int>& callIds) {
    if (callIds.empty()) return;
    std::lock_guard<std::mutex> lock(globalIndexLock_);
    for (int callId : callIds) {
        callIdToSessionId_.erase(callId);
        callIdToConfId_.erase(callId);
    }
}

void SessionManager::RemoveFromCallIdIndex(int callId) {
    std::lock_guard<std::mutex> lock(globalIndexLock_);
    callIdToSessionId_.erase(callId);
    callIdToConfId_.erase(callId);
}

std::string SessionManager::GenerateSessionId(ProtocolType protocol) {
    const uint64_t seq = sessionIdCounter_.fetch_add(1) + 1;
    std::ostringstream oss;
    oss << ProtocolPrefix(protocol) << "_" << seq;
    return oss.str();
}

// ============================================================================
// Session creation（sessionId 由 SM 生成）
// ============================================================================

std::shared_ptr<CallSession> SessionManager::CreateSession(
    const SessionConfig& config, SessionState initialState) {

    const std::string confId =
        config.conferenceId.empty() ? std::string(kDefaultConferenceId)
                                    : config.conferenceId;
    const std::string sessionId = GenerateSessionId(config.protocol);

    auto confData = GetOrCreateConferenceData(confId);
    if (!confData) {
        VCS_GATEWAY_LOG_ERROR("CreateSession: failed to get/create conference data: %s",
                              confId.c_str());
        return nullptr;
    }

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];

    std::lock_guard<std::mutex> shardLock(shard->lock);

    // Counter-based ids cannot collide in practice; the check is defensive.
    if (shard->bySessionId.find(sessionId) != shard->bySessionId.end()) {
        VCS_GATEWAY_LOG_ERROR("CreateSession: generated id already exists: %s",
                              sessionId.c_str());
        return nullptr;
    }

    auto session = std::make_shared<CallSession>();
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("CreateSession: allocation failed");
        return nullptr;
    }

    session->sessionId = sessionId;
    session->conferenceId = confId;
    session->caller = config.callerUri;
    session->state = initialState;
    session->protocol = config.protocol;
    session->callId = -1;  // bound later via AddSession(callId, session); 0 is a valid pjsua id
    const int64_t nowMs = GetCurrentTimeMs();
    session->startTime = nowMs;
    session->lastActiveTime = nowMs;

    shard->bySessionId[sessionId] = session;
    shard->stateIndex[initialState].insert(sessionId);
    totalCreated_++;

    VCS_GATEWAY_LOG_INFO("Session created: id=%s, conferenceId=%s, caller=%s, "
                         "protocol=%d, state=%d, shard=%zu",
        sessionId.c_str(), confId.c_str(), config.callerUri.c_str(),
        static_cast<int>(config.protocol), static_cast<int>(initialState), shardIdx);

    return session;
}

// ============================================================================
// 状态迁移 + 事件发布（唯一发布点）
//
// 状态提交与事件发布在同一个 shard 临界区内完成：EventBus::Publish 只入队
// （订阅者在独立派发线程回调，绝不在发布者线程执行），因此在持锁状态下发布
// 不会死锁，却能保证同一会话的事件入队顺序与锁获取顺序一致——并发场景下
// CallConnected 不可能越过 CallTerminated。
// ============================================================================

bool SessionManager::MarkOutgoing(const std::string& sessionId, const OutgoingInfo& info) {
    if (sessionId.empty()) return false;

    auto session = GetSession(sessionId);
    if (!session) return false;
    const std::string confId = session->conferenceId;
    auto confData = GetConferenceData(confId);
    if (!confData) return false;

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];

    std::lock_guard<std::mutex> shardLock(shard->lock);
    auto it = shard->bySessionId.find(sessionId);
    if (it == shard->bySessionId.end()) return false;

    // Exactly-once: a retried InitiateCall must not publish a second time.
    if (shard->outgoingPublished.count(sessionId) > 0) {
        return true;
    }

    auto& sess = it->second;
    if (sess->state != SessionState::OutgoingPending) {
        VCS_GATEWAY_LOG_WARNING("MarkOutgoing: session %s not in OutgoingPending (state=%d), ignored",
                                sessionId.c_str(), static_cast<int>(sess->state));
        return false;
    }

    if (!info.calleeUri.empty()) {
        sess->callee = info.calleeUri;
    }
    sess->lastActiveTime = GetCurrentTimeMs();
    shard->outgoingPublished.insert(sessionId);

    const std::string memberId = FindMemberIdBySessionId(shard->memberIdToSessionId, sessionId);

    VCS_GATEWAY_LOG_INFO("MarkOutgoing: id=%s, callee=%s",
                         sessionId.c_str(), sess->callee.c_str());

    if (eventBus_) {
        vcs::Event ev;
        ev.type = vcs::EventType::CallOutgoing;
        ev.source = vcs::EventSource::Gateway;
        ev.sessionId = sessionId;
        ev.conferenceId = confId;
        ev.memberId = memberId;
        vcs::base::Json j;
        j.Set("calleeUri", sess->callee);
        ev.payload = j.Dump();
        eventBus_->Publish(ev);
    }
    return true;
}

bool SessionManager::MarkIncoming(const std::string& sessionId, const IncomingInfo& info) {
    if (sessionId.empty()) return false;

    auto session = GetSession(sessionId);
    if (!session) return false;
    const std::string confId = session->conferenceId;
    auto confData = GetConferenceData(confId);
    if (!confData) return false;

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];

    std::lock_guard<std::mutex> shardLock(shard->lock);
    auto it = shard->bySessionId.find(sessionId);
    if (it == shard->bySessionId.end()) return false;

    auto& sess = it->second;
    if (sess->state != SessionState::IncomingPending) {
        VCS_GATEWAY_LOG_WARNING("MarkIncoming: session %s not in IncomingPending (state=%d), ignored",
                                sessionId.c_str(), static_cast<int>(sess->state));
        return false;
    }

    sess->caller = info.callerUri;
    sess->lastActiveTime = GetCurrentTimeMs();

    const std::string memberId = FindMemberIdBySessionId(shard->memberIdToSessionId, sessionId);

    VCS_GATEWAY_LOG_INFO("MarkIncoming: id=%s, caller=%s, hasVideo=%d",
                         sessionId.c_str(), info.callerUri.c_str(),
                         info.hasVideo ? 1 : 0);

    if (eventBus_) {
        vcs::Event ev;
        ev.type = vcs::EventType::CallIncoming;
        ev.source = vcs::EventSource::Gateway;
        ev.sessionId = sessionId;
        ev.conferenceId = confId;
        ev.memberId = memberId;
        vcs::base::Json j;
        j.Set("callerUri", info.callerUri);
        j.Set("hasVideo", info.hasVideo);
        ev.payload = j.Dump();
        eventBus_->Publish(ev);
    }
    return true;
}

bool SessionManager::MarkConnected(const std::string& sessionId, const ConnectedInfo& info) {
    if (sessionId.empty()) return false;

    auto session = GetSession(sessionId);
    if (!session) return false;
    const std::string confId = session->conferenceId;
    auto confData = GetConferenceData(confId);
    if (!confData) return false;

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];

    std::lock_guard<std::mutex> shardLock(shard->lock);
    auto it = shard->bySessionId.find(sessionId);
    if (it == shard->bySessionId.end()) return false;

    // Exactly-once publish: re-INVITE / duplicate CONFIRMED hit this branch.
    if (shard->connectedPublished.count(sessionId) > 0) {
        return true;
    }

    auto& sess = it->second;
    const SessionState old = sess->state;
    if (old != SessionState::Active) {
        shard->stateIndex[old].erase(sessionId);
        sess->state = SessionState::Active;
        shard->stateIndex[SessionState::Active].insert(sessionId);
    }
    const int64_t nowMs = GetCurrentTimeMs();
    sess->lastActiveTime = nowMs;
    if (sess->connectTime == 0) {
        sess->connectTime = nowMs;
    }
    sess->audioCodec = info.audioCodec;
    sess->videoCodec = info.videoCodec;
    shard->connectedPublished.insert(sessionId);

    const std::string memberId = FindMemberIdBySessionId(shard->memberIdToSessionId, sessionId);

    VCS_GATEWAY_LOG_INFO("MarkConnected: id=%s, remote=%s, hasVideo=%d, audio=%s(pt=%u)",
                         sessionId.c_str(), info.remoteAddress.c_str(),
                         info.hasVideo ? 1 : 0,
                         info.audioCodec.codecName.empty()
                             ? "(none)" : info.audioCodec.codecName.c_str(),
                         (unsigned)info.audioCodec.payloadType);

    if (eventBus_) {
        vcs::Event ev;
        ev.type = vcs::EventType::CallConnected;
        ev.source = vcs::EventSource::Gateway;
        ev.sessionId = sessionId;
        ev.conferenceId = confId;
        ev.memberId = memberId;
        vcs::base::Json j;
        j.Set("remoteUri", info.remoteUri);
        j.Set("remoteAddress", info.remoteAddress);
        j.Set("hasVideo", info.hasVideo);
        j.Set("audioCodec", info.audioCodec.codecName);
        j.Set("videoCodec", info.videoCodec.codecName);
        ev.payload = j.Dump();
        eventBus_->Publish(ev);
    }
    return true;
}

bool SessionManager::MarkTerminated(const std::string& sessionId, const TerminatedInfo& info) {
    if (sessionId.empty()) return false;

    auto session = GetSession(sessionId);
    if (!session) {
        // Already gone: exactly-once guard. Every teardown path funnels here,
        // so "not found" simply means another path (or an earlier call) won.
        return false;
    }
    const std::string confId = session->conferenceId;
    auto confData = GetConferenceData(confId);
    if (!confData) return false;

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];

    std::vector<std::shared_ptr<IRtpTransport>> detached;
    std::string memberId;
    std::string remoteUri;
    std::string remoteAddress;
    int64_t durationSec = 0;
    bool destroyed = false;

    // The shard lock covers ONLY the map/index mutation and the value
    // snapshot. StopReceiving joins the transport's receive thread and the
    // event bus may call arbitrary handlers — both must run outside the lock,
    // otherwise a packet callback (which needs the shard lock) deadlocks
    // against join(), and a slow event handler stalls every session in this
    // shard.
    {
        std::lock_guard<std::mutex> shardLock(shard->lock);
        auto it = shard->bySessionId.find(sessionId);
        if (it != shard->bySessionId.end()) {
            // Full value snapshot: the session object dies inside
            // DestroySessionInShard, and the event must carry everything.
            const CallSession snap = *it->second;
            memberId = FindMemberIdBySessionId(shard->memberIdToSessionId, sessionId);

            detached = DestroySessionInShard(*confData, shardIdx, sessionId,
                                             info.reason.empty() ? "Terminated" : info.reason);

            remoteUri = !snap.caller.empty() ? snap.caller : snap.callee;
            remoteAddress = snap.GetPrimaryRemoteAddress();
            const int64_t nowMs = GetCurrentTimeMs();
            durationSec = (snap.connectTime > 0 && nowMs > snap.connectTime)
                              ? (nowMs - snap.connectTime) / 1000 : 0;
            destroyed = true;
        }
    }
    if (!destroyed) {
        return false;
    }

    VCS_GATEWAY_LOG_INFO("MarkTerminated: id=%s, conf=%s, reason=%s, cause=%d, "
                         "remote=%s, durationSec=%lld",
                         sessionId.c_str(), confId.c_str(), info.reason.c_str(),
                         info.cause, remoteAddress.c_str(),
                         static_cast<long long>(durationSec));

    // Lock released: stop the transports (join their receive threads), then
    // publish the fact with a complete snapshot.
    for (auto& t : detached) {
        if (t && t->IsReceiving()) {
            t->StopReceiving();
        }
    }

    // Sweep the global index BEFORE publishing.
    //
    // TerminateSession() runs on the pjsip worker thread (incoming BYE ->
    // inv_respond_incoming_bye -> on_call_state). Publishing CallTerminated
    // executes the application's media teardown synchronously on that same
    // thread, and that chain was observed clobbering this very stack frame:
    // the `std::vector<int>` built below then used garbage begin/end pointers
    // and crashed inside memmove. Building the vector first keeps this frame
    // out of the teardown's blast radius; the observable behaviour is
    // unchanged because the session is already erased from the shard.
    const int terminatedCallId = session ? session->callId : -1;
    session.reset();
    if (terminatedCallId >= 0) {
        RemoveFromCallIdIndex(terminatedCallId);
    }

    if (eventBus_) {
        vcs::Event ev;
        ev.type = vcs::EventType::CallTerminated;
        ev.source = vcs::EventSource::Gateway;
        ev.sessionId = sessionId;
        ev.conferenceId = confId;
        ev.memberId = memberId;
        vcs::base::Json j;
        j.Set("reason", info.reason);
        j.Set("cause", info.cause);
        j.Set("remoteUri", remoteUri);
        j.Set("remoteAddress", remoteAddress);
        j.Set("durationSec", durationSec);
        ev.payload = j.Dump();
        eventBus_->Publish(ev);
    }

    return true;
}

// ============================================================================
// Session query
// ============================================================================

std::shared_ptr<CallSession> SessionManager::GetSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return nullptr;
    }

    // Scan all conferences (slower path)
    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    size_t shardIdx = ShardIndex(sessionId);
    for (const auto& confId : confIds) {
        auto confData = GetConferenceData(confId);
        if (!confData) continue;
        auto& shard = confData->shards[shardIdx];
        std::lock_guard<std::mutex> shardLock(shard->lock);
        auto it = shard->bySessionId.find(sessionId);
        if (it != shard->bySessionId.end()) {
            return it->second;
        }
    }
    return nullptr;
}

std::shared_ptr<CallSession> SessionManager::GetSessionInConference(
    const std::string& conferenceId, const std::string& sessionId) {

    if (conferenceId.empty() || sessionId.empty()) {
        return nullptr;
    }

    auto confData = GetConferenceData(conferenceId);
    if (!confData) return nullptr;

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];
    std::lock_guard<std::mutex> shardLock(shard->lock);
    auto it = shard->bySessionId.find(sessionId);
    if (it != shard->bySessionId.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<CallSession> SessionManager::GetSessionByCallId(int callId) {
    if (callId < 0) {
        return nullptr;
    }

    std::string confId, sessionId;
    {
        std::lock_guard<std::mutex> lock(globalIndexLock_);
        auto it = callIdToSessionId_.find(callId);
        if (it == callIdToSessionId_.end()) {
            return nullptr;
        }
        sessionId = it->second;
        auto cit = callIdToConfId_.find(callId);
        if (cit != callIdToConfId_.end()) {
            confId = cit->second;
        }
    }

    if (confId.empty()) {
        return GetSession(sessionId);
    }
    return GetSessionInConference(confId, sessionId);
}

std::shared_ptr<CallSession> SessionManager::GetSessionByMemberId(
    const std::string& memberId) {

    if (memberId.empty()) return nullptr;

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    for (const auto& confId : confIds) {
        auto confData = GetConferenceData(confId);
        if (!confData) continue;
        for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
            std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
            auto it = confData->shards[i]->memberIdToSessionId.find(memberId);
            if (it != confData->shards[i]->memberIdToSessionId.end()) {
                auto sit = confData->shards[i]->bySessionId.find(it->second);
                if (sit != confData->shards[i]->bySessionId.end()) {
                    return sit->second;
                }
            }
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<CallSession>> SessionManager::GetSessionsByConference(
    const std::string& conferenceId) {

    std::vector<std::shared_ptr<CallSession>> result;
    auto confData = GetConferenceData(conferenceId);
    if (!confData) return result;

    for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
        std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
        for (const auto& pair : confData->shards[i]->bySessionId) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<std::shared_ptr<CallSession>>
SessionManager::GetSessionsByConferenceAndState(
    const std::string& conferenceId, SessionState state) {

    std::vector<std::shared_ptr<CallSession>> result;
    auto confData = GetConferenceData(conferenceId);
    if (!confData) return result;

    for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
        std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
        auto stateIt = confData->shards[i]->stateIndex.find(state);
        if (stateIt == confData->shards[i]->stateIndex.end()) {
            continue;
        }
        for (const auto& sessionId : stateIt->second) {
            auto sessionIt = confData->shards[i]->bySessionId.find(sessionId);
            if (sessionIt != confData->shards[i]->bySessionId.end()) {
                result.push_back(sessionIt->second);
            }
        }
    }
    return result;
}

std::vector<std::shared_ptr<CallSession>>
SessionManager::GetSessionsByState(SessionState state) {

    std::vector<std::shared_ptr<CallSession>> result;

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    for (const auto& confId : confIds) {
        auto sessionsInConf = GetSessionsByConferenceAndState(confId, state);
        result.insert(result.end(), sessionsInConf.begin(), sessionsInConf.end());
    }
    return result;
}

std::vector<std::shared_ptr<CallSession>> SessionManager::GetAllSessions() const {
    std::vector<std::shared_ptr<CallSession>> result;

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    for (const auto& confId : confIds) {
        std::shared_ptr<ConferenceData> confData;
        {
            std::lock_guard<std::mutex> lock(confRegistryMutex_);
            auto it = confRegistry_.find(confId);
            if (it == confRegistry_.end()) continue;
            confData = it->second;
        }

        for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
            std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
            for (const auto& pair : confData->shards[i]->bySessionId) {
                result.push_back(pair.second);
            }
        }
    }
    return result;
}

size_t SessionManager::GetActiveSessionCount() const {
    size_t count = 0;
    auto sessions = const_cast<SessionManager*>(this)->GetAllSessions();
    for (const auto& s : sessions) {
        if (s->state == SessionState::Active) {
            ++count;
        }
    }
    return count;
}

size_t SessionManager::GetActiveSessionCount(const std::string& conferenceId) const {
    size_t count = 0;
    auto confData = const_cast<SessionManager*>(this)->GetConferenceData(conferenceId);
    if (!confData) return 0;

    for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
        std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
        for (const auto& pair : confData->shards[i]->bySessionId) {
            if (pair.second->state == SessionState::Active) {
                ++count;
            }
        }
    }
    return count;
}

size_t SessionManager::GetSessionCountByState(SessionState state) const {
    size_t count = 0;
    auto sessions = const_cast<SessionManager*>(this)->GetAllSessions();
    for (const auto& s : sessions) {
        if (s->state == state) {
            ++count;
        }
    }
    return count;
}

// ============================================================================
// 值快照查询（docs/07 §2.4）
// ============================================================================

bool SessionManager::GetSessionView(const std::string& sessionId, SessionView& out) const {
    out = SessionView{};
    if (sessionId.empty()) return false;

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    size_t shardIdx = ShardIndex(sessionId);
    for (const auto& confId : confIds) {
        auto confData = GetConferenceData(confId);
        if (!confData) continue;
        auto& shard = confData->shards[shardIdx];
        std::lock_guard<std::mutex> shardLock(shard->lock);
        auto it = shard->bySessionId.find(sessionId);
        if (it != shard->bySessionId.end()) {
            FillSessionView(it->second,
                            FindMemberIdBySessionId(shard->memberIdToSessionId, sessionId),
                            out);
            return true;
        }
    }
    return false;
}

std::vector<SessionView> SessionManager::GetSessionViews() const {
    std::vector<SessionView> result;

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    for (const auto& confId : confIds) {
        std::shared_ptr<ConferenceData> confData;
        {
            std::lock_guard<std::mutex> lock(confRegistryMutex_);
            auto it = confRegistry_.find(confId);
            if (it == confRegistry_.end()) continue;
            confData = it->second;
        }

        for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
            std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
            for (const auto& pair : confData->shards[i]->bySessionId) {
                SessionView view;
                FillSessionView(pair.second,
                                FindMemberIdBySessionId(confData->shards[i]->memberIdToSessionId,
                                                        pair.first),
                                view);
                result.push_back(view);
            }
        }
    }
    return result;
}

// ============================================================================
// Business logic methods
// ============================================================================

TransportAttachResult SessionManager::AttachTransport(
    const std::string& sessionId,
    MediaStreamKind kind,
    std::shared_ptr<IRtpTransport> transport,
    const std::string& remoteAddress,
    uint16_t remotePort) {

    if (sessionId.empty()) {
        VCS_GATEWAY_LOG_ERROR("AttachTransport: empty sessionId (kind=%s)",
                              vcs::MediaStreamKindToString(kind));
        return TransportAttachResult::Rejected;
    }

    auto session = GetSession(sessionId);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("AttachTransport: session not found: %s (kind=%s)",
                              sessionId.c_str(), vcs::MediaStreamKindToString(kind));
        return TransportAttachResult::Rejected;
    }

    auto confData = GetConferenceData(session->conferenceId);
    if (!confData) {
        VCS_GATEWAY_LOG_ERROR("AttachTransport: conference not found: %s",
                              session->conferenceId.c_str());
        return TransportAttachResult::Rejected;
    }
    auto& shard = confData->shards[ShardIndex(sessionId)];

    // Swap under the shard lock, stop the old object AFTER releasing it.
    // StopReceiving joins the receive thread; if that thread is delivering a
    // packet whose callback path needs shard->lock, joining under the lock
    // deadlocks.
    TransportAttachResult result = TransportAttachResult::Rejected;
    std::shared_ptr<IRtpTransport> replaced;
    {
        std::lock_guard<std::mutex> shardLock(shard->lock);
        auto it = shard->bySessionId.find(sessionId);
        if (it == shard->bySessionId.end()) {
            VCS_GATEWAY_LOG_ERROR("AttachTransport: session vanished: %s", sessionId.c_str());
            return TransportAttachResult::Rejected;
        }

        StreamTransport* slot = it->second->FindTransportMutable(kind);
        if (!slot) {
            StreamTransport& entry = it->second->AddTransport(
                kind, std::move(transport), remoteAddress, remotePort);
            // A newly installed object is a new wiring generation: upper layers
            // must re-run their media wiring against it.
            ++it->second->transportGeneration;
            VCS_GATEWAY_LOG_INFO("AttachTransport: attached %s (type=%s) session=%s, remote=%s:%u",
                                 vcs::MediaStreamKindToString(kind),
                                 entry.transportObj ? entry.transportObj->GetType().c_str() : "null",
                                 sessionId.c_str(), entry.remoteAddress.c_str(),
                                 (unsigned)entry.remotePort);
            return TransportAttachResult::Attached;
        }

        if (!slot->transportObj) {
            // Slot exists only as an endpoint placeholder (address seeded
            // before the object existed).
            if (!transport) {
                // Another address-only seed on the same placeholder (SIP URI
                // seed followed by NotifyMediaReady): refresh the endpoint and
                // keep waiting for the object. There is nothing to "complete"
                // and nothing to log a type for.
                if (!remoteAddress.empty()) {
                    slot->remoteAddress = remoteAddress;
                }
                if (remotePort != 0) {
                    slot->remotePort = remotePort;
                }
                VCS_GATEWAY_LOG_INFO("AttachTransport: reused %s placeholder (no object yet) "
                                     "session=%s, remote=%s:%u",
                                     vcs::MediaStreamKindToString(kind),
                                     sessionId.c_str(), slot->remoteAddress.c_str(),
                                     (unsigned)slot->remotePort);
                return TransportAttachResult::Reused;
            }
            // Installing the object completes the placeholder.
            slot->transportObj = std::move(transport);
            if (!remoteAddress.empty()) {
                slot->remoteAddress = remoteAddress;
            }
            if (remotePort != 0) {
                slot->remotePort = remotePort;
            }
            ++it->second->transportGeneration;
            VCS_GATEWAY_LOG_INFO("AttachTransport: completed %s (type=%s) session=%s, remote=%s:%u",
                                 vcs::MediaStreamKindToString(kind),
                                 slot->transportObj->GetType().c_str(),
                                 sessionId.c_str(), slot->remoteAddress.c_str(),
                                 (unsigned)slot->remotePort);
            return TransportAttachResult::Attached;
        }

        if (!transport || slot->transportObj.get() == transport.get()) {
            // Address-only seed, or the same object installed again
            // (re-CONFIRMED / double registration): never stop a live object.
            if (!remoteAddress.empty()) {
                slot->remoteAddress = remoteAddress;
            }
            if (remotePort != 0) {
                slot->remotePort = remotePort;
            }
            if (transport) {
                slot->transportObj = std::move(transport);
            }
            VCS_GATEWAY_LOG_INFO("AttachTransport: reused %s (type=%s) session=%s, remote=%s:%u",
                                 vcs::MediaStreamKindToString(kind),
                                 slot->transportObj->GetType().c_str(),
                                 sessionId.c_str(), slot->remoteAddress.c_str(),
                                 (unsigned)slot->remotePort);
            return TransportAttachResult::Reused;
        }

        replaced = std::move(slot->transportObj);
        slot->transportObj = std::move(transport);
        if (!remoteAddress.empty()) {
            slot->remoteAddress = remoteAddress;
        }
        if (remotePort != 0) {
            slot->remotePort = remotePort;
        }
        ++it->second->transportGeneration;
        VCS_GATEWAY_LOG_WARNING(
            "AttachTransport: replaced %s transport (old=%s -> new=%s) session=%s, remote=%s:%u",
            vcs::MediaStreamKindToString(kind),
            replaced->GetType().c_str(),
            slot->transportObj->GetType().c_str(),
            sessionId.c_str(), slot->remoteAddress.c_str(), (unsigned)slot->remotePort);
        result = TransportAttachResult::Replaced;
    }

    // Outside the shard lock: join the old receive thread safely.
    if (replaced && replaced->IsReceiving()) {
        replaced->StopReceiving();
    }
    return result;
}

std::shared_ptr<IRtpTransport> SessionManager::GetTransport(const std::string& sessionId) {
    // Locked snapshot: the transport table is mutated by AttachTransport /
    // teardown, so reading the session's vector directly would race them.
    MemberMediaTransports snap;
    if (!GetTransportSnapshot(sessionId, snap)) {
        return nullptr;
    }
    auto audio = snap.find(MediaStreamKind::Audio);
    if (audio != snap.end()) {
        return audio->second;
    }
    return snap.empty() ? nullptr : snap.begin()->second;
}

std::shared_ptr<IRtpTransport> SessionManager::GetTransportByMemberId(
    const std::string& memberId) {
    auto session = GetSessionByMemberId(memberId);
    if (!session) return nullptr;
    return GetTransport(session->sessionId);
}

std::vector<std::shared_ptr<IRtpTransport>> SessionManager::GetTransportsByConference(
    const std::string& conferenceId) {
    std::vector<std::shared_ptr<IRtpTransport>> result;
    auto confData = GetConferenceData(conferenceId);
    if (!confData) return result;

    for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
        std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
        for (const auto& pair : confData->shards[i]->bySessionId) {
            for (const auto& stream : pair.second->transports) {
                if (stream.transportObj) {
                    result.push_back(stream.transportObj);
                }
            }
        }
    }
    return result;
}

bool SessionManager::GetTransportSnapshot(const std::string& sessionId,
                                          MemberMediaTransports& out) {
    out.clear();
    if (sessionId.empty()) {
        return false;
    }

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    const size_t shardIdx = ShardIndex(sessionId);
    for (const auto& confId : confIds) {
        auto confData = GetConferenceData(confId);
        if (!confData) continue;
        auto& shard = confData->shards[shardIdx];
        std::lock_guard<std::mutex> shardLock(shard->lock);
        auto it = shard->bySessionId.find(sessionId);
        if (it == shard->bySessionId.end()) continue;
        for (const auto& stream : it->second->transports) {
            if (stream.transportObj) {
                out[stream.kind] = stream.transportObj;
            }
        }
        return true;
    }
    return false;
}

bool SessionManager::SetContext(const std::string& sessionId,
                                const SessionContext& context) {
    auto confData = GetConferenceData(context.conferenceId);
    if (!confData) {
        VCS_GATEWAY_LOG_ERROR("SetContext: conference not found: %s",
                              context.conferenceId.c_str());
        return false;
    }

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];
    std::lock_guard<std::mutex> shardLock(shard->lock);

    auto it = shard->bySessionId.find(sessionId);
    if (it == shard->bySessionId.end()) {
        VCS_GATEWAY_LOG_ERROR("SetContext: session not found: %s", sessionId.c_str());
        return false;
    }

    shard->contexts[sessionId] = context;
    it->second->conferenceId = context.conferenceId;

    if (!context.memberId.empty()) {
        shard->memberIdToSessionId[context.memberId] = sessionId;
    }

    VCS_GATEWAY_LOG_INFO("Context set: sessionId=%s, memberId=%s, conferenceId=%s",
        sessionId.c_str(), context.memberId.c_str(), context.conferenceId.c_str());
    return true;
}

bool SessionManager::ActivateSession(const std::string& sessionId) {
    auto session = GetSession(sessionId);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("ActivateSession: session not found: %s", sessionId.c_str());
        return false;
    }
    return UpdateSessionState(sessionId, SessionState::Active);
}

bool SessionManager::PauseSession(const std::string& sessionId) {
    auto session = GetSession(sessionId);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("PauseSession: session not found: %s", sessionId.c_str());
        return false;
    }
    return UpdateSessionState(sessionId, SessionState::Disconnecting);
}

SessionContext* SessionManager::GetContext(const std::string& sessionId) {
    auto session = GetSession(sessionId);
    if (!session) return nullptr;

    std::string confId = session->conferenceId;
    auto confData = GetConferenceData(confId);
    if (!confData) return nullptr;
    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];
    std::lock_guard<std::mutex> shardLock(shard->lock);
    auto it = shard->contexts.find(sessionId);
    if (it != shard->contexts.end()) {
        return &it->second;
    }
    return nullptr;
}

bool SessionManager::UpdateSessionState(const std::string& sessionId,
                                        SessionState newState) {
    auto session = GetSession(sessionId);
    if (!session) {
        VCS_GATEWAY_LOG_ERROR("UpdateSessionState: session not found: %s", sessionId.c_str());
        return false;
    }

    std::string confId = session->conferenceId;
    auto confData = GetConferenceData(confId);
    if (!confData) return false;

    size_t shardIdx = ShardIndex(sessionId);
    auto& shard = confData->shards[shardIdx];
    std::lock_guard<std::mutex> shardLock(shard->lock);
    auto it = shard->bySessionId.find(sessionId);
    if (it == shard->bySessionId.end()) return false;

    SessionState old = it->second->state;
    if (old == newState) {
        return true;
    }

    shard->stateIndex[old].erase(sessionId);
    shard->stateIndex[newState].insert(sessionId);

    it->second->state = newState;
    const int64_t nowMs = GetCurrentTimeMs();
    it->second->lastActiveTime = nowMs;
    if (newState == SessionState::Active && old != SessionState::Active) {
        it->second->connectTime = nowMs;
    }
    VCS_GATEWAY_LOG_DEBUG("Session state updated: id=%s, %d -> %d",
        sessionId.c_str(), static_cast<int>(old), static_cast<int>(newState));
    return true;
}

// ============================================================================
// Session traversal
// ============================================================================

void SessionManager::ForEachSessionInConference(const std::string& conferenceId,
    std::function<bool(std::shared_ptr<CallSession>)> callback) {

    auto confData = GetConferenceData(conferenceId);
    if (!confData) return;

    // Snapshot under the shard lock, invoke outside it: a callback that calls
    // back into SessionManager would otherwise deadlock on the same shard.
    std::vector<std::shared_ptr<CallSession>> snapshot;
    for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
        std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
        snapshot.reserve(snapshot.size() + confData->shards[i]->bySessionId.size());
        for (const auto& pair : confData->shards[i]->bySessionId) {
            snapshot.push_back(pair.second);
        }
    }

    for (const auto& session : snapshot) {
        if (!callback(session)) break;
    }
}

void SessionManager::ForEachSession(
    std::function<bool(std::shared_ptr<CallSession>)> callback) {
    auto sessions = GetAllSessions();
    for (auto& s : sessions) {
        if (!callback(s)) {
            break;
        }
    }
}

std::shared_ptr<CallSession> SessionManager::FindSession(
    std::function<bool(std::shared_ptr<CallSession>)> predicate) {
    auto sessions = GetAllSessions();
    for (auto& s : sessions) {
        if (predicate(s)) {
            return s;
        }
    }
    return nullptr;
}

// ============================================================================
// 超时清理（策略内聚，调用方不再传超时秒数）
// ============================================================================

size_t SessionManager::CleanupTimeoutSessions() {
    const int64_t nowMs = GetCurrentTimeMs();

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    std::vector<std::string> victims;
    for (const auto& confId : confIds) {
        auto confData = GetConferenceData(confId);
        if (!confData) continue;

        for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
            std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
            for (const auto& pair : confData->shards[i]->bySessionId) {
                const auto& s = pair.second;
                if (!s || s->lastActiveTime == 0) {
                    continue;
                }
                const uint32_t timeoutSec = StateTimeoutSeconds(s->state);
                if (timeoutSec == 0) {
                    continue;  // Active / unknown: never expires here
                }
                if (nowMs - s->lastActiveTime >
                    static_cast<int64_t>(timeoutSec) * 1000) {
                    victims.push_back(pair.first);
                }
            }
        }
    }

    size_t destroyed = 0;
    for (const auto& sessionId : victims) {
        TerminatedInfo info;
        info.reason = "timeout";
        info.cause = 408;  // SIP Request Timeout: signaling phase never advanced
        if (MarkTerminated(sessionId, info)) {
            ++destroyed;
        }
    }
    return destroyed;
}

void SessionManager::CleanupAllSessions(const std::string& reason) {
    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    size_t total = 0;
    for (const auto& confId : confIds) {
        total += DestroyConferenceData(confId);
    }

    {
        std::lock_guard<std::mutex> lock(globalIndexLock_);
        callIdToConfId_.clear();
        callIdToSessionId_.clear();
    }

    if (total > 0) {
        VCS_GATEWAY_LOG_INFO("[SessionManager] All sessions cleaned up: count=%zu, reason=%s",
                             total, reason.c_str());
    }
}

// ============================================================================
// callId index
// ============================================================================

CallSession* SessionManager::GetSessionByCallIdRaw(int callId) {
    auto sp = GetSessionByCallId(callId);
    return sp.get();
}

bool SessionManager::AddSession(int callId, std::shared_ptr<CallSession> session) {
    if (callId < 0 || !session) return false;
    if (session->conferenceId.empty()) {
        VCS_GATEWAY_LOG_ERROR("AddSession: session has no conferenceId");
        return false;
    }
    if (!GetSessionInConference(session->conferenceId, session->sessionId)) {
        VCS_GATEWAY_LOG_ERROR("AddSession: session %s not registered in SM "
                              "(must be created via CreateSession)",
                              session->sessionId.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(globalIndexLock_);
        callIdToSessionId_[callId] = session->sessionId;
        callIdToConfId_[callId] = session->conferenceId;
    }

    VCS_GATEWAY_LOG_INFO("AddSession: callId=%d bound to sessionId=%s",
                         callId, session->sessionId.c_str());
    return true;
}

std::vector<int> SessionManager::GetActiveCallIds() const {
    std::vector<int> result;
    std::lock_guard<std::mutex> lock(
        const_cast<std::mutex&>(globalIndexLock_));
    for (const auto& pair : callIdToSessionId_) {
        result.push_back(pair.first);
    }
    return result;
}

// ============================================================================
// Statistics / diagnostics
// ============================================================================

SessionStats SessionManager::GetStats() const {
    SessionStats stats;
    stats.totalCreated = totalCreated_.load();
    stats.totalDestroyed = totalDestroyed_.load();

    std::vector<std::string> confIds;
    {
        std::lock_guard<std::mutex> lock(confRegistryMutex_);
        stats.conferenceCount = confRegistry_.size();
        confIds.reserve(confRegistry_.size());
        for (const auto& pair : confRegistry_) {
            confIds.push_back(pair.first);
        }
    }

    for (const auto& confId : confIds) {
        std::shared_ptr<ConferenceData> confData;
        {
            std::lock_guard<std::mutex> lock(confRegistryMutex_);
            auto it = confRegistry_.find(confId);
            if (it == confRegistry_.end()) continue;
            confData = it->second;
        }
        for (size_t i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
            std::lock_guard<std::mutex> shardLock(confData->shards[i]->lock);
            stats.totalSessions += confData->shards[i]->bySessionId.size();
            for (const auto& pair : confData->shards[i]->bySessionId) {
                if (pair.second->state == SessionState::Active) {
                    ++stats.activeSessions;
                }
            }
        }
    }
    return stats;
}

size_t SessionManager::GetConferenceCount() const {
    std::lock_guard<std::mutex> lock(confRegistryMutex_);
    return confRegistry_.size();
}

}} // namespace vcs::gateway
