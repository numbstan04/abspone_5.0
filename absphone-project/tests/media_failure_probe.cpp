// ============================================================================
// media_failure_probe.cpp
//
// docs/08 §5 验收 #11：按 §2.9 B 注入媒体启动的五类失败，验证"凭单条
// SystemError 即可定位归属阶段"。
//
// 手法：用假 gateway / 假 media 驱动真实的 conference::Conference，
// 事件走真实 EventBus（单派发线程），逐阶段断言 SystemError 的
// stage / op / code / sessionId / confId / severity。
//
// 本文件是开发期验证工具，不进入产品。
// ============================================================================

#include "Conference.h"
#include "EventBus.h"
#include "ILogger.h"
#include "IProtocolAdapter.h"
#include "Json.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace vcs;

int g_failures = 0;

void Check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

// ============================================================
// Fake RTP transport：快照只需要对象存在，行为无人调用
// ============================================================
class FakeTransport : public IRtpTransport {
public:
    explicit FakeTransport(const std::string& id) : id_(id) {}

    const std::string& GetType() const override { return type_; }
    const std::string& GetConnectionId() const override { return id_; }
    uint32_t GetSsrc() const override { return ssrc_; }
    void SetSsrc(uint32_t ssrc) override { ssrc_ = ssrc; }
    bool SendRtp(const RtpPacket&) override { return false; }
    bool SendRtcp(const RtcpPacket&) override { return false; }
    bool SendFir(uint32_t, uint32_t) override { return false; }
    bool IsConnected() const override { return true; }
    std::string GetRemoteAddress() const override { return "127.0.0.1:0"; }
    bool SetCallback(IRtpTransportCallback*) override { return true; }
    IRtpTransportCallback* GetCallback() const override { return nullptr; }
    void SetContext(const SessionContext& ctx) override { ctx_ = ctx; }
    const SessionContext& GetContext() const override { return ctx_; }
    bool StartReceiving() override { return true; }
    void StopReceiving() override {}
    bool IsReceiving() const override { return false; }

private:
    std::string type_ = "fake";
    std::string id_;
    uint32_t ssrc_ = 0;
    SessionContext ctx_;
};

// ============================================================
// Fake gateway：只实现 StartMediaLocked 用到的三个查询
// ============================================================
class FakeGateway : public gateway::IGatewayManager {
public:
    gateway::SessionView view;
    bool snapshotFound = true;
    bool snapshotEmpty = false;
    Response startTransport = Response::Success();

    Response Start() override { return Response::Success(); }
    Response Stop() override { return Response::Success(); }
    bool IsRunning() const override { return true; }

    void RegisterAdapter(std::shared_ptr<gateway::IProtocolAdapter>) override {}
    void UnregisterAdapter(const std::string&) override {}
    std::shared_ptr<gateway::IProtocolAdapter> GetAdapter(const std::string&) override { return nullptr; }
    std::vector<std::string> GetRegisteredAdapterTypes() const override { return {}; }

    Response InitiateCall(const std::string&, const std::string&, const Params&,
                          std::string&) override {
        return Response::Error(ResponseCode::NotImplemented, "probe");
    }
    Response AnswerCall(const std::string&, bool, const std::string&) override {
        return Response::Success();
    }
    Response HangupCall(const std::string&, const std::string&) override {
        return Response::Success();
    }
    Response SendDTMF(const std::string&, const std::string&) override {
        return Response::Success();
    }

    bool GetSessionTransports(const std::string&, MemberMediaTransports& out) const override {
        out.clear();
        if (!snapshotFound) {
            return false;
        }
        if (!snapshotEmpty) {
            out[MediaStreamKind::Audio] = std::make_shared<FakeTransport>("sip_1_audio");
            out[MediaStreamKind::Video] = std::make_shared<FakeTransport>("sip_1_video");
        }
        return true;
    }

