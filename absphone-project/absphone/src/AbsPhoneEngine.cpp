#include "AbsPhoneEngine.h"
#include "IModuleFactory.h"
#include "ConferenceFactory.h"
#include "GatewayFactory.h"
#include "MediaFactory.h"
#include "IMediaDebug.h"
#include "Config.h"
#include "IConfig.h"  // [V7.0+] explicit dependency for the h264-dump config plumbing
#include "LegacyKeyMap.h"
#include "AppPaths.h"
#include "EventBus.h"
#include "ILogger.h"  // [V7.0+] for VCS_LOG_INFO / VCS_LOG_WARNING
#include "PlatformTime.h"
#include <chrono>
#include <exception>
#include <memory>
#include <boost/asio.hpp>

namespace vcs {
namespace absphone {

// 配置键名常量
static const char* KEY_MEDIA_ENCRYPTION = "media_encryption_enabled";
static const char* KEY_FEC_ENABLED = "fec_enabled";
static const char* KEY_RS485_ENABLED = "rs485_enabled";
static const char* KEY_RTP_PORT_DETECTION = "rtp_port_detection_enabled";
static const char* KEY_H245_TUNNELING = "h245_tunneling_enabled";
static const char* KEY_FAST_START = "fast_start_enabled";
static const char* KEY_MAX_RTP_PAYLOAD_SIZE = "max_rtp_payload_size";
static const char* KEY_VIDEO_JITTER_BUFFER = "video_jitter_buffer_enabled";
static const char* KEY_AUDIO_JITTER_BUFFER = "audio_jitter_buffer_enabled";
static const char* KEY_AUTO_MATCHING_VIDEO = "auto_matching_video_capability";
static const char* KEY_SEND_GRQ = "send_grq_enabled";
static const char* KEY_LOG_LEVEL = "log_level";
static const char* KEY_LOG_SIZE = "log_size";
static const char* KEY_GK_ENABLED = "gk_enabled";
static const char* KEY_GK_IP = "gk_ip";
static const char* KEY_GK_REG_NUMBER = "gk_reg_number";
static const char* KEY_GK_REG_NAME = "gk_reg_name";
static const char* KEY_GK_REG_PASSWORD = "gk_reg_password";
static const char* KEY_NAT_ENABLED = "nat_enabled";
static const char* KEY_NAT_IP = "nat_ip";
static const char* KEY_PORTS_ENABLED = "ports_setting_enabled";
static const char* KEY_TCP_PORT_BASE = "tcp_port_base";
static const char* KEY_TCP_PORT_MAX = "tcp_port_max";
static const char* KEY_RTP_PORT_BASE = "rtp_port_base";
static const char* KEY_RTP_PORT_MAX = "rtp_port_max";
static const char* KEY_SIP_LISTEN_PORT = "sip_listen_port";
static const char* KEY_USERNAME = "username";
static const char* KEY_TERMINAL_TYPE = "terminal_type";
static const char* KEY_DEFAULT_ROOM_NAME = "default_room_name";
static const char* KEY_H264_EXTEND_CAP = "h264_extend_capability";
static const char* KEY_MIX_DUAL_STREAM = "mix_dual_stream";
static const char* KEY_SIMULT_INTERP = "simult_interp_enabled";
static const char* KEY_5G_ENABLED = "5g_enabled";
static const char* KEY_AUX_STREAM_AUDIO = "aux_stream_audio";

// 旧 GetGKParameters 只返回逗号分隔后的第一个房间号
// （G:\src\absphone\src\absphone_impl.cxx:5153-5154）。
static string FirstCommaToken(const string& value) {
    const size_t comma = value.find(',');
    return comma == string::npos ? value : value.substr(0, comma);
}

// 旧 SetGKParameters 在 ipAddr=="127.0.0.1" 时把监听地址改写成"本机网卡地址"
// 并置 Use Local GK（absphone_impl.cxx:5114-5127）。这里用默认路由探测本机
// IPv4，失败时保留 127.0.0.1，与旧实现的兜底一致。
static string DetectLocalIpv4() {
    try {
        boost::asio::io_context io;
        boost::asio::ip::udp::socket socket(io);
        socket.open(boost::asio::ip::udp::v4());
        socket.connect(boost::asio::ip::udp::endpoint(
            boost::asio::ip::address::from_string("8.8.8.8"), 53));
        return socket.local_endpoint().address().to_string();
    } catch (const std::exception&) {
        return "127.0.0.1";
    }
}

// ============================================================
// 媒体信息：快照 → 旧 MediaInfo 的展示映射
//
// 旧系统的展示词表来自 ep_sip_connection.cpp:1218-1470：
//   PCMA->"G.711-ALaw"，PCMU->"G.711-ULaw"，G722->"G.722"，无编码->"NA"；
//   音频 format 是 "N KHz"（G722 与未知 clock 固定 16）；视频 format 是
//   "WxH"；帧率上限 60；fractionLost 保留 1 位小数且不超过 100；
//   地址未知显示 "NA"；bitrate 字段是"每秒字节数"（音频上限 8K）。
// ============================================================
static string LegacyCodecDisplayName(const string& codecName) {
    if (codecName.empty()) {
        return "NA";
    }
    if (codecName == "PCMA") return "G.711-ALaw";
    if (codecName == "PCMU") return "G.711-ULaw";
    if (codecName == "G722") return "G.722";
    return codecName;
}

static string LegacyAudioFormat(const string& codecName, uint32_t clockRate) {
    // ep_sip_connection.cpp:1280-1287：默认 clock=16000；只有"能拿到
    // clock_rate 且不是 G722"才采用 SDP 值。G722 的 RTP clock 是 8000，
    // 但旧实现一律显示 16 KHz，这里保持同一展示。
    if (codecName == "G722" || clockRate == 0) {
        clockRate = 16000;
    }
    return std::to_string(clockRate / 1000) + " KHz";
}

// fractionLost：旧实现保留 1 位小数并截断到 100（ep_sip_connection.cpp:1277-1279）。
static float LegacyFractionLost(float percent) {
    if (percent > 100.0f) {
        percent = 100.0f;
    }
    return static_cast<float>(static_cast<int>(percent * 10)) / 10.0f;
}

static string LegacyAddress(const string& address) {
    return address.empty() ? "NA" : address;
}

static string LegacyVideoFormat(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return "NA";
    }
    return std::to_string(width) + "x" + std::to_string(height);
}

static uint32_t LegacyFrameRate(uint32_t framesPerSecond) {
    return framesPerSecond > 60 ? 60 : framesPerSecond;
}

// bitrate 字段：旧系统填的是"每秒字节数"，这里把 bps 换算成 B/s。
// 音频旧实现是 PMIN(上一秒字节数, 8*1024)（ep_sip_connection.cpp:1271/1312），
// 上限必须保留，否则界面会出现旧系统从未显示过的值。
// 差异说明：旧实现视频"发送"方向填的是配置的 nominal_bitrate/8（静态值），
// 这里填实测发送速率 —— 排障时能看出编码器是否真的在发；单位一致。
static uint32_t LegacyBitrate(bool isAudio, uint32_t bitsPerSecond) {
    const uint32_t bytesPerSecond = bitsPerSecond / 8;
    const uint32_t kAudioBitrateCap = 8 * 1024;
    return (isAudio && bytesPerSecond > kAudioBitrateCap)
        ? kAudioBitrateCap : bytesPerSecond;
}

static void FillLegacyMediaInfo(const vcs::MediaStreamSnapshot& stream,
                                MediaInfo& info) {
    const bool isVideo = (stream.kind != vcs::MediaStreamKind::Audio);
    const string codecName = LegacyCodecDisplayName(stream.codec.codecName);

    // ---- 发送方向 ----
    info.sent.codec = codecName;
    info.sent.format = isVideo
        ? LegacyVideoFormat(stream.encodedWidth, stream.encodedHeight)
        : LegacyAudioFormat(stream.codec.codecName, stream.codec.clockRate);
    info.sent.bitrate = LegacyBitrate(!isVideo, stream.sendBitrateBps);
    info.sent.bytesSent = stream.bytesSent;
    info.sent.packetsSent = stream.packetsSent;
    info.sent.packetsLost = stream.packetsLost;
    info.sent.fractionLost = LegacyFractionLost(stream.fractionLost);
    info.sent.averageSendTime = 0;   // 旧实现恒为 0
    info.sent.framerate = isVideo ? LegacyFrameRate(stream.framesSentPerSecond) : 0;
    info.sent.jitter = stream.jitterUs / 1000;
    info.sent.rttDelay = stream.rttMs;
    info.sent.remoteAddress = LegacyAddress(stream.remoteAddress);
    info.sent.bEncrypted = false;    // SRTP 未实现，没有加密状态可报
    info.sent.packetsSentInSecond = stream.packetsSentInSecond;
    info.sent.packetsLostInSecond = stream.packetsLostInSecond;

    // ---- 接收方向 ----
    info.received.codec = codecName;
    info.received.format = isVideo
        ? LegacyVideoFormat(stream.decodedWidth, stream.decodedHeight)
        : LegacyAudioFormat(stream.codec.codecName, stream.codec.clockRate);
    info.received.bitrate = LegacyBitrate(!isVideo, stream.receiveBitrateBps);
    info.received.bytesReceived = stream.bytesReceived;
    info.received.packetsReceived = stream.packetsReceived;
    info.received.packetsLost = stream.packetsLost;
    info.received.fractionLost = LegacyFractionLost(stream.fractionLost);
    info.received.averageReceiveTime = 0;   // 旧实现恒为 0
    info.received.framerate = isVideo ? LegacyFrameRate(stream.framesDecodedPerSecond) : 0;
    info.received.jitter = stream.jitterUs / 1000;
    info.received.rttDelay = stream.rttMs;
    info.received.localAddress = LegacyAddress(stream.localAddress);
    info.received.bEncrypted = false;
    info.received.packetsReceivedInSecond = stream.packetsReceivedInSecond;
    info.received.packetsLostInSecond = stream.packetsLostInSecond;
}

struct AbsPhoneEngine::Impl {
    // 配置管理器
    std::shared_ptr<vcs::base::Config> config;
    // 已解析的配置文件路径（SaveConfigs 写回同一文件）
    string configPath;

