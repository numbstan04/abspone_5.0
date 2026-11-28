/**
 * @file SessionManager.h
 * @brief Session Manager - 唯一事实源 + 唯一呼叫事件发布点
 *
 * [Design Notes - docs/07 会话层施工图]
 * 1. 唯一事实源：所有 CallSession 由 SessionManager 创建/查询/销毁，
 *    适配层（SIP/H.323）不再维护任何 session 映射表。
 * 2. 唯一发布点：CallOutgoing / CallIncoming / CallConnected / CallTerminated
 *    四个事实事件只能由 MarkOutgoing / MarkIncoming / MarkConnected /
 *    MarkTerminated 触发，语义 = 状态提交 + 事件发布，二者原子完成。
 *    事件到达时快照已齐全、会话已清理（CallTerminated）。
 * 3. 2-level shard lock model:
 *    - L1: sharded by conferenceId (different conferences have no lock contention)
 *    - L2: 8 buckets per conference (sharded by sessionId hash)
 * 4. Global callId index (single lock) for cross-conference queries
 *
 * [Mark 语义约定]
 * - MarkOutgoing:  仅 OutgoingPending 状态有效；提交 callee 并发布
 *                  CallOutgoing（重复调用幂等跳过）。
 * - MarkIncoming:  仅 IncomingPending 状态有效；提交 caller 并发布 CallIncoming。
 * - MarkConnected: 首次调用发布 CallConnected（重复调用幂等跳过），
 *                  状态推进到 Active 并补齐 connectTime。
 * - MarkTerminated: 恰好一次。会话存在 => 取快照、拆传输、删索引、
 *                  停传输（锁外）、发布 CallTerminated；
 *                  会话不存在（已终止）=> 幂等 no-op。
 *
 * [Thread Safety]
 * - All public methods are thread-safe
 * - Operations within the same conference+bucket are serialized;
 *   different conferences / different buckets can run in parallel
 */

#pragma once

#include "CallSession.h"
#include "SessionContext.h"
#include "IRtpTransport.h"
#include "EventBus.h"
#include <string>
#include <memory>
#include <map>
#include <unordered_map>
#include <set>
#include <mutex>
#include <functional>
#include <chrono>
#include <atomic>
#include <array>
#include <vector>

namespace vcs { namespace gateway {

/**
 * @brief Session configuration
 */
struct SessionConfig {
    std::string conferenceId;      // Conference ID
    std::string callerUri;         // Caller URI
    std::string callerIp;          // Caller IP
    uint16_t localAudioPort = 0;   // Local audio port
    uint16_t localVideoPort = 0;   // Local video port
    uint16_t localSecondaryVideoPort = 0; // Local secondary-video port
    uint32_t keepaliveInterval = 30; // Keepalive interval (seconds)