    Response StartTransport(const std::string&) override { return startTransport; }
    Response StopTransport(const std::string&) override { return Response::Success(); }

    bool GetSessionView(const std::string&, gateway::SessionView& out) const override {
        out = view;
        return view.hasSession;
    }
    std::vector<gateway::SessionView> GetActiveSessions() const override {
        if (!view.hasSession) {
            return {};
        }
        return std::vector<gateway::SessionView>(1, view);
    }

    void SetEventBus(std::shared_ptr<IEventBus>) override {}

    Response RegisterGW(std::string&, int, int, int, int) override { return Response::Success(); }
    Response UnregisterGW(int) override { return Response::Success(); }
    Response IsGWRegistered(int, bool& registered) override {
        registered = false;
        return Response::Success();
    }
    Response AddGWNode(const std::string&, int, int, int, int) override {
        return Response::Success();
    }
    Response DelGWNode(int) override { return Response::Success(); }
    Response GetGWEPInfo(const std::string&, gateway::AbsPhoneGWEPInfo&) override {
        return Response::Success();
    }
    Response GetGWEPInfoByIndex(int, gateway::AbsPhoneGWEPInfo&) override {
        return Response::Success();
    }
    Response InviteGWMember(const std::string&, const std::string&,
                            const std::string&, const std::string&) override {
        return Response::Success();
    }
    Response EnableStream(bool, const std::string&, const std::string&, int, const void*) override {
        return Response::Success();
    }
};

// ============================================================
// Fake media：Setup / Start 结果可注入
// ============================================================
class FakeMedia : public media::IMediaControl {
public:
    Response setup = Response::Success();
    Response start = Response::Success();

    Response SetConferenceCodecConfig(const std::string&,
                                      const media_config::VideoCodecConfig&) override {
        return Response::Success();
    }
    media_config::VideoCodecConfig GetConferenceCodecConfig(const std::string&) const override {
        return media_config::VideoCodecConfig();
    }
    Response StartMedia(const std::string&, const std::string&,
                        const MemberMediaTransports&) override {
        return start;
    }
    Response StopMedia(const std::string&, const std::string&) override {
        return Response::Success();
    }
    void OnMemberCodecNegotiated(const std::string&, const std::string&,
                                 const media::NegotiatedCodec&,
                                 const media::NegotiatedCodec&) override {}
    Response GetMemberMediaInfo(const std::string&, MediaInfoSnapshot& out) const override {
        out = MediaInfoSnapshot();
        return Response::Error(ResponseCode::NotFound, "no media in probe");
    }
    Response GetConferenceMediaInfo(const std::string&, MediaInfoSnapshot& out) const override {
        out = MediaInfoSnapshot();
        return Response::Error(ResponseCode::NotFound, "no media in probe");
    }
    Response SetupMediaResources(const std::string&,
                                 const media_config::MediaResourceConfig&) override {
        return setup;
    }
    ErrorCode TeardownMediaResources(const std::string&) override { return ErrorCode::Success; }
    bool HasMediaResources(const std::string&) const override { return true; }
    void SetEventBus(std::shared_ptr<IEventBus>) override {}
    void SetObserver(media::IMediaControlObserver*) override {}
    media::IMediaControlObserver* GetObserver() const override { return nullptr; }
    void SetConfig(std::shared_ptr<base::IConfig>) override {}
    std::vector<std::string> ListVideoSources() const override { return {}; }
    std::vector<std::string> ListAudioSources() const override { return {}; }
    std::vector<std::string> ListVideoSinks() const override { return {}; }
    std::vector<std::string> ListAudioSinks() const override { return {}; }
};

// ============================================================
// 夹具：一条 Conference + 真总线 + SystemError 收集器
// ============================================================
struct Probe {
    FakeGateway gw;
    FakeMedia media;
    std::shared_ptr<EventBus> bus = std::make_shared<EventBus>();
    Subscription sub;
    std::vector<Event> systemErrors;
    std::mutex mutex;
    std::condition_variable cv;

