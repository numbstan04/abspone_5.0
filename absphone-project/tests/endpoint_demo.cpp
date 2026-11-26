// ============================================================================
// endpoint_demo.cpp
//
// Endpoint demo for the AbsPhone library.
//
// Purpose:
//   DEVELOPMENT AND DEBUGGING TOOL that drives the AbsPhone library from a
//   PC. NOT shipped to production platforms. Each production platform (Qt on
//   Windows / embedded Linux, JNI on Android, ObjC++ on iOS) writes its own
//   UI and calls AbsPhone directly.
//
// Architecture:
//   - IDemoUi         : UI channel abstraction used ONLY inside this demo.
//   - ConsoleUi       : console implementation (first version).
//   - WebUI           : WebSocket implementation (second version, placeholder).
//   - DemoObserver    : implements AbsPhoneObserver, forwards signaling
//                       callbacks to DemoApp.
//   - DemoApp         : holds AbsPhone*, dispatches commands to the AbsPhone
//                       API, emits events to IDemoUi.
//   - main            : wiring.
//
// Notes:
//   - Media does NOT flow through AbsPhone. Media flows through the gateway
//     module internally. The demo does not touch media.
//   - Boost is used for cross-platform support. No platform macros.
//   - All comments in English.
//   - JSON uses vcs::base::Json (base module).
// ============================================================================

#include "IAbsPhone.h"
#include "ILogger.h"
#include "Json.h"


#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

class DemoApp;
using namespace vcs::absphone;
using namespace vcs::base;


// ============================================================================
// Global state
// ============================================================================
static std::atomic<bool> g_running{true};

static void SignalHandler(int /*sig*/) {
    g_running = false;
}

// ============================================================================
// IDemoUi
//
// UI channel abstraction used ONLY inside this demo. It lets the demo
// switch between console and Web UI without touching the business logic.
// Production platforms do NOT use this interface; they implement their
// own UI directly on top of AbsPhone.
// ============================================================================
class IDemoUi {
public:
    virtual ~IDemoUi() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;

    // demo -> UI. Thread-safe: may be called from an AbsPhone callback thread.
    virtual void SendEvent(const std::string& json) = 0;

    // UI -> demo. The handler is called from the UI's thread.
    virtual void SetCommandHandler(std::function<void(const std::string&)> cb) = 0;
};

// ============================================================================
// ConsoleUi
//
// Command-line UI. Reads text commands from stdin, converts them to JSON,
// and passes them to the command handler. Events received from DemoApp are
// printed to stdout.
// ============================================================================
class ConsoleUi : public IDemoUi {
public:
    ConsoleUi() = default;
    ~ConsoleUi() override { Stop(); }

    void Start() override {
        PrintHelp();
        thread_ = std::thread([this]() { RunLoop(); });
    }

    void Stop() override {
        // Detach to avoid blocking on a getline call that may still be waiting
        // on stdin. The process is about to exit; the OS reclaims the thread.
        if (thread_.joinable()) {
            thread_.detach();
        }
    }

    void SendEvent(const std::string& jsonStr) override {
        std::lock_guard<std::mutex> lock(outputMutex_);
        Json j;
        if (!Json::Parse(jsonStr, j)) {
            std::cout << "\n[EVENT] " << jsonStr << "\n> " << std::flush;
            return;
        }
        const std::string type = j.GetString("type");

        if (type == "incoming_sip_call") {
            std::cout << "\n[EVENT] Incoming SIP call from "
                      << j.GetString("caller") << "\n"
                      << "        type 'answer' or 'reject'\n> " << std::flush;
        } else if (type == "call_established") {
            std::cout << "\n[EVENT] Call established: "
                      << j.GetString("remoteParty")
                      << " @ " << j.GetString("remoteAddress")
                      << " proto=" << j.GetInt("proto")
                      << " hasVideo=" << (j.GetBool("hasVideo") ? "yes" : "no")
                      << "\n> " << std::flush;
        } else if (type == "call_cleared") {
            std::cout << "\n[EVENT] Call cleared: reason="
                      << j.GetInt("reason")
                      << " duration=" << j.GetInt("duration") << "s"
                      << "\n> " << std::flush;
        } else if (type == "codec") {
            std::cout << "\n[EVENT] Codec: " << j.GetString("name")
                      << " " << (j.GetBool("isSend") ? "send" : "recv")
                      << "\n> " << std::flush;
        } else if (type == "system") {
            std::cout << "\n[EVENT] System: " << j.GetString("message")
                      << "\n> " << std::flush;
        } else if (type == "error") {
            std::cout << "\n[EVENT] Error: " << j.GetString("message")
                      << "\n> " << std::flush;
        } else {
            std::cout << "\n[EVENT] " << jsonStr << "\n> " << std::flush;
        }
    }