    ProtocolType protocol = ProtocolType::UNKNOWN;
};

/**
 * @brief 呼出信息（CallOutgoing = calleeUri）
 *
 * 发起期状态回调：会话在协议栈接受呼叫后由 OutgoingPending 建立，此刻
 * 上层拿到的只是一句 "调用已受理"；没有这条事实事件，只订阅事件的观察者
 * 在接通/失败之前看不到任何"呼出中"的痕迹。
 */
struct OutgoingInfo {
    std::string calleeUri;
};

/**
 * @brief 呼入信息（docs/07 §2.1 payload 契约：CallIncoming = callerUri + hasVideo）
 */
struct IncomingInfo {
    std::string callerUri;
    bool hasVideo = false;
};

/**
 * @brief 接通信息（CallConnected = remoteUri + remoteAddress + hasVideo + codecs）
 */
struct ConnectedInfo {
    std::string remoteUri;
    std::string remoteAddress;
    bool hasVideo = false;
    vcs::NegotiatedCodec audioCodec;
    vcs::NegotiatedCodec videoCodec;
};

/**
 * @brief 终止信息（CallTerminated = reason + cause [+ 派生的 remoteUri/
 *        remoteAddress/durationSec，由快照计算，不由调用方填写]）
 */
struct TerminatedInfo {
    std::string reason;
    int cause = 0;
};

/**
 * @brief Session statistics information
 */
struct SessionStats {
    size_t totalSessions = 0;      // Current total session count
    size_t activeSessions = 0;     // Active session count
    size_t conferenceCount = 0;    // Conference count
    size_t totalCreated = 0;       // Cumulative created count
    size_t totalDestroyed = 0;     // Cumulative destroyed count
};

/**
 * @brief AttachTransport 的结果
 *
 * - Attached: 槽位原为空（或只有地址占位），本次写入建立了新的登记项。
 * - Reused:   同一对象已在槽位，只刷新了远端地址/端口。
 * - Replaced: 槽位原有一个不同的对象，已用新对象替换；旧对象在锁外停止。
 * - Rejected: 会话不存在（或参数不合法），没有做任何修改。
 */
enum class TransportAttachResult {
    Attached,
    Reused,
    Replaced,
    Rejected
};

/**
 * @brief Session Manager - 唯一事实源
 */
class SessionManager {
public:
    /**
     * @brief Fixed number of shards per conference (L2)
     */
    static constexpr int SHARDS_PER_CONFERENCE = 8;

    /**
     * @brief 默认会议 ID（无显式会议归属的呼叫都落在 "local"）
     */
    static constexpr const char* kDefaultConferenceId = "local";

    /**
     * @brief Constructor (protocol-independent, SessionManager is protocol-agnostic)
     */
    SessionManager();

    /**
     * @brief Destructor
     */
    ~SessionManager();

    // Disable copy and move
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // ============================================================================
    // 事件总线注入（docs/07：SM 是呼叫事件唯一发布点，总线由 GatewayManager 注入）
    // ============================================================================

    /**
     * @brief 注入平台事件总线（必须在任何 Mark* 之前完成）
     */
    void SetEventBus(const std::shared_ptr<vcs::IEventBus>& bus);

    // ============================================================================
    // Session creation（sessionId 由 SM 生成：sip_1 / h323_2 / webrtc_3 ...）
    // ============================================================================

    /**
     * @brief 创建会话并生成 sessionId
     * @param config       会话配置（conferenceId 为空时落 kDefaultConferenceId）
     * @param initialState 初始状态：呼入 IncomingPending / 呼出 OutgoingPending
     * @return 新建的 CallSession（失败返回 nullptr）
     */
    std::shared_ptr<CallSession> CreateSession(
        const SessionConfig& config,
        SessionState initialState = SessionState::IncomingPending);

    // ============================================================================
    // 状态迁移 + 事件发布（三个语义入口，docs/07 §2.2）
    // ============================================================================

    /**
     * @brief 呼出登记：提交 callee，发布 CallOutgoing（首次调用；幂等）
     *
     * 必须在协议栈已接受呼叫、会话以 OutgoingPending 建立之后调用。
     * @return false = 会话不存在或当前状态不是 OutgoingPending
     */
    bool MarkOutgoing(const std::string& sessionId, const OutgoingInfo& info);

    /**
     * @brief 呼入登记：提交 caller，发布 CallIncoming
     * @return false = 会话不存在或当前状态不允许（已发布过/已终止）
     */
    bool MarkIncoming(const std::string& sessionId, const IncomingInfo& info);

    /**
     * @brief 接通：状态推进 Active，首次调用发布 CallConnected（幂等）
     * @return false = 会话不存在
     */
    bool MarkConnected(const std::string& sessionId, const ConnectedInfo& info);

    /**
     * @brief 终止：快照 -> 锁内拆传输/删索引 -> 锁外停传输 -> 发布 CallTerminated
     * @return false = 会话已不存在（幂等 no-op）
     */
    bool MarkTerminated(const std::string& sessionId, const TerminatedInfo& info);