    Probe() {
        gw.view.hasSession = true;
        gw.view.sessionId = "sip_1";
        gw.view.conferenceId = "local";
        gw.view.state = gateway::SessionState::Active;

        bus->Start();
        EventFilter filter;
        filter.allowList.push_back(EventType::SystemError);
        sub = bus->Subscribe(filter, [this](const Event& ev) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                systemErrors.push_back(ev);
            }
            cv.notify_all();
        });
    }

    ~Probe() {
        sub.Reset();
        bus->Stop();
    }

    // 驱动一次收敛（CallConnected 只作触发器），返回媒体失败信息并等待派发完成。
    conference::MediaErrorInfo Run(bool& hasError) {
        conference::Conference conf("probe", &gw, &media, bus.get());

        Event ev;
        ev.type = EventType::CallConnected;
        ev.sessionId = "sip_1";
        ev.conferenceId = "local";

        const std::vector<conference::CallEventNotification> notes =
            conf.Converge(&ev, conference::ConvergeSource::CallFact);
        hasError = false;
        conference::MediaErrorInfo info;
        for (const auto& n : notes) {
            if (n.hasMediaError) {
                hasError = true;
                info = n.mediaError;
            }
        }

        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(2),
                    [this]() { return !systemErrors.empty(); });
        return info;
    }
};

bool PayloadHas(const Event& ev, const std::string& key, const std::string& value) {
    base::Json json;
    if (!base::Json::Parse(ev.payload, json)) {
        return false;
    }
    return json.GetString(key) == value;
}

} // namespace