    void SetCommandHandler(std::function<void(const std::string&)> cb) override {
        handler_ = std::move(cb);
    }

private:
    void RunLoop() {
        std::string line;
        while (g_running && std::getline(std::cin, line)) {
            if (line.empty()) {
                std::cout << "> " << std::flush;
                continue;
            }
            const std::string jsonStr = TextToJson(line);
            if (!jsonStr.empty() && handler_) {
                handler_(jsonStr);
            }
            if (g_running) {
                std::cout << "> " << std::flush;
            }
        }
        g_running = false;
    }

    // Convert a text command line into a JSON string.
    // Examples:
    //   "call SIP:1001@1.2.3.4" -> {"cmd":"call","uri":"SIP:1001@1.2.3.4"}
    //   "answer"                -> {"cmd":"answer"}
    //   "dtmf 1234"             -> {"cmd":"dtmf","digits":"1234"}
    static std::string TextToJson(const std::string& line) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) return "";

        Json j;
        j.Set("cmd", cmd);

        if (cmd == "call") {
            std::string uri;
            iss >> uri;
            j.Set("uri", uri);
        } else if (cmd == "dtmf") {
            std::string digits;
            iss >> digits;
            j.Set("digits", digits);
        }
        return j.Dump();
    }

    static void PrintHelp() {
        std::cout <<
            "Commands:\n"
            "  call <uri>          e.g. call SIP:1001@172.16.40.45\n"
            "  answer | a          answer incoming SIP call\n"
            "  reject | r          reject incoming SIP call\n"
            "  hangup | hu         hangup current call\n"
            "  mute                mute microphone\n"
            "  unmute              unmute microphone\n"
            "  video_off           disable video\n"
            "  video_on            enable video\n"
            "  dtmf <digits>       send DTMF digits\n"
            "  status              print call status\n"
            "  help | h            print this help\n"
            "  quit | q            exit\n"
            "\n> " << std::flush;
    }

    std::thread thread_;
    std::mutex outputMutex_;
    std::function<void(const std::string&)> handler_;
};

// ============================================================================
// WebUI (second version, placeholder)
//
// class WebUI : public IDemoUi {
//     // TODO: Boost.Beast WebSocket server implementation.
//     // It will speak the same JSON commands / events as ConsoleUi.
// };
// ============================================================================

// ============================================================================
// DemoApp
//
// Holds AbsPhone* and IDemoUi*. Dispatches JSON commands to the AbsPhone API
// and forwards JSON events to the UI. Commands go straight to the AbsPhone
// API; no business wrapper is added on top.
// ============================================================================
class DemoApp {
public:
	DemoApp(AbsPhone* engine, IDemoUi* ui) : engine_(engine), ui_(ui) {}

	void EmitEvent(const Json& j) {
		if (ui_) ui_->SendEvent(j.Dump());
	}