    // ============================================================================
    // Session query (by sessionId)
    // ============================================================================

    /**
     * @brief Get CallSession by session ID
     * @note Conference ID is required to locate L1; if unknown, scans all
     *       conferences (slower path)
     */
    std::shared_ptr<CallSession> GetSession(const std::string& sessionId);

    /**
     * @brief Get by session ID and conference ID (fast path)
     */
    std::shared_ptr<CallSession> GetSessionInConference(const std::string& conferenceId,
                                                        const std::string& sessionId);

    /**
     * @brief Get CallSession by callId (via global index)
     */
    std::shared_ptr<CallSession> GetSessionByCallId(int callId);

    /**
     * @brief Get CallSession by member ID
     */
    std::shared_ptr<CallSession> GetSessionByMemberId(const std::string& memberId);

    /**
     * @brief Get all sessions for the specified conference
     */
    std::vector<std::shared_ptr<CallSession>> GetSessionsByConference(
        const std::string& conferenceId);

    /**
     * @brief Get sessions of the specified conference by state
     */
    std::vector<std::shared_ptr<CallSession>> GetSessionsByConferenceAndState(
        const std::string& conferenceId, SessionState state);

    /**
     * @brief Get sessions across all conferences by state
     */
    std::vector<std::shared_ptr<CallSession>> GetSessionsByState(SessionState state);

    /**
     * @brief Get all sessions
     */
    std::vector<std::shared_ptr<CallSession>> GetAllSessions() const;

    /**
     * @brief Get active session count
     */
    size_t GetActiveSessionCount() const;

    /**
     * @brief Get active session count for a specific conference
     */
    size_t GetActiveSessionCount(const std::string& conferenceId) const;

    /**
     * @brief Get session count for a specific state
     */
    size_t GetSessionCountByState(SessionState state) const;

    // ============================================================================
    // 值快照查询（docs/07 §2.4：对外只读视图，不暴露会话内部可变对象）
    // ============================================================================

    /**
     * @brief 读取单个会话快照
     * @return false = 会话不存在（out.hasSession = false）
     */
    bool GetSessionView(const std::string& sessionId, SessionView& out) const;

    /**
     * @brief 读取全部活跃会话快照
     */
    std::vector<SessionView> GetSessionViews() const;

    // ============================================================================
    // 业务逻辑
    // ============================================================================

    /**
     * @brief 会话-传输登记的唯一写入口（docs/07 §2.6）
     *
     * 所有传输写入（适配器登记、地址占位、re-CONFIRMED 刷新）都必须走这里。
     * 调用方持有 shared_ptr<CallSession> 但没有锁；直接改 transports 会与拆链、
     * 与其他写者竞争。这里的规则：
     *
     * - session 不存在 => Rejected，什么都不改。
     * - transport 非空、槽位为空 => Attached（建立登记项，首项自动 isPrimary）。
     * - transport 非空且与槽位内对象相同 => Reused（只刷新地址/端口）。
     * - transport 非空且与槽位内对象不同 => Replaced（槽位换成新对象，
     *   旧对象在本方法返回前、锁外 StopReceiving；调用方仍可从返回值判断）。
     * - transport 为空 => 只做"地址占位"：槽位有对象时保持对象不动，槽位为空
     *   时建立 transportObj == nullptr 的登记项。绝不因为传空而清掉活对象。
     * - remoteAddress 为空 / remotePort 为 0 => 保留槽位当前值。
     *
     * @param sessionId     目标会话
     * @param kind          媒体流种类（一路一个对象）
     * @param transport     传输对象，可为空（仅地址占位）
     * @param remoteAddress 远端地址（空 = 保持当前值）
     * @param remotePort    远端端口（0 = 保持当前值）
     */
    TransportAttachResult AttachTransport(
        const std::string& sessionId,
        MediaStreamKind kind,
        std::shared_ptr<IRtpTransport> transport,
        const std::string& remoteAddress = "",
        uint16_t remotePort = 0);