    // 工厂实例
    vcs::conference::ConferenceFactory conferenceFactory;
    vcs::gateway::GatewayFactory gatewayFactory;
    vcs::media::MediaFactory mediaFactory;

    // 平台事件总线：整个进程唯一，由组合根(AbsPhoneEngine)创建并注入各层
    std::shared_ptr<vcs::EventBus> eventBus;

    // 业务核心 - conference模块
    std::unique_ptr<vcs::conference::IConferenceControl> conferenceControl;

    // 被调用服务 - gateway模块
    std::unique_ptr<vcs::gateway::IGatewayManager> gatewayManager;

    // 被调用服务 - media模块
    std::unique_ptr<vcs::media::IMediaControl> mediaControl;

    // 观察者指针（由外部设置）
    AbsPhoneObserver* observer = nullptr;

	

    bool initialized = false;
    bool running = false;

    // 终端类型
    ITCTerminalType terminalType = ITCTerminalType::e_ITCEndpoint;

    // 用户名
    string username;

    // 关机收口（docs/08 §3）：Release 与 Impl 析构共用同一序列，幂等。
    // 成员的自然逆序（media 先于 conference）会 UAF，不能依赖。
    void Shutdown() {
        if (shutdownDone) {
            return;
        }
        shutdownDone = true;

        // 1. 发起挂断（异步，不等待）
        if (conferenceControl) {
            conferenceControl->HangupCall();
        }
        // 2. 总线排空后 join → 返回即 T2 已退出。必须在会议层收口之前：
        //    Shutdown 会 join 并析构 executor_，T2 上在途的订阅回调随后
        //    若再 Submit，就是碰已析构对象（UAF）。
        if (eventBus) {
            eventBus->Stop();
        }
        // 3. 会议层收口：stopped_ = true + 退订 + 有界 join T1 + 停当前会议媒体
        if (conferenceControl) {
            conferenceControl->Shutdown();
        }
        // 4. 此时无在途 handler 引用、T1 已退出，可安全析构会议层
        conferenceControl.reset();
        // 5. gateway 停机（pj 销毁）；会话清理在 adapter.Stop 之前完成
        if (gatewayManager) {
            gatewayManager->Stop();
            gatewayManager.reset();
        }
        // 6. media 释放（此前已 StopMedia）
        mediaControl.reset();
        // 7. 总线释放
        eventBus.reset();

        // 8. 日志最后关：此前所有生产者（会议/gateway/media/总线）都已停止，
        //    之后再无并发写入，才能安全 Flush 并销毁各模块 logger（docs/10 §4.3）。
        vcs::base::GetLogManager()->Flush();
        vcs::base::GetLogManager()->Shutdown();

        observer = nullptr;
        initialized = false;
    }

    ~Impl() {
        Shutdown();
    }

