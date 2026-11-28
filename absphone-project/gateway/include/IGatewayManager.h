#pragma once

#include "Response.h"

#include "gateway_export.h"
#include "IProtocolAdapter.h"
#include "IRtpTransport.h"
#include "EventBus.h"
#include "IMediaSdpNegotiator.h"
#include <string>
#include <memory>
#include <vector>
#include <map>

namespace vcs {

class Params;

namespace gateway {

// Forward declaration
class IProtocolAdapter;

/**
 * @brief Device information (AbsPhone compatible)
 */
struct AbsPhoneDeviceInfo {
    std::string deviceId;
    std::string memberId;
    std::string roomId;
    bool active;
    bool mainVideoActive;
    bool extVideoActive;
};

/**
 * @brief Gateway EP information (AbsPhone compatible)
 */
struct AbsPhoneGWEPInfo {
    std::map<std::string, AbsPhoneDeviceInfo> devices;
    std::string serverId;
    int index;
};

/**
 * @brief Gateway Manager interface
 *
 * Core responsibilities: Manages lifecycle of protocol adapters
 *
 * Design principles:
 * 1. GatewayManager only manages adapters, does not directly manage sessions
 * 2. Each adapter manages its own sessions
 * 3. GatewayManager only does adapter aggregation and lifecycle management
 *
 * Flow description:
 * - Inbound call: Protocol stack callback -> Adapter processing
 * - Outbound call: Business layer initiates -> Adapter initiates call
 */
class IGatewayManager {
public:
    virtual ~IGatewayManager() = default;

    // ============================================================
    // Gateway lifecycle
    // ============================================================
    virtual Response Start() = 0;
    virtual Response Stop() = 0;
    virtual bool IsRunning() const = 0;

    // ============================================================
    // Adapter management (core responsibility)
    // ============================================================

    // Register protocol adapter
    virtual void RegisterAdapter(std::shared_ptr<IProtocolAdapter> adapter) = 0;

    // Unregister protocol adapter
    virtual void UnregisterAdapter(const std::string& type) = 0;

    // Get adapter
    virtual std::shared_ptr<IProtocolAdapter> GetAdapter(const std::string& type) = 0;

    // Get all registered adapter types
    virtual std::vector<std::string> GetRegisteredAdapterTypes() const = 0;

    // ============================================================
    // Outbound call initiation (delegated to adapter)
    // ============================================================

    virtual Response InitiateCall(const std::string& protocol,
                                const std::string& callee,
                                const Params& params,
                                std::string& connectionId) = 0;

    // ============================================================
    // 会话控制（docs/08 §2.1）
    //
    // sessionId → 唯一 SessionManager 查会话 → 协议 → 对应 adapter。
    // 会议层只认 sessionId，不认识协议栈（06 §1.4.1）。
    // 错误口径：会话不存在 → NotFound；adapter 缺失 → InternalError；
    //           协议未识别 → InvalidOperation（不猜）。
    // ============================================================

    /// 应答/拒接来电。accept=false 时 reason 原样透传给 RejectCall。
    virtual Response AnswerCall(const std::string& sessionId,
                                bool accept,
                                const std::string& reason = "User rejected") = 0;

    /// 挂断会话。
    virtual Response HangupCall(const std::string& sessionId,
                                const std::string& reason = "Local hangup") = 0;

    /// 会话内 DTMF（由 adapter 决定 RFC2833 / SIP INFO）。
    virtual Response SendDTMF(const std::string& sessionId,
                              const std::string& digits) = 0;

    // ============================================================
    // 传输快照（06 §1.4.1"建立路径"，docs/08 §2.1）
    //
    // per-kind shared_ptr 值快照，锁内拷出；会话不存在 → false 且 out 为空。
    // 会议层把这快照原样交给媒体层编排，不接触 gateway 内部对象。
    // ============================================================
    virtual bool GetSessionTransports(const std::string& sessionId,
                                      MemberMediaTransports& out) const = 0;