int main() {
    base::GetLogManager()->Initialize(nullptr, "");

    int caseNo = 0;

    // ---------- 0. 健康路径：不产生媒体失败 ----------
    {
        std::printf("case %d: healthy path\n", ++caseNo);
        Probe p;
        bool hasError = true;
        const conference::MediaErrorInfo info = p.Run(hasError);
        Check(!hasError, "no media error reported");
        Check(info.stage.empty(), "stage empty");
        Check(p.systemErrors.empty(), "no SystemError published");
    }

    // ---------- 1. SessionNotActive：正常丢弃，不产生失败 ----------
    {
        std::printf("case %d: session not active (stale CallConnected)\n", ++caseNo);
        Probe p;
        p.gw.view.state = gateway::SessionState::Disconnected;
        bool hasError = true;
        const conference::MediaErrorInfo info = p.Run(hasError);
        Check(!hasError, "no media error reported");
        Check(info.stage.empty(), "stage empty");
        Check(p.systemErrors.empty(), "no SystemError published");
    }

    // ---------- 2. SetupResources ----------
    {
        std::printf("case %d: setup resources fails\n", ++caseNo);
        Probe p;
        p.media.setup = Response::Error(ResponseCode::InternalError,
                                        "video processor initialize failed");
        bool hasError = false;
        const conference::MediaErrorInfo info = p.Run(hasError);
        Check(hasError, "hasMediaError");
        Check(info.stage == "SetupResources", "stage=SetupResources (got " + info.stage + ")");
        Check(info.op == "SetupMediaResources", "op=SetupMediaResources");
        Check(info.errorCode == static_cast<int>(ErrorCode::InternalError), "code=InternalError");
        Check(info.message == "video processor initialize failed", "message passed through");
        Check(info.sessionId == "sip_1", "sessionId=sip_1");
        Check(info.conferenceId == "local", "confId=local");
        Check(p.systemErrors.size() == 1, "exactly one SystemError");
        if (p.systemErrors.size() == 1) {
            const Event& ev = p.systemErrors.front();
            Check(ev.source == EventSource::Conference, "source=Conference");
            Check(ev.sessionId == "sip_1", "event sessionId=sip_1");
            Check(!PayloadHas(ev, "stage", "") && ev.payload.find("\"stage\":\"SetupResources\"") != std::string::npos,
                  "payload carries stage");
            Check(ev.payload.find("\"confId\":\"local\"") != std::string::npos,
                  "payload carries confId");
            Check(ev.payload.find("\"sessionId\":\"sip_1\"") != std::string::npos,
                  "payload carries sessionId");
            Check(ev.severity == EventSeverity::Error, "severity=Error");
        }
    }

    // ---------- 3. TransportSnapshotNotFound ----------
    {
        std::printf("case %d: transport snapshot not found\n", ++caseNo);
        Probe p;
        p.gw.snapshotFound = false;
        bool hasError = false;
        const conference::MediaErrorInfo info = p.Run(hasError);
        Check(hasError, "hasMediaError");
        Check(info.stage == "TransportSnapshotNotFound", "stage=TransportSnapshotNotFound (got " + info.stage + ")");
        Check(info.op == "GetSessionTransports", "op=GetSessionTransports");
        Check(info.errorCode == static_cast<int>(ErrorCode::ResourceNotFound), "code=ResourceNotFound");
        Check(info.conferenceId == "local", "confId=local");
        Check(p.systemErrors.size() == 1, "exactly one SystemError");
    }

    // ---------- 4. TransportSnapshotEmpty ----------
    {
        std::printf("case %d: transport snapshot empty\n", ++caseNo);
        Probe p;
        p.gw.snapshotEmpty = true;
        bool hasError = false;
        const conference::MediaErrorInfo info = p.Run(hasError);
        Check(hasError, "hasMediaError");
        Check(info.stage == "TransportSnapshotEmpty", "stage=TransportSnapshotEmpty (got " + info.stage + ")");
        Check(info.op == "GetSessionTransports", "op=GetSessionTransports");
        Check(info.errorCode == static_cast<int>(ErrorCode::MediaTransportNotReady), "code=MediaTransportNotReady");
        Check(p.systemErrors.size() == 1, "exactly one SystemError");
    }

    // ---------- 5. StartMedia ----------
    {
        std::printf("case %d: media start fails\n", ++caseNo);
        Probe p;
        p.media.setup = Response::Error(ResponseCode::Conflict, "already exists");  // 幂等放行
        p.media.start = Response::Error(ResponseCode::InternalError, "device open failed");
        bool hasError = false;
        const conference::MediaErrorInfo info = p.Run(hasError);
        Check(hasError, "hasMediaError");
        Check(info.stage == "StartMedia", "stage=StartMedia (got " + info.stage + ")");
        Check(info.op == "StartMedia", "op=StartMedia");
        Check(info.errorCode == static_cast<int>(ErrorCode::InternalError), "code=InternalError");
        Check(p.systemErrors.size() == 1, "exactly one SystemError");
        if (p.systemErrors.size() == 1) {
            Check(p.systemErrors.front().payload.find("\"kinds\":\"audio,video\"") != std::string::npos,
                  "payload carries kinds=audio,video");
        }
    }

    // ---------- 6. StartTransport（Warning 等级） ----------
    {
        std::printf("case %d: receive gate fails\n", ++caseNo);
        Probe p;
        p.gw.startTransport = Response::Error(ResponseCode::InternalError,
                                               "StartReceiving failed");
        bool hasError = false;
        const conference::MediaErrorInfo info = p.Run(hasError);
        Check(hasError, "hasMediaError");
        Check(info.stage == "StartTransport", "stage=StartTransport (got " + info.stage + ")");
        Check(info.op == "StartTransport", "op=StartTransport");
        Check(info.errorCode == static_cast<int>(ErrorCode::InternalError), "code=InternalError");
        Check(p.systemErrors.size() == 1, "exactly one SystemError");
        if (p.systemErrors.size() == 1) {
            Check(p.systemErrors.front().severity == EventSeverity::Warning,
                  "severity=Warning (media already up)");
        }
    }

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