    /**
     * @brief Get the media transport by sessionId.
     */
    std::shared_ptr<IRtpTransport> GetTransport(const std::string& sessionId);

    /**
     * @brief Get the media transport by memberId.
     */
    std::shared_ptr<IRtpTransport> GetTransportByMemberId(const std::string& memberId);

    /**
     * @brief Get every media transport of a given conference.
     */
    std::vector<std::shared_ptr<IRtpTransport>> GetTransportsByConference(
        const std::string& conferenceId);

    /**
     * @brief Per-kind transport snapshot of one session (docs/08 §2.1)
     *
     * Copies Audio/Video/... transport shared_ptrs out under the shard lock.
     * The conference layer hands this snapshot straight to the media layer;
     * holding the shared_ptrs keeps the transports alive independently of
     * session teardown (06 §1.4.1).
     *
     * @return false if the session does not exist (out emptied either way).
     */
    bool GetTransportSnapshot(const std::string& sessionId,
                              MemberMediaTransports& out);

    /**
     * @brief Set the session context
     */
    bool SetContext(const std::string& sessionId, const SessionContext& context);

    /**
     * @brief Activate a session
     */
    bool ActivateSession(const std::string& sessionId);

    /**
     * @brief Pause a session
     */
    bool PauseSession(const std::string& sessionId);

    /**
     * @brief Get session context
     */
    SessionContext* GetContext(const std::string& sessionId);

    /**
     * @brief Update session state（中间态迁移用；终态一律走 MarkTerminated）
     */
    bool UpdateSessionState(const std::string& sessionId, SessionState newState);

    // ============================================================================
    // Session traversal
    // ============================================================================

    /**
     * @brief Traverse sessions in the specified conference
     *
     * Sessions are copied under the shard lock and the callback runs outside
     * it, so the callback may call back into SessionManager. The snapshot is
     * a point-in-time view: sessions created or destroyed afterwards are not
     * reflected.
     */
    void ForEachSessionInConference(const std::string& conferenceId,
        std::function<bool(std::shared_ptr<CallSession>)> callback);

    /**
     * @brief Traverse all sessions
     */
    void ForEachSession(std::function<bool(std::shared_ptr<CallSession>)> callback);

    /**
     * @brief Find the first session matching a predicate
     */
    std::shared_ptr<CallSession> FindSession(
        std::function<bool(std::shared_ptr<CallSession>)> predicate);

    // ============================================================================
    // 超时清理（docs/07 §2.5：策略表内聚在 SM，调用方不再传超时秒数）
    // ============================================================================

    /**
     * @brief 按内聚策略表清理超时会话，每个超时会话走 MarkTerminated（发布事件）
     * @return 本次清理的会话数
     *
     * 策略表：IncomingPending 32s / OutgoingPending 32s / Connecting 32s /
     *         Disconnecting 5s / Active 不超时
     */
    size_t CleanupTimeoutSessions();

    /**
     * @brief 停机路径批量清理（不发布事件，锁外停传输）
     */
    void CleanupAllSessions(const std::string& reason = "Shutdown");

    // ============================================================================
    // callId index related methods
    // ============================================================================

    /**
     * @brief Get a raw pointer by callId (internal use)
     */
    CallSession* GetSessionByCallIdRaw(int callId);

    /**
     * @brief Register a session into the callId index
     */
    bool AddSession(int callId, std::shared_ptr<CallSession> session);

    /**
     * @brief Get all active callIds
     */
    std::vector<int> GetActiveCallIds() const;

    // ============================================================================
    // Statistics / diagnostics
    // ============================================================================

    /**
     * @brief Get statistics
     */
    SessionStats GetStats() const;

    /**
     * @brief Get conference count
     */
    size_t GetConferenceCount() const;

private:
    // ============================================================================
    // Internal data structures
    // ============================================================================