	void HandleCommand(const std::string& jsonStr) {
		Json j;
		if (!Json::Parse(jsonStr, j)) {
			Json e;
			e.Set("type", "error");
			e.Set("message", "parse error");
			EmitEvent(e);
			return;
		}

		const std::string cmd = j.GetString("cmd");

		if (cmd == "call") {
			const std::string uri = j.GetString("uri");
			if (uri.empty()) {
				Json e;
				e.Set("type", "error");
				e.Set("message", "call: missing uri");
				EmitEvent(e);
				return;
			}
			ErrCode ec = engine_->MakeCall(uri);
			if (ec != ErrCode::e_ErrNone) {
				Json e;
				e.Set("type", "error");
				e.Set("message", "MakeCall failed");
				e.Set("code", static_cast<int>(ec));
				EmitEvent(e);
			}
		}
		else if (cmd == "answer") {
			engine_->SipAnswerCall(true);
		}
		else if (cmd == "reject") {
			engine_->SipAnswerCall(false);
		}
		else if (cmd == "hangup") {
			engine_->ClearCall();
		}
		else if (cmd == "mute") {
			engine_->MuteAudio(AudioDeviceType::e_RecordDevice, true);
		}
		else if (cmd == "unmute") {
			engine_->MuteAudio(AudioDeviceType::e_RecordDevice, false);
		}
		else if (cmd == "video_off") {
			engine_->MuteVideo(true);
		}
		else if (cmd == "video_on") {
			engine_->MuteVideo(false);
		}
		else if (cmd == "dtmf") {
			engine_->SendSipDTMF(j.GetString("digits"));
		}
		else if (cmd == "status") {
			Json e;
			e.Set("type", "system");
			e.Set("message", std::string("callDuration=") +
				std::to_string(engine_->GetCallDuration()) + "s");
			EmitEvent(e);
		}
		else if (cmd == "quit" || cmd == "exit") {
			g_running = false;
		}
		else {
			Json e;
			e.Set("type", "error");
			e.Set("message", "unknown command: " + cmd);
			EmitEvent(e);
		}
	}

private:
	AbsPhone* engine_;
	IDemoUi* ui_;
};

// ============================================================================
// DemoObserver
//
// Implements AbsPhoneObserver. Signaling callbacks are forwarded to DemoApp
// as JSON events. Media callbacks are empty: media does NOT flow through
// AbsPhone.
// ============================================================================
class DemoObserver : public AbsPhoneObserver {
public:
    explicit DemoObserver(DemoApp* app) : app_(app) {}

    // ---- Common ----

    bool OnGetEndpointParam(string keyUsername, string keyHostname,
                            string param, string& value) override {
        (void)keyUsername; (void)keyHostname; (void)param; (void)value;
        return true;
    }

    // ---- Signaling ----

    void OnAnswerCall(string caller) override {
        // VCS / H323 path. The demo targets SIP, so this is a no-op.
        (void)caller;
    }

    void OnSipAnswerCall(string caller) override {
        Json j;
        j.Set("type", "incoming_sip_call");
        j.Set("caller", caller);
        app_->EmitEvent(j);
    }

    void OnConnectionEstablished(string remoteParty, string remoteAddress,
                                 ProtoType proto, bool hasVideo) override {
        Json j;
        j.Set("type", "call_established");
        j.Set("remoteParty", remoteParty);
        j.Set("remoteAddress", remoteAddress);
        j.Set("proto", static_cast<int>(proto));
        j.Set("hasVideo", hasVideo);
        app_->EmitEvent(j);
    }

    void OnConnectionCleared(ErrCode endedReason, ProtoType protoType,
                             string remoteParty, string remoteAddress,
                             uint32_t duration) override {
        Json j;
        j.Set("type", "call_cleared");
        j.Set("reason", static_cast<int>(endedReason));
        j.Set("proto", static_cast<int>(protoType));
        j.Set("remoteParty", remoteParty);
        j.Set("remoteAddress", remoteAddress);
        j.Set("duration", duration);
        app_->EmitEvent(j);
    }

    void OnSetCodecName(uint32_t streamID, string codecName,
                        bool isSend) override {
        Json j;
        j.Set("type", "codec");
        j.Set("streamID", streamID);
        j.Set("name", codecName);
        j.Set("isSend", isSend);
        app_->EmitEvent(j);
    }