    bool shutdownDone = false;
};

AbsPhoneEngine::AbsPhoneEngine()
    : impl_(std::make_unique<Impl>()) {
}

AbsPhoneEngine::~AbsPhoneEngine() {
    Release();
}

AbsPhone* CreateAbsPhone() {
    return new AbsPhoneEngine();
}

void DestroyAbsPhone(AbsPhone* phone) {
    if (phone) {
        phone->Release();
        delete phone;
    }
}

//=======================================================================================================
//                                         Common APIs                                                  =
//=======================================================================================================

AbsPhoneObserver* AbsPhoneEngine::GetObserver() const {
	return impl_->observer; 
}

/// 设置观察者
void AbsPhoneEngine::SetObserver(AbsPhoneObserver* observer) {
	impl_->observer = observer; 
}

ErrCode AbsPhoneEngine::Initialize(ITCTerminalType terminaltype, AbsPhoneObserver* observer,
	void* androidJVM, void* androidObj) {
	if (impl_->initialized) {
		return ErrCode::e_ErrUnspecified;
	}

	impl_->terminalType = terminaltype;
	impl_->observer = observer;

	impl_->config = std::make_shared<vcs::base::Config>();
	// 旧键名（hdmeeting.ini 原文键）-> 规范键，见 docs/10 §3.1.8 表 A。
	// 必须在 LoadFromFile 之前注入，否则规范键读不到旧文件。
	impl_->config->SetKeyAliases(vcs::absphone::BuildLegacyKeyAliases());

	// --- 加载配置（公共） ---
	{
		const vcs::absphone::AppPaths paths = vcs::absphone::ResolveAppPaths();
		impl_->configPath = paths.configFile;
		vcs::Response r = impl_->config->LoadFromFile(impl_->configPath);

		// --- 日志单源接入：早于任何组件创建（docs/10 §4.3） ---
		vcs::Response logRes = vcs::base::GetLogManager()->Initialize(impl_->config, paths.logDir);

		// 加载期警告在日志配置生效后再输出，LogLevel=Off 时保持完全静默
		// （docs/10 §3.2.10）。
		const std::vector<std::string> loadWarnings = impl_->config->TakeLoadWarnings();
		for (size_t i = 0; i < loadWarnings.size(); ++i) {
			VCS_LOG_WARNING(vcs::base::LogModule::Base, "[Config] " + loadWarnings[i]);
		}

		if (!logRes.IsSuccess()) {
			VCS_LOG_WARNING(vcs::base::LogModule::Common,
				"AbsPhoneEngine: log initialize failed: " + logRes.GetMsg());
		}

		// 媒体调试旁路（docs/06）：独立 media_debug.ini，缺省即关闭。
		// 放在日志初始化之后，加载结果能被日志看到。
		vcs::media::GetMediaDebug()->LoadFromFile(paths.mediaDebugFile);
		if (!r.IsSuccess()) {
			if (r.GetCode() != vcs::ResponseCode::NotFound) {
				VCS_LOG_WARNING(vcs::base::LogModule::Common,
					std::string("AbsPhoneEngine: failed to load ") + impl_->configPath + ": " + r.GetMsg());
			}
			else {
				VCS_LOG_INFO(vcs::base::LogModule::Common,
					std::string("AbsPhoneEngine: ") + impl_->configPath + " not found, using built-in defaults");
			}
		}
	}

	// --- 创建公共组件 ---
	impl_->eventBus = std::make_shared<vcs::EventBus>();
	impl_->mediaControl = impl_->mediaFactory.CreateMediaControl();
	impl_->gatewayManager = impl_->gatewayFactory.CreateGatewayManager(impl_->config.get());

	if (!impl_->eventBus || !impl_->mediaControl || !impl_->gatewayManager) {
		VCS_LOG_ERROR(vcs::base::LogModule::Common, "AbsPhoneEngine::Initialize failed: Create components failed");
		Release();
		return ErrCode::e_ErrUnspecified;
	}

	// --- 启动事件总线：所有跨模块事件都经它异步派发 ---
	impl_->eventBus->Start();

	// --- 注入公共依赖（Gateway 和 Media 是公共的基础设施） ---
	impl_->gatewayManager->SetEventBus(impl_->eventBus);
	impl_->mediaControl->SetEventBus(impl_->eventBus);
	impl_->mediaControl->SetConfig(impl_->config);

	vcs::Response gwRes = impl_->gatewayManager->Start();
	if (!gwRes.IsSuccess()) {
		VCS_LOG_ERROR(vcs::base::LogModule::Common, "AbsPhoneEngine::Initialize failed: Gateway failed to start");
		Release();
		return ErrCode::e_ErrUnspecified;
	}

	// ============================================================
	// 终端业务初始化。e_ITCMCU 仅为对外兼容保留，内部不再有 MCU 分支。
	// ============================================================
	if (terminaltype == ITCTerminalType::e_ITCMCU) {
		VCS_LOG_WARNING(vcs::base::LogModule::Common,
			"MCU terminal type is deprecated; running as endpoint");
	}

	impl_->conferenceControl = impl_->conferenceFactory.CreateConferenceControl(
		impl_->gatewayManager.get(), impl_->mediaControl.get());
	if (!impl_->conferenceControl) {
		VCS_LOG_ERROR(vcs::base::LogModule::Common, "Failed to create ConferenceControl");
		Release();
		return ErrCode::e_ErrUnspecified;
	}

	impl_->conferenceControl->SetEventBus(impl_->eventBus);
	impl_->conferenceControl->SetObserver(this);

	if (!impl_->conferenceControl->Initialize().IsSuccess()) {
		VCS_LOG_ERROR(vcs::base::LogModule::Common, "ConferenceControl::Initialize returned error");
		Release();
		return ErrCode::e_ErrUnspecified;
	}

	impl_->initialized = true;
	return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::Release() {
	if (!impl_) return ErrCode::e_ErrNone;

	// 逆序拆除见 Impl::Shutdown（docs/08 §3，与 Impl 析构复用、幂等）。
	impl_->Shutdown();
	return ErrCode::e_ErrNone;
}

//=======================================================================================================
//                        vcs::conference::IConferenceObserver adaption
//=======================================================================================================

void AbsPhoneEngine::OnIncomingCall(const std::string& remoteUri, bool hasVideo) {
	(void)hasVideo;
	if (impl_->observer) {
		impl_->observer->OnAnswerCall(remoteUri);
	}
}

void AbsPhoneEngine::OnCallConnected(const std::string& remoteUri, bool hasVideo) {
	if (impl_->observer) {
		impl_->observer->OnConnectionEstablished(remoteUri, "", ProtoType::e_ProtoSIP, hasVideo);
	}
}

void AbsPhoneEngine::OnCallTerminated(const vcs::conference::CallTerminationInfo& info) {
	if (impl_->observer) {
		impl_->observer->OnConnectionCleared(ErrCode::e_ErrNone, ProtoType::e_ProtoSIP,
			info.remoteUri, info.remoteAddress, info.durationSec);
	}
}

ITCTerminalType AbsPhoneEngine::GetTerminalType() {
    return impl_->terminalType;
}

ErrCode AbsPhoneEngine::SetTerminalType(ITCTerminalType terminaltype) {
    impl_->terminalType = terminaltype;
    impl_->config->SetInt(KEY_TERMINAL_TYPE, static_cast<int>(terminaltype));
    return ErrCode::e_ErrNone;
}

string AbsPhoneEngine::GetUsername() {
    return impl_->username;
}

ErrCode AbsPhoneEngine::SetUsername(string userName) {
    impl_->username = userName;
    impl_->config->SetString(KEY_USERNAME, userName);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::EnableMediaEncryption(bool bEnable, H235AlgType h235AlgType) {
    impl_->config->SetBool(KEY_MEDIA_ENCRYPTION, bEnable);
    impl_->config->SetInt("h235_alg_type", static_cast<int>(h235AlgType));
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::IsMediaEncryptionEnabled() {
    return impl_->config->GetBool(KEY_MEDIA_ENCRYPTION, false);
}

ErrCode AbsPhoneEngine::EnableFEC(bool bEnable) {
    impl_->config->SetBool(KEY_FEC_ENABLED, bEnable);
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::IsFECEnabled() {
    return impl_->config->GetBool(KEY_FEC_ENABLED, false);
}

ErrCode AbsPhoneEngine::ExecuteCommand(string cmd, string args) {
    // TODO: 实现命令执行
    return ErrCode::e_ErrUnspecified;
}

ErrCode AbsPhoneEngine::EnableRS485(bool enable) {
    impl_->config->SetBool(KEY_RS485_ENABLED, enable);
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::IsRS485Enabled() {
    return impl_->config->GetBool(KEY_RS485_ENABLED, false);
}

ErrCode AbsPhoneEngine::EnableRTPSendPortDetection(bool enable) {
    impl_->config->SetBool(KEY_RTP_PORT_DETECTION, enable);
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::IsRTPSendPortDetectionEnabled() {
    return impl_->config->GetBool(KEY_RTP_PORT_DETECTION, false);
}

ErrCode AbsPhoneEngine::GetSystemTime(int& second, int& minute, int& hour,
                                       int& day, int& month, int& year) {
    std::tm tm_buf{};
    if (!vcs::base::platform::LocalNow(tm_buf)) {
        return ErrCode::e_ErrUnspecified;
    }
    second = tm_buf.tm_sec;
    minute = tm_buf.tm_min;
    hour   = tm_buf.tm_hour;
    day    = tm_buf.tm_mday;
    month  = tm_buf.tm_mon + 1;
    year   = tm_buf.tm_year + 1900;
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::SetSystemTime(int second, int minute, int hour,
                                       int day, int month, int year) {
    // TODO: 实现系统时间设置
    return ErrCode::e_ErrUnspecified;
}

ErrCode AbsPhoneEngine::GetNATRouterInfo(bool& bNATEnabled, string& natIP) {
    bNATEnabled = impl_->config->GetBool(KEY_NAT_ENABLED, false);
    natIP = impl_->config->GetString(KEY_NAT_IP, "");
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::SetNATRouter(bool bNATEnabled, string natIP) {
    impl_->config->SetBool(KEY_NAT_ENABLED, bNATEnabled);
    impl_->config->SetString(KEY_NAT_IP, natIP);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetPortsInfo(bool& enablePortsSetting,
                                      uint32_t& tcpPortBase, uint32_t& tcpPortMax,
                                      uint32_t& rtpPortBase, uint32_t& rtpPortMax,
                                      uint32_t& sipListenPort) {
    enablePortsSetting = impl_->config->GetBool(KEY_PORTS_ENABLED, false);
    tcpPortBase = impl_->config->GetInt(KEY_TCP_PORT_BASE, 5060);
    tcpPortMax = impl_->config->GetInt(KEY_TCP_PORT_MAX, 5069);
    rtpPortBase = impl_->config->GetInt(KEY_RTP_PORT_BASE, 5000);
    rtpPortMax = impl_->config->GetInt(KEY_RTP_PORT_MAX, 5500);
    sipListenPort = impl_->config->GetInt(KEY_SIP_LISTEN_PORT, 5060);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::SetPorts(bool enablePortsSetting,
                                  uint32_t tcpPortBase, uint32_t tcpPortMax,
                                  uint32_t rtpPortBase, uint32_t rtpPortMax,
                                  uint32_t sipListenPort) {
    impl_->config->SetBool(KEY_PORTS_ENABLED, enablePortsSetting);
    impl_->config->SetInt(KEY_TCP_PORT_BASE, tcpPortBase);
    impl_->config->SetInt(KEY_TCP_PORT_MAX, tcpPortMax);
    impl_->config->SetInt(KEY_RTP_PORT_BASE, rtpPortBase);
    impl_->config->SetInt(KEY_RTP_PORT_MAX, rtpPortMax);
    impl_->config->SetInt(KEY_SIP_LISTEN_PORT, sipListenPort);
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::IsCorrectIPV4Address(string strIP) {
    if (strIP.empty()) return false;

    int seg = 0;
    int val = 0;
    int digits = 0;

    for (size_t i = 0; i <= strIP.size(); ++i) {
        char c = (i < strIP.size()) ? strIP[i] : '.';

        if (c == '.') {
            if (digits == 0 || digits > 3) return false;
            if (val > 255) return false;
            if (digits > 1 && strIP[i - digits] == '0') return false;

            seg++;
            val = 0;
            digits = 0;

            if (seg > 4) return false;
        } else if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            digits++;
        } else {
            return false;
        }
    }

    return seg == 4;
}

bool AbsPhoneEngine::IsConferenceControlEnabled() {
    return true;
}

ErrCode AbsPhoneEngine::EnableH245Tunneling(bool bEnable) {
    impl_->config->SetBool(KEY_H245_TUNNELING, bEnable);
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::IsH245TunnelingEnabled() {
    return impl_->config->GetBool(KEY_H245_TUNNELING, true);
}

ErrCode AbsPhoneEngine::EnableFastStart(bool bEnable) {
    impl_->config->SetBool(KEY_FAST_START, bEnable);
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::IsFastStartEnabled() {
    return impl_->config->GetBool(KEY_FAST_START, true);
}

ErrCode AbsPhoneEngine::GetMaxRTPPayloadSize(uint32_t& size) {
    size = impl_->config->GetInt(KEY_MAX_RTP_PAYLOAD_SIZE, 1400);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::SetMaxRTPPayloadSize(uint32_t size) {
    impl_->config->SetInt(KEY_MAX_RTP_PAYLOAD_SIZE, size);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::SaveConfigs() {
    if (!impl_->config) {
        return ErrCode::e_ErrServerInternalError;
    }
    // 写穿（docs/10 D2-A）已在 Set* 时完成；这里是把当前内存模型再落一次盘，
    // 返回最终结果（写穿失败的行仍保留 dirty，可由此重试）。
    vcs::Response r = impl_->config->SaveToFile(impl_->configPath);
    if (r.IsSuccess()) {
        return ErrCode::e_ErrNone;
    }
    VCS_LOG_ERROR(vcs::base::LogModule::Common,
        "AbsPhoneEngine::SaveConfigs failed: " + impl_->configPath + ": " + r.GetMsg());
    return r.GetCode() == vcs::ResponseCode::InvalidParams
        ? ErrCode::e_ErrInvalidParameters
        : ErrCode::e_ErrServerInternalError;
}

string AbsPhoneEngine::GetGkName() {
    return impl_->config->GetString(KEY_GK_IP, "");
}

string AbsPhoneEngine::GetGkHostName() {
    return impl_->config->GetString("gk_hostname", "");
}

ErrCode AbsPhoneEngine::UpdateGK() {
    // TODO: 实现GK更新
    return ErrCode::e_ErrUnspecified;
}

ErrCode AbsPhoneEngine::SetLogLevel(uint32_t level) {
    if (level > static_cast<uint32_t>(vcs::base::LogLevel::Off)) {
        return ErrCode::e_ErrInvalidParameters;
    }
    // 旧键是唯一持久化入口：写穿后立即 Reload，让所有 logger 生效（docs/10 §3.2.3）
    impl_->config->SetInt(KEY_LOG_LEVEL, static_cast<int>(level));
    vcs::Response r = vcs::base::GetLogManager()->Reload();
    return r.IsSuccess() ? ErrCode::e_ErrNone : ErrCode::e_ErrServerInternalError;
}

ErrCode AbsPhoneEngine::GetLogLevel(uint32_t& level) {
    // 返回生效值（D5-A）：单一默认 Info，不再与配置文件默认值分叉
    level = static_cast<uint32_t>(
        vcs::base::GetLogManager()->GetLogger(vcs::base::LogModule::Common)->GetLevel());
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::SetLogSize(uint32_t logSize) {
    if (logSize == 0) {
        return ErrCode::e_ErrInvalidParameters;
    }
    impl_->config->SetInt(KEY_LOG_SIZE, static_cast<int>(logSize));
    vcs::Response r = vcs::base::GetLogManager()->Reload();
    return r.IsSuccess() ? ErrCode::e_ErrNone : ErrCode::e_ErrServerInternalError;
}

ErrCode AbsPhoneEngine::GetLogSize(uint32_t& logSize) {
    logSize = impl_->config->GetInt(KEY_LOG_SIZE, 10);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::ClearLog() {
    // TODO: 实现日志清除
    return ErrCode::e_ErrUnspecified;
}

ErrCode AbsPhoneEngine::SetGKParameters(bool bEnableGK, string ipAddr, string registrationNumber,
                                         string registrationName, string registrationPassword) {
    impl_->config->SetBool(KEY_GK_ENABLED, bEnableGK);
    impl_->config->SetString(KEY_GK_REG_NUMBER, registrationNumber);
    impl_->config->SetString(KEY_GK_REG_NAME, registrationName);
    impl_->config->SetString(KEY_GK_REG_PASSWORD, registrationPassword);
    if (ipAddr == "127.0.0.1") {
        impl_->config->SetString(KEY_GK_IP, DetectLocalIpv4());
        impl_->config->SetBool("is_local_gk", true);
    } else {
        impl_->config->SetString(KEY_GK_IP, ipAddr);
        impl_->config->SetBool("is_local_gk", false);
    }
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetGKParameters(bool& bEnableGK, bool& bGKRegistered,
                                         string& ipAddr, string& registrationNumber,
                                         string& registrationName, string& registrationPassword,
                                         bool& isLocalGK) {
    bEnableGK = impl_->config->GetBool(KEY_GK_ENABLED, false);
    bGKRegistered = impl_->config->GetBool("gk_registered", false);
    ipAddr = impl_->config->GetString(KEY_GK_IP, "");
    registrationNumber = FirstCommaToken(impl_->config->GetString(KEY_GK_REG_NUMBER, ""));
    registrationName = impl_->config->GetString(KEY_GK_REG_NAME, "");
    registrationPassword = impl_->config->GetString(KEY_GK_REG_PASSWORD, "");
    isLocalGK = impl_->config->GetBool("is_local_gk", false);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::RemoveGatekeeper() {
    impl_->config->SetBool(KEY_GK_ENABLED, false);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::ConfigSetInteger(string key, int32_t value) {
    impl_->config->SetInt(key, value);
    return ErrCode::e_ErrNone;
}

int32_t AbsPhoneEngine::ConfigGetInteger(string key, int32_t defaultValue) {
    return impl_->config->GetInt(key, defaultValue);
}

ErrCode AbsPhoneEngine::ConfigSetString(string key, string value) {
    impl_->config->SetString(key, value);
    return ErrCode::e_ErrNone;
}

string AbsPhoneEngine::ConfigGetString(string key, string defaultValue) {
    return impl_->config->GetString(key, defaultValue);
}

ErrCode AbsPhoneEngine::ConfigSetBoolean(string key, bool value) {
    impl_->config->SetBool(key, value);
    return ErrCode::e_ErrNone;
}

bool AbsPhoneEngine::ConfigGetBoolean(string key, bool defaultValue) {
    return impl_->config->GetBool(key, defaultValue);
}

ErrCode AbsPhoneEngine::EnableH264ExtendCapabilityInfo(bool enable) {
    impl_->config->SetBool(KEY_H264_EXTEND_CAP, enable);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetH264ExtendCapabilityInfoEnableStatus(bool& enable) {
    enable = impl_->config->GetBool(KEY_H264_EXTEND_CAP, false);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::EnableVideoJitterBuffer(bool enable) {
    impl_->config->SetBool(KEY_VIDEO_JITTER_BUFFER, enable);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetVideoJitterBufferStatus(bool& enabled) {
    enabled = impl_->config->GetBool(KEY_VIDEO_JITTER_BUFFER, true);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::EnableAudioJitterBuffer(bool enable) {
    impl_->config->SetBool(KEY_AUDIO_JITTER_BUFFER, enable);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetAudioJitterBufferStatus(bool& enabled) {
    enabled = impl_->config->GetBool(KEY_AUDIO_JITTER_BUFFER, true);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::WriteLog(int level, string content) {
    // TODO: 实现日志写入
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::EnableAutoMatchingVideoCapability(bool enable) {
    impl_->config->SetBool(KEY_AUTO_MATCHING_VIDEO, enable);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetAutoMatchingVideoCapabilityStatus(bool& enabled) {
    enabled = impl_->config->GetBool(KEY_AUTO_MATCHING_VIDEO, true);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetAudioJitterDelay(list<ItemInfo>& jitterList, uint32_t& curJitterIndex) {
    jitterList.clear();
    curJitterIndex = impl_->config->GetInt("audio_jitter_index", 0);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::SetAudioJitterDelay(uint32_t curJitterIndex) {
    impl_->config->SetInt("audio_jitter_index", curJitterIndex);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::EnableSendGRQ(bool enable) {
    impl_->config->SetBool(KEY_SEND_GRQ, enable);
    return ErrCode::e_ErrNone;
}

ErrCode AbsPhoneEngine::GetGRQStatus(bool& enabled) {
    enabled = impl_->config->GetBool(KEY_SEND_GRQ, true);
    return ErrCode::e_ErrNone;
}

//                                         Endpoint APIs                                              =

/*
    Login VCS server
    @param account: Login account, must be [VCS account / phone number / email]
    @param password: Login password
    @param VCSAddress: VCS server address, must be [ip / ip:port]
    @return: e_ErrNone on success, other value on error
    */
    ErrCode AbsPhoneEngine::LoginVCS(string account, string password, string VCSAddress, string registerKey ) {
		return ErrCode::e_ErrNone;
    }

    /* Get last VCS loging error description */
    string AbsPhoneEngine::GetLastVCSLoginErrorDescription() {
		return "";
    }

    /*
    Logout VCS server
    @return: e_ErrNone on success, other value on error
    */
    ErrCode AbsPhoneEngine::LogoutVCS() {
		return ErrCode::e_ErrNone;
    }

    /*
    Check if the server has been logged in
    @return: true on logged, false on offline
    */
    bool AbsPhoneEngine::IsVCSLogged() {
		return true;
    }

    /*
    Make call to remote party
    @param remoteParty: The input format must be H323:XXX / SIP:XXX / VCS:XXX
    @return: e_ErrNone on success, other value on error
    */
    ErrCode AbsPhoneEngine::MakeCall(string remoteParty) {
		if (impl_->conferenceControl) {
			return impl_->conferenceControl->MakeCall(remoteParty, true).IsSuccess()
				? ErrCode::e_ErrNone : ErrCode::e_ErrUnspecified;
		}
		return ErrCode::e_ErrUnspecified;
    }

    ErrCode AbsPhoneEngine::AnswerCall(AnswerCallResponse response) {
		if (!impl_->initialized || !impl_->conferenceControl) {
			return ErrCode::e_ErrUnspecified;
		}

		const bool accept = (response == AnswerCallResponse::e_AnswerCallNow);

		vcs::Response resp = impl_->conferenceControl->AnswerCall(accept);
		return resp.IsSuccess() ? ErrCode::e_ErrNone : ErrCode::e_ErrUnspecified;
    }

    ErrCode AbsPhoneEngine::InviteVCSMember(string account) {
		return ErrCode::e_ErrNone;
    }

	ErrCode AbsPhoneEngine::ClearCall() {
		if (!impl_->conferenceControl) {
			return ErrCode::e_ErrUnspecified;
		}
		vcs::Response resp = impl_->conferenceControl->HangupCall();
		return resp.IsSuccess() ? ErrCode::e_ErrNone : ErrCode::e_ErrUnspecified;
	}

    ErrCode AbsPhoneEngine::OpenH239Channel() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::CloseH239Channel() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetH239ChannelOpenStatus(bool& isOpen) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableH239Protocol(bool bEnable) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsH239ProtocolEnabled() {
		return true;
    }

    ErrCode AbsPhoneEngine::GetForwardCall(ForwardCallMode& mode, string& busyForwardParty) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetForwardCall(ForwardCallMode mode, string busyForwardParty) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetRingSoudFileList(list<ItemInfo>& fileList, uint32_t& curFileIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::PlayRingSoundFile(uint32_t fileIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetRingSoundFile(uint32_t fileIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::Beep() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableBeep(bool bEnable) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsBeepEnabled() {
		return true;
    }

    ErrCode AbsPhoneEngine::SetAnswerMode(AnswerMode mode, uint32_t maxWaitTime ) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAnswerMode(AnswerMode& mode, uint32_t& maxWaitTime) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableOutputCallRecord(bool bEnable) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsCallRecordCanOutput() {
		return true;
    }

    ErrCode AbsPhoneEngine::EnableAutoAnswerMute(bool bEnable) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsAutoAnswerMuteEnabled() {
		return true;
    }

    ErrCode AbsPhoneEngine::GetMicrophoneVolume(uint32_t& value) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetSpeakerVolume(uint32_t& value) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetMicrophoneVolume(uint32_t value) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetSpeakerVolume(uint32_t value) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartSpeakerTest() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopSpeakerTest() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartAudioLoopbackTest() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopAudioLoopbackTest() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAudioRecordDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetAudioRecordDevice(uint32_t devIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAudioPlayoutDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetAudioPlayoutDevice(uint32_t devIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAuxAudioRecordDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetAuxAudioRecordDevice(uint32_t devIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableAudioPhasePower(bool bEnable) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsAudioPhasePowerEnable() {
		return true;
    }

    ErrCode AbsPhoneEngine::GetAudioMixDevices(list<ItemInfo>& deviceList, list<int>& selectedIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetAudioMixDevices(map<int,bool>& deviceMap) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAudioEncoderParameters(list<ItemInfo>& codecList,
            uint32_t& curCodecIndex,
            bool& bEnableAEC,
            bool& bEnableNS,
            bool& bEnableAGC,
            bool& bEnableVAD,
            bool& bEnableMusicMode,
            bool& bEnableDSPProcess) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetAudioEncoderParameters(uint32_t codecIndex,
            bool bEnableAEC,
            bool bEnableNS,
            bool bEnableAGC,
            bool bEnableVAD,
            bool bEnableMusicMode,
            bool bEnableDSPProcess) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAudioStreamDelayMs(list<ItemInfo>& delayList, uint32_t& curDelayIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetAudioStreamDelayMs(uint32_t curDelayIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoInputStitchMode(LTVideoStitchMode& stitchMode, bool& bStitchSwap) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoInputStitchMode(bool bMainVideo, VideoTemplet& videoTemplet, uint32_t& devicesNameCount, vector<string>& mixerDevicesName) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoInputDeviceInfo(list<ItemInfo>& portList,
            uint32_t& curPortIndex,
            bool bMainVideo) {
		return ErrCode::e_ErrNone;
    }
    ErrCode AbsPhoneEngine::SetVideoInputDevice(uint32_t portIndex, bool bMainVideo, LTVideoStitchMode stitchMode , bool stitchSwap ) {
		return ErrCode::e_ErrNone;
    }
   
    ErrCode AbsPhoneEngine::SetVideoInputDevice(uint32_t portIndex,
            bool bMainVideo,
            VideoTemplet videoTemplet,
            vector<string> mixerDivicesName) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoInputFramerate(list<ItemInfo>& framerateList, uint32_t& curFramerateIndex, bool bMainVideo) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetVideoInputFramerate(uint32_t framerateIndex, bool bMainVideo) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetPTZControlInfo(list<ItemInfo>& protocolList,
            uint32_t& curProtocolIndex,
            bool& bEnableRemoteControl,
            list<ItemInfo>& deviceList,
            uint32_t& curDeviceIndex,
            list<ItemInfo>& baudrateList,
            uint32_t& curBaudrateIndex,
            list<ItemInfo> &presetList,
            uint32_t& curPresetIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetPTZControl(uint32_t protocolIndex,
            bool bEnableRemoteControl,
            uint32_t deviceIndex,
            uint32_t baudrateIndex,
            uint32_t presetIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoOutputDeviceInfo(list<ItemInfo>& portList,
            uint32_t& curPortIndex,
            list<ItemInfo>& resolutionList,
            uint32_t& curResolutionIndex,
            bool bMainVideo) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetVideoOutputDevice(uint32_t portIndex,
            uint32_t resolutionIndex,
            bool bMainVideo) {
		return ErrCode::e_ErrNone;
    }
    
    ErrCode AbsPhoneEngine::GetVideoOutputColorMode(list<ItemInfo>& colorModeList, uint32_t& curColorModeIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetVideoOutputColorMode(uint32_t colorModeIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoEncoderParameters(list<ItemInfo>& codecList, 
            uint32_t& curCodecIndex, 
            list<ItemInfo>& bitrateList,
            uint32_t& curBitrateIndex,
            list<ItemInfo>& bitrateControlList,
            uint32_t& curBitrateControlIndex,
            list<ItemInfo>& keyframeIntervalList,
            uint32_t& curKeyframeIntervalIndex,
            bool bMainVideo) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetVideoEncoderParameters(uint32_t codecIndex, 
            uint32_t bitrateIndex,
            uint32_t bitrateControlIndex,
            uint32_t keyframeIntervalIndex,
            bool bMainVideo) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartVideoLoopbackTest() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopVideoLoopbackTest() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetImageAdjustingParameters(uint32_t& brightness,  //[0-256]
            uint32_t& contrast,    //[0-256]
            uint32_t& saturation,  //[0-256]
            uint32_t& hue) {
		return ErrCode::e_ErrNone;
    }    //[0-256]

    ErrCode AbsPhoneEngine::SetImageAdjustingParameters(uint32_t controId, uint32_t controlVal) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetLocalNetworkConnectionStatus(string ifName,
            bool& bEnableDHCP, 
            string& ip, 
            string& subnetMask, 
            string& gateway, 
            string& preferedDNS, 
            string& standbyDNS,
            string& macAddress) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetLocalNetworkConnection(string ifName,
            bool bEnableDHCP, 
            string ip, 
            string subnetMask, 
            string gateway, 
            string preferedDNS, 
            string standbyDNS) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetLocalNetworkConnectionStatusForIPV6(string ifName,
            bool& bEnableIpv6,
            bool& bEnableDHCP,
            string& ip,
            string& subnetMask,
            string& gateway,
            string& preferedDNS,
            string& standbyDNS,
            string& macAddress) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetLocalNetworkConnectionForIPV6(string ifName,
            bool bEnableIpv6,
            bool bEnableDHCP,
            string ip,
            string subnetMask,
            string gateway,
            string preferedDNS,
            string standbyDNS) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetQoSInfo(uint32_t& audioDSCP, uint32_t& videoDSCP, uint32_t& dataDSCP) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetQoS(uint32_t audioDSCP, uint32_t videoDSCP, uint32_t dataDSCP) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendPTZCommand(PTZCmd cmd, int32_t param , long terminalID ) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetPTZControlDirection(PTZDirection dir) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetPTZControlDirection(PTZDirection& dir) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RestoreFactorySettings() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetMediaInformation(MediaInfo& mediaInfo, MediaType mtype) {
        // 旧语义（absphone_impl.cxx:5319-5327 + ep_sip_connection.cpp:1218）：
        // 先按 InitMediaInformation 置默认值，再按类型取对应流；类型不支持或
        // 无会话返回 e_ErrUnspecified。
        //
        // 新链路：类型 → MediaStreamKind → conference → media 统计库一次性
        // 拷贝出快照。UI 每秒轮询，查询是纯读，不排队进媒体线程。
        mediaInfo = MediaInfo();

        if (!impl_->initialized || !impl_->conferenceControl) {
            return ErrCode::e_ErrUnspecified;
        }

        vcs::MediaStreamKind kind = vcs::MediaStreamKind::Audio;
        switch (mtype) {
        case MediaType::e_Audio:
            kind = vcs::MediaStreamKind::Audio;
            break;
        case MediaType::e_Video:
            kind = vcs::MediaStreamKind::Video;
            break;
        case MediaType::e_ExtendedVideo:
            kind = vcs::MediaStreamKind::SecondaryVideo;
            break;
        default:
            // e_Data：旧实现直接返回 false（没有数据流媒体信息）。
            return ErrCode::e_ErrUnspecified;
        }

        vcs::MediaInfoSnapshot snapshot;
        vcs::Response resp = impl_->conferenceControl->GetMediaInfo(snapshot);
        if (!resp.IsSuccess()) {
            return ErrCode::e_ErrUnspecified;
        }

        const vcs::MediaStreamSnapshot* stream = snapshot.Find(kind);
        if (!stream) {
            return ErrCode::e_ErrUnspecified;
        }

        FillLegacyMediaInfo(*stream, mediaInfo);

        // 排障关键：值是挂断前的最终采样（live=false）还是实时值，
        // 看这一行日志即可，不必回到 MediaInfo 里找状态位。
        VCS_LOG_DEBUG_L(vcs::base::LogModule::Common,
            "[MediaInfo] type=%d live=%d active=%d codec=%s sent=%llu/%u recv=%llu/%u "
            "local=%s remote=%s",
            static_cast<int>(mtype), snapshot.live ? 1 : 0, stream->active ? 1 : 0,
            mediaInfo.sent.codec.c_str(),
            static_cast<unsigned long long>(stream->bytesSent), stream->packetsSent,
            static_cast<unsigned long long>(stream->bytesReceived), stream->packetsReceived,
            mediaInfo.received.localAddress.c_str(),
            mediaInfo.sent.remoteAddress.c_str());

        return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::Ping(string destHost,       
            uint32_t& delay,
            string& remoteAddr,      /// Source address of reply packet.
            string& localAddr,       /// Destination address of reply packet.
            PingStatus& status ) {
		return ErrCode::e_ErrNone;
    }   /// Status of the last ping operation

    ErrCode AbsPhoneEngine::MuteAudio(AudioDeviceType type, bool bMute) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAudioMuteStatus(AudioDeviceType type, bool& bMute) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::MuteVideo(bool bMute) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsVideoMute() {
		return true;
    }

    long AbsPhoneEngine::GetLocalTerminalID() {
		return 0;
    }

    long AbsPhoneEngine::GetChairmanID() {
		return 0;
    }

    ErrCode AbsPhoneEngine::RequestChairman(bool bRequest) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartChairmanSync() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopChairmanSync() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendChairmanSyncCmd(LayoutInfo layout, long targetTerminalID ) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartChairmanBroadcast() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopChairmanBroadcast() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::LockConference() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::UnlockConference() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RequestFloor(bool bRequest) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RequestVideoSource(long terminalID, uint32_t streamID, bool bRequest) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RequestFloorAssign(long terminalID, bool bRequest) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RequestBroadcastTerminal(long terminalID, bool bRequest) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RequestBroadcastMultiPicture(bool bRequest) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::DropTerminal(long terminalID) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::DropConference() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetTerminalListInfo(list<TerminalInfo>& terminalList) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendConferencePassword(string password) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendUserInput(string cmd) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetVideoLayout(uint32_t layoutID) {
		return ErrCode::e_ErrNone;
    }  //[0, 8]

    ErrCode AbsPhoneEngine::GetVideoLayoutId(uint32_t& layoutID) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetFastCallStatus(bool& bEnabled,  string& callAddress) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetFastCall(bool bEnable, string callAddress) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartFastCall() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetHDDeviceType(HDDeviceType& type) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetConferenceControlMode(ConfControlMode& mode) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsFlowControlEnabled() {
		return true;
    }

    ErrCode AbsPhoneEngine::EnableFlowControl(bool bEnable) {
		return ErrCode::e_ErrNone;
    }

    string AbsPhoneEngine::GetAppVersion(VersionType type) {
		return "";
    }

    ErrCode AbsPhoneEngine::SetNTPStatus(bool enable, string serverAddr) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetNTPStatus(bool& enabled, string& serverAddr) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetRemoteApplication(string& application) {
		return ErrCode::e_ErrNone;
    }

    string AbsPhoneEngine::GetTimeZone() {
		return "";
    }

    ErrCode AbsPhoneEngine::SetTimeZone(string timeZone) {
		return ErrCode::e_ErrNone;
    }

    uint32_t AbsPhoneEngine::GetCallDuration() {
		return 0;
    }

    uint32_t AbsPhoneEngine::GetMiniMCUMaxParticipants() {
		return 0;
    }

    ErrCode AbsPhoneEngine::GetProductModel(ProductModel& model) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetProductModel(ProductModel model) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetSystemLanguage(list<ItemInfo>& langList, uint32_t& curLangIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetSystemLanguage(uint32_t langIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendPCMAudioFrame(uint8_t* frameBuf, uint32_t frameSize) {
		return ErrCode::e_ErrNone;
    }  //just for ios/Android

    ErrCode AbsPhoneEngine::SendH264VideoFrame(uint8_t* frameBuf, uint32_t frameSize, uint32_t streamID ) {
		return ErrCode::e_ErrNone;
    }  //just for ios/Android

    ErrCode AbsPhoneEngine::SendH265VideoFrame(uint8_t* frameBuf, uint32_t frameSize, uint32_t streamID ) {
		return ErrCode::e_ErrNone;
    }  //just for ios/Android

    ErrCode AbsPhoneEngine::SendYUV420VideoFrame(uint8_t* frameBuf, uint32_t width, uint32_t height, uint32_t streamID ) {
		return ErrCode::e_ErrNone;
    }  //just for Android/Windows

    ErrCode AbsPhoneEngine::SendRGB24VideoFrame(uint8_t* frameBuf, uint32_t width, uint32_t height, uint32_t streamID ) {
		return ErrCode::e_ErrNone;
    }  //just for  Android/Windows

    ErrCode AbsPhoneEngine::EnableAndroidHardwareDecoder(bool enable) {
		return ErrCode::e_ErrNone;
    }  //just for Android

    ErrCode AbsPhoneEngine::GetAndroidHardwareDecodeStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetWindowsHardwareCodecStatus(bool& enableEncoder, bool& enableDecoder) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableWindowsHardwareCodec(bool enableEncoder, bool enableDecoder) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendWhiteboardMessage(uint8_t* msgBuf, uint32_t msgSize) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendDesktopControlMessage(uint8_t* msgBuf, uint32_t msgSize) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendVoteMessage(uint8_t* msgBuf, uint32_t msgSize) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendCollaborativeBrowsingMessage(uint8_t* msgBuf, uint32_t msgSize) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::ClearWhiteboard(bool eraseBGPicture) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendWhiteboardBackgroundPicture(uint8_t* data, uint32_t size) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoLayoutInfo(LayoutInfo & layout) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartVideoPolling(uint32_t destWindowID, uint32_t timeInterval, list<long> pollingList) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopVideoPolling() {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsVideoPollingStarted() {
		return true;
    }

    ErrCode AbsPhoneEngine::GetVideoPollingWindow(uint32_t& windowId) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartRecording() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopRecording() {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsRecorderStarted() {
		return true;
    }

    bool AbsPhoneEngine::IsWifiEnable() {
		return true;
    }

    ErrCode AbsPhoneEngine::SetWifiEnable(bool isEnable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::ScanWifiAccessPoint(list<WifiAccessPointParam> &wifiAccessPointParam) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetWifiConnectionStatus(string &essid, string &accessPoint) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::ConnectWifi(string essid, string passwd) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::DisconnectWifi() {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::Is4GEnable() {
		return true;
    }

    ErrCode AbsPhoneEngine::Set4GEnable(bool isEnable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::Get4GSiagnalPower(int &signalPower) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsNetCardExist(string ifName) {
		return true;
    }

    ErrCode AbsPhoneEngine::SetAuxScreenVideo(AuxScreenVideoType videoType) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAuxScreenVideo(AuxScreenVideoType& videoType) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetTerminalEncoderBitrate(long terminalID, bool bMainVideo, list<uint32_t>& bitrateList, uint32_t& curBitrateIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetTerminalEncoderBitrate(long terminalID, bool bMainVideo, uint32_t curBitrateIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendBannerMessage(string message) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendScrollingMessage(string message) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendShortMessage(string message, long terminalID) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableExecuteChairmanSyncAction(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetExecuteChairmanSyncActionStatus(bool& enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::ExchangeWindow(uint32_t winId1, uint32_t winId2) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::CaptureOneAuxVideoFrameAsWBPicture() {
		return ErrCode::e_ErrNone;
    }
    
    ErrCode AbsPhoneEngine::CaptureOneAuxI420VideoFrame(uint8_t* buf, uint32_t& width, uint32_t& height, uint32_t& outSize) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableEliminateMosaic(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetEliminateMosaicStatus(bool& enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetBootPresetNumber(list<ItemInfo>& presetNumList, uint32_t& curPresetIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetBootPresetNumber(uint32_t presetIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableSyncPolling(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetSyncPollingStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableMultiSliceMode(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetMultiSliceMode(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetVADWindow(uint32_t winId) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVADWindow(uint32_t& winId) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartMonitoringMode() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopMonitoringMode() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::OpenRTSP(string url, string name) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopRTSP(string url) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetLocalNameOSDParam(ITCOSDParams& params) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetLocalNameOSDParam(ITCOSDParams params) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetConferenceNameOSDParam(ITCOSDParams& params) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetConferenceNameOSDParam(ITCOSDParams params) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableRunningStatusLED(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableH264SoftDecoding(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetH264SoftDecodingStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableVideoRTPOverTCPMode(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoRTPOverTCPModeStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableAudioRTPOverTCPMode(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAudioRTPOverTCPModeStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetAudioPlayerChannel(AudioChannel channel) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetAudioPlayerChannel(AudioChannel& channel) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StartWebRTCVoiceEngine() {
		return ErrCode::e_ErrNone;
    }  //Only for Android 

    ErrCode AbsPhoneEngine::StopWebRTCVoiceEngine() {
		return ErrCode::e_ErrNone;
    }  //Only for Android

    ErrCode AbsPhoneEngine::SetVideoAutoLayoutMode(VideoAutoLayoutMode mode) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVideoAutoLayoutMode(VideoAutoLayoutMode& mode) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableVCSOverWriteLocalUsername(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetVCSOverWriteLocalUsernameStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetMicrophoneSignalLevel(uint32_t& level) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetSpeakerSignalLevel(uint32_t& level) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetMicrophoneGain(list<ItemInfo>& gainList, uint32_t& curGainIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetMicrophoneGain(uint32_t curGainIndex) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EnableLTMixHdmiAudio(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetEnableLTMixHdmiAudio(bool& enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EableProtocol(ProtoType protoType,bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::IsEnableProtocol(ProtoType protoType,bool& enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::PcmVolumeControl(int16_t* pcmSrc, uint32_t pcmSrcByteSize, int16_t* pcmDst, float coefficient) {
		return ErrCode::e_ErrNone;
    }

    //add by EF 
    ErrCode AbsPhoneEngine::ChangeLiveMemberToInteract(string dstLiveMemberId) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::ChangeMemberToLive(long dstMemberId) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::AnswerApplyForInteract(ErrCode code, string applyLiveMemberId, bool result) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::InforMemberChangeToLiveResult(bool result, ErrCode code) {
		return ErrCode::e_ErrNone;
    }
    
    ErrCode AbsPhoneEngine::SetLocalContent(string content) {
		return ErrCode::e_ErrNone;
    }
    //end
    //for sip Enpoint
    ErrCode AbsPhoneEngine::SetSipParams(bool enableSipProtocol, SipAccountInfo accInfo) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetSipParams(bool& enableSipProtocol, vector<SipAccountInfo>& accs) {
		return ErrCode::e_ErrNone;
    }  

    ErrCode AbsPhoneEngine::SipAnswerCall(bool ok) {
		if (impl_->conferenceControl) {
			return impl_->conferenceControl->AnswerCall(ok).IsSuccess()
				? ErrCode::e_ErrNone : ErrCode::e_ErrUnspecified;
		}
		return ErrCode::e_ErrUnspecified;
    }

    ErrCode AbsPhoneEngine::SendSipDTMF(string dtmf) {
		return ErrCode::e_ErrUnspecified;
    }
    //end 
    
    ErrCode AbsPhoneEngine::EnableHDMIHotPlugMode(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetHDMIHotPlugStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::VCSEnableFullScreenForAuxVideo(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::VCSGetFullScreenModeStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SetRTMPLocalCacheSec(int sec) {
		return ErrCode::e_ErrNone;
    }
    
    ErrCode AbsPhoneEngine::GetRTMPLocalCacheSec(int& sec) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::OpenRTMP(string rtmpAddress, bool bEncodedVideoCallback) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::CloseRTMP() {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::LoginRelatedInfoRequest(string url, int brand) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::InformStopVCSExtvideo(long srcMemberId, long dstMemberId) {
		return ErrCode::e_ErrNone;
    }
    
    ErrCode AbsPhoneEngine::UpdateUIImage(uint8_t* buf, uint32_t size, uint32_t portID ) {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::SetMainStreamMarkEnable(bool bMark) {
		return true;
    }

     ErrCode AbsPhoneEngine::AssignVideoSource(long terminalID, uint32_t streamID, bool bRequest) {
		 return ErrCode::e_ErrNone;
    }

     ErrCode AbsPhoneEngine::SetCallProtoType(CallProtoType type) {
		 return ErrCode::e_ErrNone;
    }

     ErrCode AbsPhoneEngine::GetCallProtoType(CallProtoType& type) {
		 return ErrCode::e_ErrNone;
    }

     ErrCode AbsPhoneEngine::IndicateVerifyPassword(bool isNeedToVerifyPassword) {
		 return ErrCode::e_ErrNone;
    }

     ErrCode AbsPhoneEngine::SetOutputInterface(uint32_t streamID, VideoOutputInterface VO) {
		 return ErrCode::e_ErrNone;
    }

     ErrCode AbsPhoneEngine::GetOutputInterface(uint32_t streamID, VideoOutputInterface& VO) {
		 return ErrCode::e_ErrNone;
    }
    
     ErrCode AbsPhoneEngine::CloseSubSystems() {
		 return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsMixCapatyEnough(bool& mainOut4k, bool& extOut4k, bool& mainIn4k1, bool&  mainIn4k2,bool& extIn4k) {
		return true;
    }
     /*========================================== HTTP APIs (Blocking APIs)==================================*/
    ErrCode AbsPhoneEngine::CreateQuickMeeting(ITCHttpParam param,         // not null, url info
                                       string templateId,          // use default template when param is empty
                                       string meetingNumber,       // auto assign when param is empty
                                       string meetingName,         // not null   
                                       string startTime,           // use current date time when param is empty
                                       string endTime,             // not null
                                       string meetingPasswd,       // use default password when param is empty
                                       string chairmanPasswd,      // use default password when param is empty      
                                       string livePasswd,          // use default password when param is empty      
                                       int userId,                 // not null
                                       string memberIds,           // only yourself when param is empty, use ";" to seperate memberId
                                       ErrCode &retCode,               // http return code
                                       string &retMeetingId,       
                                       string &retMeetingNumber) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::StopMeeting(ITCHttpParam param, string meetingId, ErrCode &retCode) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RemoveMeeting(ITCHttpParam param, string meetingId, ErrCode &retCode) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::ListMeetingMembers(ITCHttpParam param, string meetingId, ErrCode &retCode, list<ITCMeetingMemberInfo> &membersInfoList) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::JoinMeeting(ITCHttpParam param, string meetingNumber, string ipAddr, ErrCode &retCode, string &confAddress, string &meetingPasswdMd5) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetConferenceLiveAddress(ITCHttpParam param, 
                                             string meetingID, 
                                             string meetingNumber, 
                                             string ipaddr, 
                                             ErrCode &retCode, 
                                             string &retMeetingId, 
                                             string &retMeetingNumber,
                                             ProtoType &retProtoType,
                                             string &retLiveAddress, 
                                             string &retLivePasswdMd5) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::PreUploadShareFile(ITCHttpParam param, int userId, int size, ErrCode &retCode) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::EncodeYUVVideoFrame(uint8_t* buffer, uint32_t width, uint32_t height, uint32_t streamId, ITCYuvType yuvType) {
		return ErrCode::e_ErrNone;
    }
      
    ErrCode AbsPhoneEngine::SetRenderWindow(uint32_t streamId, void* windows, uint32_t width, uint32_t height) {
		return ErrCode::e_ErrNone;
    }
   /*=======================================================================================================*/

    /*========================================== Websocket APIs ===========================================*/
    ErrCode AbsPhoneEngine::WebsocketConnect(string ip, uint16_t port, string path, bool bWSS) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::WebsocketDisconnect() {
		return ErrCode::e_ErrNone;
    }

    bool AbsPhoneEngine::IsWebsocketConnected() {
		return true;
    }

    ErrCode AbsPhoneEngine::WebsocketSendMsg(string msg) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RequestJoinConferenceFromLive(string liveUserId) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendJoinConferenceFromLiveResult(string liveUserId, int result) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::SendChatMessage(string liveUserId, string content) {
		return ErrCode::e_ErrNone;
    }
   /*=======================================================================================================*/
   
    ErrCode AbsPhoneEngine::EnableWebRTCAudioProcessing(bool enable) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::GetWebRTCAudioProcessingStatus(bool& enabled) {
		return ErrCode::e_ErrNone;
    }

    ErrCode AbsPhoneEngine::RequestKeyframe(uint32_t streamID) {
		return ErrCode::e_ErrNone;
    } 

} // namespace absphone
} // namespace vcs