    /**
     * @brief Single shard (8 per conference)
     */
    struct SessionShard {
        mutable std::mutex lock;
        // sessionId -> CallSession
        std::unordered_map<std::string, std::shared_ptr<CallSession>> bySessionId;
        // memberId -> sessionId
        std::unordered_map<std::string, std::string> memberIdToSessionId;
        // sessionId -> SessionContext
        std::unordered_map<std::string, SessionContext> contexts;
        // state -> {sessionIds}（高效状态查询与超时清理）
        std::unordered_map<SessionState, std::set<std::string>> stateIndex;
        // MarkConnected 幂等守卫：已发布过 CallConnected 的 sessionId 集合
        std::set<std::string> connectedPublished;
        // MarkOutgoing 幂等守卫：已发布过 CallOutgoing 的 sessionId 集合
        std::set<std::string> outgoingPublished;
    };

    /**
     * @brief Per-conference data (8 shards)
     */
    struct ConferenceData {
        std::array<std::shared_ptr<SessionShard>, SHARDS_PER_CONFERENCE> shards;
        ConferenceData() {
            for (int i = 0; i < SHARDS_PER_CONFERENCE; ++i) {
                shards[i] = std::make_shared<SessionShard>();
            }
        }
    };

    /**
     * @brief Compute shard index from sessionId string
     */
    static size_t ShardIndex(const std::string& sessionId) {
        return std::hash<std::string>{}(sessionId) % SHARDS_PER_CONFERENCE;
    }

    /**
     * @brief Get or create conference data (called within L1 lock)
     */
    std::shared_ptr<ConferenceData> GetOrCreateConferenceData(
        const std::string& conferenceId);

    /**
     * @brief Get conference data (returns nullptr if not present)
     */
    std::shared_ptr<ConferenceData> GetConferenceData(const std::string& conferenceId) const;

    /**
     * @brief Destroy conference data (called within L1 lock)
     * @return Number of destroyed sessions
     */
    size_t DestroyConferenceData(const std::string& conferenceId);

    /**
     * @brief Internal destruction (caller already holds the lock)
     * @return Detached transports; caller MUST stop them AFTER releasing the
     *         shard lock (receive thread may need the same lock to deliver).
     */
    std::vector<std::shared_ptr<IRtpTransport>> DestroySessionInShard(
        ConferenceData& confData,
        size_t shardIdx,
        const std::string& sessionId,
        const std::string& reason);

    /**
     * @brief 批量从全局 callId 索引移除（一次加锁）
     */
    void RemoveFromCallIdIndex(const std::vector<int>& callIds);

    /**
     * @brief 从全局 callId 索引移除单个条目
     *
     * 只做两次 map erase，不构造临时 std::vector：调用方（挂断路径，
     * 运行在 pjsip 回调线程）不再为 4 字节的 callId 做一次堆分配。
     */
    void RemoveFromCallIdIndex(int callId);

    /**
     * @brief sessionId 生成：{sip|h323|webrtc|sess}_{自增序号}
     */
    std::string GenerateSessionId(ProtocolType protocol);

    // ============================================================================
    // Members
    // ============================================================================

    // L1: Conference registry
    mutable std::mutex confRegistryMutex_;
    std::unordered_map<std::string, std::shared_ptr<ConferenceData>> confRegistry_;

    // Global callId index (for fast lookup by callId)
    mutable std::mutex globalIndexLock_;
    std::unordered_map<int, std::string> callIdToConfId_;  // callId -> conferenceId
    std::unordered_map<int, std::string> callIdToSessionId_;  // callId -> sessionId

    // 呼叫事件唯一发布点持有的总线
    std::shared_ptr<vcs::IEventBus> eventBus_;

    // Statistics
    std::atomic<uint64_t> totalCreated_{0};
    std::atomic<uint64_t> totalDestroyed_{0};

    // sessionId 生成序号
    std::atomic<uint64_t> sessionIdCounter_{0};
};

}} // namespace vcs::gateway
