#pragma once

// _GNU_SOURCE is defined via CMakeLists.txt

#include "IGatewayManager.h"
#include "IProtocolAdapter.h"
#include "CallSession.h"
#include "SessionManager.h"   // Owned by GatewayManager (single instance)
#include "GatewayFactory.h"
#include "EventBus.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vcs { namespace gateway {

/**
 * @brief Gateway Manager
 *
 * Core responsibilities: Manages the lifecycle of protocol adapters and owns
 * the single SessionManager (docs/07 会话层施工图).
 *
 * Design principles:
 * 1. GatewayManager owns exactly one SessionManager — the single source of
 *    truth for all sessions and the single publish point for call events
 *    (CallIncoming / CallConnected / CallTerminated via Mark*).
 * 2. Adapters receive a non-owning pointer to that SM at registration time.
 *    Per-conference SM fan-out (V5.9/V6.0) was removed: session isolation is
 *    the SM's own L1 conference sharding, not N SM instances.
 * 3. Adapters do not own sessions; all session CRUD is delegated to SessionManager.
 * 4. GatewayManager implements IProtocolAdapterCallback to receive adapter notifications.
 */
class GatewayManager : public IGatewayManager, public IProtocolAdapterCallback {
public:
    GatewayManager();
    ~GatewayManager() override;

    // ============================================================
    // IGatewayManager Implementation
    // ============================================================
    Response Start() override;
    Response Stop() override;
    bool IsRunning() const override;

    // Adapter management
    void RegisterAdapter(std::shared_ptr<IProtocolAdapter> adapter) override;
    void UnregisterAdapter(const std::string& type) override;
    std::shared_ptr<IProtocolAdapter> GetAdapter(const std::string& type) override;
    std::vector<std::string> GetRegisteredAdapterTypes() const override;

    // ============================================================
    // 唯一 SessionManager 访问器（注入用；业务代码一律走 IGatewayManager 查询）
    // ============================================================
    SessionManager* GetSessionManager() const { return sessionManager_.get(); }

    // ============================================================
    // SessionView 值快照查询（docs/07 §2.4）
    // ============================================================
    bool GetSessionView(const std::string& sessionId, SessionView& out) const override;
    std::vector<SessionView> GetActiveSessions() const override;

    // Outbound call initiation
    Response InitiateCall(const std::string& protocol,
                         const std::string& callee,
                         const Params& params,
                         std::string& connectionId) override;

    // ============================================================
    // 会话控制（docs/08 §2.1）：sessionId → SM 快照 → 协议 → adapter
    // ============================================================
    Response AnswerCall(const std::string& sessionId,
                        bool accept,
                        const std::string& reason = "User rejected") override;
    Response HangupCall(const std::string& sessionId,
                        const std::string& reason = "Local hangup") override;
    Response SendDTMF(const std::string& sessionId,
                      const std::string& digits) override;
    bool GetSessionTransports(const std::string& sessionId,
                              MemberMediaTransports& out) const override;

    // Business layer integration
    // [过渡] Set media control（08 号片删除：媒体不再经由 gateway 转手）
    void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) override;

    // 收包门（docs/09 §2.2）
    Response StartTransport(const std::string& sessionId) override;
    Response StopTransport(const std::string& sessionId) override;

    // ============================================================
    // IProtocolAdapterCallback Implementation
    // ============================================================
    void OnAdapterSessionCountChanged(const std::string& adapterType,
                                          size_t activeCount) override;
    void OnAdapterError(const std::string& adapterType,
                        int errorCode,
                        const std::string& errorMsg) override;

    // ============================================================
    // [AbsPhone Extension] Gateway registration interface
    // ============================================================
    Response RegisterGW(std::string& ipAddr, int signalPort,
                      int tcpVideoPort, int tcpAudioPort, int index) override;
    Response UnregisterGW(int index) override;
    Response IsGWRegistered(int index, bool& registered) override;

    // ============================================================
    // [AbsPhone Extension] Gateway node management
    // ============================================================
    Response AddGWNode(const std::string& ip, int signalPort,
                      int tcpVideoPort, int tcpAudioPort, int index) override;
    Response DelGWNode(int index) override;

    // ============================================================
    // [AbsPhone Extension] Gateway EP information query
    // ============================================================
    Response GetGWEPInfo(const std::string& serverId, AbsPhoneGWEPInfo& info) override;
    Response GetGWEPInfoByIndex(int index, AbsPhoneGWEPInfo& info) override;

    // ============================================================
    // [AbsPhone Extension] Gateway member invitation
    // ============================================================
    Response InviteGWMember(const std::string& roomId,
                          const std::string& callToken,
                          const std::string& serverId,
                          const std::string& deviceId) override;

    // ============================================================
    // [AbsPhone Extension] Stream control
    // ============================================================
    Response EnableStream(bool enable,
                        const std::string& serverId,
                        const std::string& deviceId,
                        int streamType,
                        const void* streamInfo) override;

private:
    // sessionId → SM 快照 → 协议 → adapter；失败时填充 error 并返回 nullptr
    std::shared_ptr<IProtocolAdapter> FindAdapterForSession(
        const std::string& sessionId, Response& error);

    // State
    bool running_;
    std::shared_ptr<vcs::IEventBus> eventBus_;

    // Factory (for creating default adapters)
    GatewayFactory factory_;

    // Adapter map <type, adapter>
    std::unordered_map<std::string, std::shared_ptr<IProtocolAdapter>> adapters_;

    // The single source of truth (docs/07)
    std::unique_ptr<SessionManager> sessionManager_;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace gateway
} // namespace vcs