    // ============================================================
    // Business layer integration
    // ============================================================

    // Set the platform event bus (passed to all adapters). Gateway publishes
    // signaling facts on it and never calls into upper layers.
    virtual void SetEventBus(std::shared_ptr<vcs::IEventBus> bus) = 0;

    // ============================================================
    // Application-managed media SDP (pjsua mediaAppManaged)
    //
    // Injected by the composition root before Start(). Gateway forwards it to
    // every registered adapter (and to adapters registered later), so the SIP
    // adapter can create offers/answers and validate SDP itself.
    // ============================================================
    virtual void SetMediaSdpNegotiator(
        std::shared_ptr<vcs::media_config::IMediaSdpNegotiator> negotiator) = 0;

    // ============================================================
    // Protocol stack logging
    // Forwarded to every adapter so pjsip/pjmedia logs follow the
    // application's log level without restarting the process.
    // ============================================================
    virtual void SetStackLogLevel(int level) = 0;

    // ============================================================
    // 收包门（docs/09 §2.2）
    //
    // "媒体就绪 → 传输开始收包"的门由 gateway 把守：StartTransport 在
    // 媒体启动成功返回后调用，逐 kind 打开该会话传输的接收；
    // StopTransport 为拆除路径的显式关门前置（幂等）。
    // 调用方是会议层（docs/08 §2.3）；gateway 内部不主动调用收包门。
    // ============================================================

    /// 打开 sessionId 名下全部传输的接收。会话不存在或无传输 → 错误
    ///（门没开就没有媒体，不静默）；已开 → 幂等成功。
    virtual Response StartTransport(const std::string& sessionId) = 0;

    /// 关闭 sessionId 名下全部传输的接收。任何路径下幂等成功。
    virtual Response StopTransport(const std::string& sessionId) = 0;


    // ============================================================
    // SessionView 值快照查询（docs/07 §2.4）
    // 事实源在 GatewayManager 持有的唯一 SessionManager 内。
    // ============================================================

    /// 读取单个会话快照；out.hasSession = false 表示会话不存在。
    virtual bool GetSessionView(const std::string& sessionId, SessionView& out) const = 0;

    /// 读取全部活跃会话快照。
    virtual std::vector<SessionView> GetActiveSessions() const = 0;

    // ============================================================
    // [AbsPhone Extension] Gateway registration interface
    // ============================================================
    virtual Response RegisterGW(std::string& ipAddr, int signalPort,
                                 int tcpVideoPort, int tcpAudioPort, int index) = 0;
    virtual Response UnregisterGW(int index) = 0;
    virtual Response IsGWRegistered(int index, bool& registered) = 0;

    // ============================================================
    // [AbsPhone Extension] Gateway node management
    // ============================================================
    virtual Response AddGWNode(const std::string& ip, int signalPort,
                                int tcpVideoPort, int tcpAudioPort, int index) = 0;
    virtual Response DelGWNode(int index) = 0;

    // ============================================================
    // [AbsPhone Extension] Gateway EP information query
    // ============================================================
    virtual Response GetGWEPInfo(const std::string& serverId, AbsPhoneGWEPInfo& info) = 0;
    virtual Response GetGWEPInfoByIndex(int index, AbsPhoneGWEPInfo& info) = 0;

    // ============================================================
    // [AbsPhone Extension] Gateway member invitation
    // ============================================================
    virtual Response InviteGWMember(const std::string& roomId,
                                     const std::string& callToken,
                                     const std::string& serverId,
                                     const std::string& deviceId) = 0;

    // ============================================================
    // [AbsPhone Extension] Stream control
    // ============================================================
    virtual Response EnableStream(bool enable,
                                   const std::string& serverId,
                                   const std::string& deviceId,
                                   int streamType,
                                   const void* streamInfo) = 0;
};

} // namespace gateway
} // namespace vcs