    void OnReceivedSystemMsg(string systemMsg) override {
        Json j;
        j.Set("type", "system");
        j.Set("message", systemMsg);
        app_->EmitEvent(j);
    }

    // ---- Other signaling callbacks (empty for this demo) ----

    void OnAddNewCallRecord(CallDirection, string, string, string,
                            string, uint32_t, CallResult) override {}
    void OnUpdateConferenceList(list<VCSConfInfo>) override {}
    void OnUpdateTerminalList(list<VCSMember>) override {}
    void OnUpdateFloorList(list<VCSFloor>) override {}
    void OnUpdateConferenceChairmanID(long) override {}
    void OnAnswerVCSCall(string, string, string) override {}
    void OnReceivedWhiteboardCmd(uint8_t*, uint32_t) override {}
    void OnReceivedWhiteboardBGPicture(uint8_t*, uint32_t) override {}
    void OnReceivedDesktopControlActionCmd(uint8_t*, uint32_t) override {}
    void OnChangeVCSConfControlMode(VCSConfControlMode) override {}
    void OnReceivedVCSLoginResponse(string, string, string, string) override {}
    void OnReceivedVCSConferenceParameters(string, string, string, string,
                                           long, string, bool,
                                           uint32_t, uint32_t) override {}
    void OnReceivedVoteCmd(uint8_t*, uint32_t) override {}
    void OnReceivedCollaborativeBrowsingCmd(uint8_t*, uint32_t) override {}
    void OnClearWhiteboard(bool) override {}
    void OnReceivedChairmanSyncCmd(LayoutInfo) override {}
    void OnSetEncoderProfile(VideoProfile, uint32_t) override {}
    void OnStartReceiveH239Video(long) override {}
    void OnStopReceiveH239Video() override {}
    void OnPacketLossRateUpdate(float) override {}
    void OnInitPTZControl(string, uint32_t, uint32_t) override {}
    void OnSetPTZParameters(string, uint32_t, uint32_t) override {}
    void OnExecutePTZCommand(uint32_t) override {}
    void OnRtmpConnected() override {}
    void OnRtmpDisconnected() override {}
    void OnGetRtmpPlayerAudioData(uint8_t*, uint32_t, uint32_t,
                                  uint32_t, uint32_t) override {}
    void OnGetRtmpPlayerVideoData(uint8_t*, uint32_t, uint32_t,
                                  uint32_t, uint32_t,
                                  ITCVideoCodecType) override {}
    void OnGetLoginRelatedInfo(int, bool, uint16_t, bool, string) override {}
    void OnWebsocketConnected(ErrCode, string, string, uint16_t,
                              string, bool) override {}
    void OnWebsocketDisconnected(ITCWSDisconnectType, string) override {}
    void OnWebsocketReceivedData(string) override {}
    void OnWebsocketReplyJoinConfereceResult(ErrCode) override {}
    void OnWebsocketAcceptLiveToConference(ProtoType, string) override {}
    void OnWebsocketRejectLiveToConference(ErrCode) override {}
    void OnWebsocketQuitLive() override {}
    void OnWebsocketGetChatMessage(string, string, string, string) override {}
    void OnForceKeyframe(uint32_t) override {}
    void OnInfraredRemoteControlCallback(uint8_t*, uint32_t) override {}
    void OnReceivedH323ConferenceParameters(string, string, long) override {}
    void OnSipBfcpAuxVideoStatusChange(bool) override {}
    void OnStartExtVideo() override {}
    void OnStopExtVideo() override {}
    void OnInformStopVCSExtVideoResp(bool) override {}
    void OnInformEPBePolled(bool, bool) override {}
    void OnAudioControlCMD(int, string) override {}
    void OnReceivedStandarConfVideoSrcInd(long, uint32_t) override {}
    void OnUpdateVideoSrcList(list<VideoSourceMember>) override {}

    // ---- Media callbacks (empty: media does NOT flow through AbsPhone) ----

    bool OnOpenAudioCaptureDevice(uint32_t, uint32_t) override { return true; }
    bool OnReadAudioFrame(uint8_t*, uint32_t) override { return true; }
    void OnCloseAudioCaptureDevice() override {}
    bool OnOpenAudioPlayerDevice(uint32_t, uint32_t) override { return true; }
    bool OnWriteAudioFrame(uint8_t*, uint32_t) override { return true; }
    void OnCloseAudioPlayerDevice() override {}
    bool OnGetYUVFrame(uint32_t, uint8_t*, uint32_t, uint32_t) override {
        return true;
    }
    void OnRenderYUVFrame(long, uint32_t, uint32_t, uint32_t,
                          const uint8_t*, uint32_t) override {}

private:
    DemoApp* app_;
};



// ============================================================================
// SIP configuration helper
// ============================================================================
static ErrCode ConfigureSip(AbsPhone* engine,
                            const std::string& localIp,
                            const std::string& username) {
    SipAccountInfo acc;
    acc.enable = true;
    acc.enableReg = false;                 // direct IP call, no registration
    acc.regTimeout = 3600;
    acc.userName = username;
    acc.displayName = username;
    acc.authName = username;
    acc.passWord = "";
    acc.protocol = SipTransProtocol::e_udp;
    acc.dtmfMode = DTMFMode::e_rfc2833;
    acc.localIp = localIp;
    return engine->SetSipParams(true, acc);
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    GetLogManager()->Initialize(nullptr, "");

    std::string localIp = "0.0.0.0";
    std::string username = "1001";
    std::string autoCallUri;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout <<
                "endpoint_demo [options]\n"
                "  -a <local ip>   local SIP bind IP (default 0.0.0.0)\n"
                "  -u <username>   SIP username (default 1001)\n"
                "  -c <uri>        auto call on startup, e.g. SIP:1001@1.2.3.4\n"
                "  -h, --help      this help\n";
            return 0;
        } else if (arg == "-a" && i + 1 < argc) {
            localIp = argv[++i];
        } else if (arg == "-u" && i + 1 < argc) {
            username = argv[++i];
        } else if (arg == "-c" && i + 1 < argc) {
            autoCallUri = argv[++i];
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    AbsPhone* engine = CreateAbsPhone();
    if (!engine) {
        VCS_LOG_FATAL_L(LogModule::Common, "CreateAbsPhone failed");
        return 1;
    }

    // ConsoleUi for v1; WebUI for v2. DemoApp and DemoObserver are unchanged.
    std::unique_ptr<IDemoUi> ui = std::unique_ptr<IDemoUi>(new ConsoleUi());

    DemoApp app(engine, ui.get());
    DemoObserver observer(&app);

    ErrCode ec = engine->Initialize(
        ITCTerminalType::e_ITCEndpoint,
        &observer,
        nullptr, nullptr);
    if (ec != ErrCode::e_ErrNone) {
        VCS_LOG_FATAL_L(LogModule::Common,
            "Initialize failed: %d", static_cast<int>(ec));
        DestroyAbsPhone(engine);
        return 1;
    }

    engine->SetUsername(username);

    ec = ConfigureSip(engine, localIp, username);
    if (ec != ErrCode::e_ErrNone) {
        VCS_LOG_WARNING_L(LogModule::Common,
            "ConfigureSip returned %d", static_cast<int>(ec));
    }

    ui->SetCommandHandler([&app](const std::string& jsonStr) {
        app.HandleCommand(jsonStr);
    });

    ui->Start();

    VCS_LOG_INFO_L(LogModule::Common,
        "endpoint_demo started. username=%s, localIp=%s",
        username.c_str(), localIp.c_str());

    if (!autoCallUri.empty()) {
        Json j;
        j.Set("cmd", "call");
        j.Set("uri", autoCallUri);
        app.HandleCommand(j.Dump());
    }

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ui->Stop();
    engine->Release();
    DestroyAbsPhone(engine);

    VCS_LOG_INFO_L(LogModule::Common, "endpoint_demo exited");
    return 0;
}