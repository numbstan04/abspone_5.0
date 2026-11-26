#ifndef ABSPHONE_DEF_H
#define ABSPHONE_DEF_H




#include <stdint.h>
#include <string>
#include <list>
#include <map>
#include <queue>
#include <vector>
#include <time.h>

// 使用新系统的 std::string 兼容
#ifndef STRING_TYPE
#include <string>
#define STRING_TYPE std::string
using string = std::string;
#endif

// 使用标准库的组件
using std::vector;
using std::list;
using std::map;
using std::queue;

// 宏定义
#define MAXVIDEOMIXERS              100
#define MAX_MAINSCREEN_VIDEO_WINDOW 64
#define MAX_VIDEO_WINDOW            (MAX_MAINSCREEN_VIDEO_WINDOW + 1)
#define MAX_ENC_CHANNELS            4
#define MAX_HDMI_COUNT              2
#define MAX_VID_INPUT               5

// 错误码
enum class ErrCode : int {
    e_ErrNone = 0,
    e_ErrUnspecified,
    e_ErrEndedByRemoteUser,
    e_ErrEndedByCallerAbort,
    e_ErrEndedByRefusal,
    e_ErrEndedByNoAnswer,
    e_ErrEndedByTransportFail,
    e_ErrEndedByCapabilityExchange,
    e_ErrEndedByNoAccept,
    e_ErrEndedByAnswerDenied,
    e_ErrEndedByNoUser,
    e_ErrEndedByNoBandwidth,
    e_ErrEndedByUnreachable,
    e_ErrEndedByHostOffline,
    e_ErrEndedByNoEndPoint,
    e_ErrEndedByConnectFail,
    e_ErrEndedByCallCompleted,
    e_ErrInvalidParameters,
    e_ErrServerInternalError,
    e_ErrAccountAlreadyLoggeg,
    e_ErrAccountExpired,
    e_ErrAccountNotExist,
    e_ErrUnspecifiedPlatform,
    e_ErrIncorrectUsernameOrPassword,
    e_ErrConfIsNotVCS,
    e_ErrChairmanIsNotExsit,
    e_ErrConfNotExsit,
    e_ErrConfLocked,
    e_ErrBeyondConfMaxMbrcount,
    e_ErrBeyondMCUMaxMbrCount,
    e_ErrBeyondEnterpriseMaxMbrCount,
    e_ErrBeyondPlatformMaxMbrCount,
    e_ErrConfIsNotLive,
    e_ErrBeyondLiveMbrCount,
    e_ActiveRefusedByChairman,
    e_ErrBeyondEnterprisePlatformMaxMbrCount,
    e_ErrBeyongSystemLicens,
    e_ErrMeetingIDNotMatchRoomID,
    e_BeyondConfsCount,
    e_StartVideoEngineFail,
    e_NotEnoughMP,
    e_ErrIncorrectMPVideoParams,
    e_ErrStartFailUnsupportMP,
    e_ErrPleaseSelectMPResource,
    e_ErrMPResourceAlreadyBound,
    e_ErrMPResourceNotFound,
    e_ErrMPResourceInUse
};

// 协议类型
enum class ProtoType {
    e_ProtoVCS,
    e_ProtoH323,
    e_ProtoSIP
};

// 视频编解码类型
enum class ITCVideoCodecType {
    e_VideoCodecH264,
    e_VideoCodecH265,
    e_VideoCodecYuv420SP
};

// YUV类型
enum class ITCYuvType {
    e_YuvTypeI420,
    e_YuvTypeNV21
};

// 编码器颜色格式
enum class ITCEncColorFormat {
    e_Coder_Yuv420 = 0,
    e_Coder_Yuv444
};

// 邀请模式
enum class InviteMode {
    e_alwaysInvite,
    e_refuseInvite,
    e_limitedInvite
};

// 窗口模式
enum class WinMode {
    e_WinNormal,
    e_WinExtVideo,
    e_WinPoll,
    e_WinVad,
    e_WinChair,
    e_WinAutoFilling
};

// 会议媒体类型
enum class ConferenceMediaType {
    e_BothVideoAndAudio,
    e_AudioOnly
};

// HD设备类型
enum class HDDeviceType {
    e_HDD0,
    e_HDD1
};

// 编解码器类型
enum class CoderType {
    e_CoderTerminal,
    e_CoderTerminal_MB02,
    e_CoderMCU,
    e_CoderMiniMCU,
    e_CoderTerminal_LT,
    e_CoderTerminal_LT_4K,
    e_CoderTerminal_4K,
    e_CoderTerminal_B500,
    e_CoderTerminal_B510,
    e_CoderTerminal_A500,
    e_CoderTerminal_A510
};

// 视频profile
enum class VideoProfile {
    e_Baseline,
    e_MainProfile,
    e_HighProfile,
    e_H265MainProfile,
    e_H265Main10Profile
};

// GOP模式
enum class VideoGopMode {
    e_Venc_Gopmode_NORMALP = 1,
    e_Venc_Gopmode_TSVC2,
    e_Venc_Gopmode_TSVC3,
    e_Venc_Gopmode_TSVC4
};

// 参考帧类型
enum class VideoRefType {
    e_Base_IDRSLICE = 0,
    e_Base_Pslice_REFTOIDR,
    e_Base_Pslice_REFBYBASE,
    e_Base_Pslice_REFBYENHANCE,
    e_Enhance_Pslice_REFBYENHANCE,
    e_Enhance_Pslice_NOTFORREF,
    e_Enhance_Pslice_BUTT
};

// 码率控制算法
enum class BitrateControlAlgorithm {
    e_VBR,
    e_CBR
};

// 视频码率策略
enum class VideoBitrateStrategy {
    e_BitratePriority,
    e_QualityPriority
};

// OSD类型
enum class ITCOSDType {
    e_OSDBannerText,
    e_OSDRollingText,
    e_OSDUserName,
    e_OSDLocalName,
    e_OSDUserLogo,
    e_OSDSubtitles,
    e_OSDNameBadge
};

// 会议状态
enum class ConfStatus {
    e_NotStarted = 0,
    e_Ongoing,
    e_Stoped,
    e_CreateFail,
    e_Deleted
};

// OSD参数
struct ITCOSDParams {
    uint32_t position;      // 0:topleft 1:lowerleft 2:topright 3:lowerright
    uint32_t globalAlpha;
    uint32_t fontSize;      // 0:small, 1:middle, 2:big
    uint32_t fontColor;     // 0:reb 1:yellow 2:blue 3:green 4:white 5:black
    uint32_t bgColor;       // 0:transparent 1:red 2:yellow 3:blue 4:green 5:white 6:black
    uint32_t osdEnable;     // 0:fasle 1:true
    uint32_t rollingTimes;
    uint32_t showSeconds;   // 1~5 secs, use by subtitles
    uint32_t fontType;
    uint32_t rollingSpeed;
    int32_t  enBold;
    int32_t  posX;
    int32_t  posY;
};

// 布局信息
struct LayoutInfo {
    uint32_t layoutId;
    uint32_t totalWindow;
    string   userName[MAX_VIDEO_WINDOW];
    long memberId[MAX_VIDEO_WINDOW];
    uint32_t streamId[MAX_VIDEO_WINDOW];
};

// 会议角色
enum class ConfRole {
    e_role_none,
    e_role_chairman,
    e_role_speaker,
    e_role_audience
};

// 会议角色信息
struct ConfRoleInfo {
    long chairmanRoleMemberId;
    long speakerRoleMemberId;
    long chairmanRoleSrcMemberId;
    ConfRole chairmanRoleSrc;
    long speakerRoleSrcMemberId;
    ConfRole speakerRoleSrc;
    long audienceRoleSrcMemberId;
    ConfRole audienceRoleSrc;
    bool enableChairmanPolling;
    vector<long> chairmanPollingMemberIds;
    uint32_t chairmanPollingTimeInterval;
    bool enableSpeakerPolling;
    vector<long> speakerPollingMemberIds;
    uint32_t speakerPollingTimeInterval;
};

// MP类型
enum class MPType {
    e_hardWare,
    e_softWare
};

// 会议编解码类型
enum class ConfCodecType {
    e_NotCompleteCodec,
    e_CompleteCodec
};

// SIP传输协议
enum class SipTransProtocol {
    e_udp = 0,
    e_tcp,
    e_tls
};

// DTMF模式
enum class DTMFMode {
    e_sipInfo = 0,
    e_rfc2833 = 1,
    e_tone = 2
};

// SRTP算法
enum class SRTPAlgorithm {
    e_Auto = 0,
    e_SM4_128_ECB_HMAC_SM3,
    e_SM4_128_CBC_HMAC_SM3,
    e_SM4_128_OFB_HMAC_SM3,
    e_SM4_128_CTR_HMAC_SM3,
    e_AES_256_CM_HMAC_SHA1_80,
    e_AES_256_CM_HMAC_SHA1_32,
    e_AES_CM_128_HMAC_SHA1_80,
    e_AES_CM_128_HMAC_SHA1_32
};

// SRTP使用模式
enum class SRTPUse {
    e_disable = 0,
    e_optional,
    e_mandatory
};

// NAT配置
struct NatConfig {
    bool enableICE;
    bool enableMediaSTUN;
    vector<string> stunServers;
    bool enableTURN;
    string turnServer;
    string turnUsername;
    string turnPassword;
    SRTPUse mediaSRTP;
    SRTPAlgorithm srtpAlgorithm;
};

// SIP账户信息
struct SipAccountInfo {
    bool enable;
    bool enableReg;
    uint32_t regTimeout;
    string registrarUri;
    vector<string> proxies;
    string userName;
    string authName;
    string passWord;
    string displayName;
    bool isReg;
    SipTransProtocol protocol;
    DTMFMode dtmfMode;
    NatConfig natConfig;
    string localIp;
};

// 视频窗口信息
struct VideoWindow_absphone {
    long memberID;
    uint32_t streamID;
    WinMode mode;  // 0:normal 1:ext_video 2:poll 3:vad 4:chair
};

// 视频布局
struct VideoLayout {
    VideoWindow_absphone win[MAX_VIDEO_WINDOW];
    uint32_t layoutID;
    uint32_t windows;
};

// 视频编码器参数
struct VideoEncoderParam_s {
    ITCVideoCodecType encType;
    VideoProfile encProfile;
    BitrateControlAlgorithm encRcMode;
    VideoBitrateStrategy bitrateStrategy;
    ITCEncColorFormat colorFormat;
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;
    uint32_t framerate;
    uint32_t keyframeInterval;
    uint32_t minQp;
    uint32_t maxQp;
    uint32_t minIQp;
    uint32_t maxIQp;
    uint32_t initQp;
    uint32_t maxSliceSize;

    bool operator==(const VideoEncoderParam_s& encParams) const {
        return (encParams.encType == encType &&
                encParams.encProfile == encProfile &&
                encParams.encRcMode == encRcMode &&
                encParams.bitrateStrategy == bitrateStrategy &&
                encParams.colorFormat == colorFormat &&
                encParams.width == width &&
                encParams.height == height &&
                encParams.bitrate == bitrate &&
                encParams.framerate == framerate &&
                encParams.keyframeInterval == keyframeInterval &&
                encParams.minQp == minQp &&
                encParams.maxQp == maxQp &&
                encParams.minIQp == minIQp &&
                encParams.maxIQp == maxIQp &&
                encParams.initQp == initQp &&
                encParams.maxSliceSize == maxSliceSize);
    }
};
using VideoEncoderParam = VideoEncoderParam_s;

// 视频编解码器
struct VideoCodec {
    string codec;
    uint32_t bitrate;
    BitrateControlAlgorithm bitrateControl;
    uint32_t keyframeInterval;
    bool enableMultiSliceMode;
    uint32_t channelId;
};

// 混音器参数
struct MixerParams {
    VideoCodec videoCodecs[MAX_ENC_CHANNELS];
    ITCOSDParams osdName;
    ITCOSDParams osdBanner;
    ITCOSDParams osdRollingMsg;
    ITCOSDParams osdLogo;
    ITCOSDParams osdSubtitles;
    ITCOSDParams osdNameBadge;
    string bannerText;
    string rollingText;
    string ip;
    uint32_t channelsCount;
};

// 视频参数
struct VideoParams {
    string codec;
    uint32_t defaultBandWidth;
    uint32_t maxBandWidth;
    MixerParams mixerParams[MAXVIDEOMIXERS];
    uint32_t videoMixerCount;
};

// 会议创建参数
struct ConfSetupParams {
    string roomID;
    string name;
    string creator;
    string confPassword;
    bool autoMute;
    bool enableFEC;
    bool autoSplitScreen;
    bool enableSubPicLoopback;
    bool enableFlowControl;
    bool enableQoS;
    uint32_t qos_dscp;
    bool enableAnonymous;
    InviteMode inviteMode;
    uint32_t maxInviteTimes;
    string confDescription;
    string audioCodec;
    VideoLayout layout;
    bool enableCascade;
    uint32_t memberCountLimit;
    bool enablePTZ;
    bool enableh235Encription;
    bool bMiniMCU;
    bool enableEliminateMosaic;
    ConferenceMediaType confMediaType;
    ProtoType protoType;
    string startTime;
    string endTime;
    string mcuIP;
    string ftpAddress;
    string ftpAccount;
    string ftpPassword;
    string extRoomID;
    string chairmanPassword;
    bool enableH239IndependentTransmit;
    bool enableVideoRTPOverTCPMode;
    bool enableAudioRTPOverTCPMode;
    bool enableVCSManuallySendH239;
    uint32_t defaultLayoutID;
    uint32_t maxVideoWindows;
    uint32_t DefaultCascadeMixerID;
    VideoCodec extendedVideoCodec;
    VideoParams videoParams;
    ConfRoleInfo confRoleInfo;
    ConfCodecType confCodecType;
    bool enableAudioSwitchMode;
};

// 列表项信息
struct ItemInfo {
    uint32_t index;
    string name;
    bool isActive;
};

// HTTP参数
struct ITCHttpParam {
    bool bHttps;
    string ip;
    uint16_t port;
    uint32_t timeoutMs;
    list<string> headerList;
};

// 会议成员信息
struct ITCMeetingMemberInfo {
    int id;
    string name;
    string account;
    string phoneEmail;
    string avatar;
    int enterpriseId;
    string enterpriseName;
};

// WebSocket断开类型
enum class ITCWSDisconnectType {
    e_WS_Null,
    e_WS_NormalClose,
    e_WS_PeerClose,
    e_WS_ErrorClose
};

// 终端类型
enum class ITCTerminalType {
    e_ITCEndpoint = 50,
    e_ITCMCU = 190
};

// 静音类型
enum class MuteType {
    e_MuteAudioReceive = 1,
    e_MuteAudioTransmit = 2,
    e_MuteVideoReceive = 4,
    e_MuteVideoTransmit = 8
};

// 呼叫响应
enum class AnswerCallResponse {
    e_AnswerCallNow,
    e_AnswerCallDenied
};

// 应答模式
enum class AnswerMode {
    e_ManualAnswer,
    e_AutoAnswer,
    e_DoNotDisturb,
    e_WaitAnswer
};

// 呼叫方向
enum class CallDirection {
    e_OutgoingCall,
    e_IncomingCall,
    e_MissedCall
};

// 呼叫结果
enum class CallResult {
    e_CallSetup,
    e_CallFailed
};

// 颜色模式
enum class ColorMode {
    e_RGB,
    e_YUV
};

// 媒体信息
struct MediaInfo {
    struct SentInfo {
        string codec;
        string format;
        uint32_t bitrate;
        uint64_t bytesSent;
        uint32_t packetsSent;
        uint32_t packetsLost;
        float fractionLost;
        uint32_t averageSendTime;
        uint32_t framerate;
        uint32_t jitter;
        uint32_t rttDelay;
        string remoteAddress;
        bool bEncrypted;
        uint32_t packetsSentInSecond;
        uint32_t packetsLostInSecond;
    } sent;

    struct ReceivedInfo {
        string codec;
        string format;
        uint32_t bitrate;
        uint64_t bytesReceived;
        uint32_t packetsReceived;
        uint32_t packetsLost;
        float fractionLost;
        uint32_t averageReceiveTime;
        uint32_t framerate;
        uint32_t jitter;
        uint32_t rttDelay;
        string localAddress;
        bool bEncrypted;
        uint32_t packetsReceivedInSecond;
        uint32_t packetsLostInSecond;
    } received;

    string remoteApplication;

    MediaInfo() {
        sent.codec = "NA";
        sent.format = "NA";
        sent.bitrate = 0;
        sent.bytesSent = 0;
        sent.packetsSent = 0;
        sent.packetsLost = 0;
        sent.fractionLost = 0;
        sent.averageSendTime = 0;
        sent.framerate = 0;
        sent.jitter = 0;
        sent.rttDelay = 0;
        sent.remoteAddress = "NA";
        sent.bEncrypted = false;
        sent.packetsSentInSecond = 0;
        sent.packetsLostInSecond = 0;

        received.codec = "NA";
        received.format = "NA";
        received.bitrate = 0;
        received.bytesReceived = 0;
        received.packetsReceived = 0;
        received.packetsLost = 0;
        received.fractionLost = 0;
        received.averageReceiveTime = 0;
        received.framerate = 0;
        received.jitter = 0;
        received.rttDelay = 0;
        received.localAddress = "NA";
        received.bEncrypted = false;
        received.packetsReceivedInSecond = 0;
        received.packetsLostInSecond = 0;
    }
};

// 媒体类型
enum class MediaType {
    e_Video,
    e_ExtendedVideo,
    e_Audio,
    e_Data
};

// Ping状态
enum class PingStatus {
    e_PingSuccess,
    e_NetworkUnreachable,
    e_HostUnreachable,
    e_PacketTooBig,
    e_RequestTimedOut,
    e_BadRoute,
    e_TtlExpiredTransmit,
    e_TtlExpiredReassembly,
    e_SourceQuench,
    e_MtuChange,
    e_PingGeneralError,
    e_NumStatuses
};

// 前转呼叫模式
enum class ForwardCallMode {
    e_ForwardCallDisabled,
    e_ForwardCallWhenBusy,
    e_ForwardCallAnyway
};

// 音频设备类型
enum class AudioDeviceType {
    e_RecordDevice,
    e_PlayoutDevice
};

// QoS选项
enum class QoSOption {
    e_DiffServ,
    e_IPPrecedence
};

// 终端角色
enum class TerminalRole {
    e_OrdinaryMember,
    e_Chairman
};

// 终端信息
struct TerminalInfo {
    TerminalRole role;
    int32_t terminalID;
    string name;
    bool bVideoProvider;
    bool bFloor;
};

// 网络类型
enum class NetType {
    e_static = 0,
    e_dhcp
};

// 路由标志
enum class RouteFlag {
    e_net = 0,
    e_host
};

// 会议控制模式
enum class ConfControlMode {
    e_FreeMode,
    e_ChairControlMode,
    e_DirectorControlMode
};

// 网卡信息
struct NetCardInfo {
    NetType netType;
    string ifname;
    string ipaddr;
    string netmask;
    string defaultGW;
    string dns;
    string mac;
};

// 视频质量
enum class VideoQuality {
    e_HighVideoQuality,
    e_LowVideoQuality
};

// 连接状态
enum class ConnectionStatus {
    e_Connected,
    e_Connecting,
    e_Disconnect
};

// 网络状态
enum class EPNetworkStatus {
    e_good,
    e_common,
    e_bad,
    e_terrible,
    e_max_level
};

// 成员信息
struct MemberInfo {
    string name;
    long memberID;
    bool active;
    bool muteAudioRxCodec;
    bool muteAudioTxCodec;
    bool muteVideoRxCodec;
    bool muteVideoTxCodec;
    bool enableVad;
    int32_t audioLevel;
    int32_t manualGainDB;
    int32_t outputGainDB;
    uint32_t windowPos;
    uint32_t windowPos2;
    bool isChair;
    bool isH239Sharer;
    bool isMCU;
    bool isH239Assigned;
    bool isChairAssigned;
    bool isAnonymous;
    bool isBlack;
    bool isMiniMCULocalVideo;
    VideoQuality videoQuality;
    uint32_t videoChannelId;
    long videoSourceID;
    long audioSourceID;
    ConnectionStatus connStatus;
    bool isVCSMember;
    string VCSAccount;
    ConfRole confRole;
    EPNetworkStatus epNetworkStatus;
    bool isVCSCall;
};

// 会议信息
struct ConferenceInfo {
    string roomID;
    string name;
    string creator;
    uint32_t duration;
    uint32_t activeMembers;
    uint32_t activeLinuxMembers;
    uint32_t activeWindowsMembers;
    uint32_t activeMACMembers;
    uint32_t activeAndroidMembers;
    uint32_t activeIOSMembers;
    uint32_t memberCountLimit;
    ConfControlMode mode;
    bool isRecorderRunning;
    string startTime;
    string endTime;
    string mcuIP;
    ProtoType protoType;
    string confPassword;
    uint32_t maxInviteTimes;
    InviteMode inviteMode;
    bool enableVCSManuallySendH239;
};

// PTZ方向
enum class PTZDirection {
    e_PTZLocal,
    e_PTZRemoteSite
};

// PTZ命令
enum class PTZCmd {
    e_PanLeft,
    e_PanRight,
    e_TiltUp,
    e_TiltDown,
    e_Home,
    e_FocusOut,
    e_FocusIn,
    e_ZoomOut,
    e_ZoomIn,
    e_IrisOut,
    e_IrisIn,
    e_StopAction,
    e_StoreAsPreset,
    e_ActivatePreset,
    e_ClearPreset,
    e_WbAuto,
    e_WbIndoor,
    e_WbOutdoor,
    e_WbOnepush,
    e_WbManual,
    e_WbRGain,
    e_WbBGain,
    e_Contrast,
    e_Bright,
    e_Acutance,
    e_BlackLightOn,
    e_BlackLightOff,
    e_FlickOff,
    e_Flick50hz,
    e_Flick60hz,
    e_VFlipOn,
    e_VFlipOff,
    e_LRReverseOn,
    e_LRReverseOff,
    e_2DNoiseReduce,
    e_3DNoiseReduce,
    e_MtpNRStrth,
    e_MtnNRStrth,
    e_Saturation,
    e_AeAuto,
    e_AeManual,
    e_AeShutter,
    e_AeIris,
    e_AeBright,
    e_ShutterLevel,
    e_IrisLevel,
    e_GainLevel,
    e_ExposureLevel,
    e_BrightLevel,
    e_WdrOn,
    e_WdrOff,
    e_WdrLevel,
    e_PanTiltReset,
    e_ImageReset,
    e_AllReset
};

// H239通道状态
enum class H239ChannelStatus {
    e_h239TransmitOnly,
    e_h239ReceiveOnly,
    e_h239TransmitAndReceive,
    e_h239NotActive
};

// 内存信息
struct MemoryInfo {
    unsigned long memoryTotal;
    unsigned long memoryFree;
    unsigned long memoryUsage;
};

// 磁盘信息
struct DiskInfo {
    unsigned long diskTotal;
    unsigned long diskFree;
    unsigned long diskUsage;
};

// CPU占用
struct CPU_OCCUPY {
    char name[20];
    unsigned int user;
    unsigned int nice;
    unsigned int system;
    unsigned int idle;
};

// 路由信息
struct Route_Info {
    string ipAddr;
    string mask;
    string gateWay;
};

// 账户信息
struct Account_Info {
    int accountType;  // 0:gk 1:sip
    string account;
    string passWord;
};

// 版本类型
enum class VersionType {
    e_ver_APP,
    e_ver_Hardware
};

// 产品型号
enum class ProductModel {
    e_NT90_MT01,
    e_NT90_MT01M4,
    e_NT90_MT01M8,
    e_NT90_MT01M16,
    e_NT90_MT01M22,
    e_NT90_MB01,
    e_NT90_MB01M4,
    e_NT90_MB01M8,
    e_NT90_MB01M16,
    e_NT90_MB01M22,
    e_NT90_LT01,
    e_NT90_LT01M4,
    e_NT90_LT01M8,
    e_NT90_LT01M16,
    e_NT90_LT01M22,
    e_NT90_MB02,
    e_NT90_MB02M4,
    e_NT90_MB02M8,
    e_NT90_MB02M16,
    e_NT90_MB02M22,
    e_NT90_LT02,
    e_NT90_LT02M4,
    e_NT90_LT02M8,
    e_NT90_LT02M16,
    e_NT90_LT02M22,
    e_NT90_MT04,
    e_NT90_MT04M4,
    e_NT90_MT04M8,
    e_NT90_MT04M16,
    e_NT90_MT04M22,
    e_B500,
    e_B500_4,
    e_B500_8,
    e_B500_16,
    e_B510,
    e_B510_4,
    e_B510_8,
    e_B510_16,
    e_A500,
    e_A500_4,
    e_A500_8,
    e_A500_16,
    e_A510,
    e_A510_4,
    e_A510_8,
    e_A510_16
};

// 芯片类型
enum class ChipType {
    e_Chip_3536 = 0,
    e_Chip_3559 = 1,
    e_Chip_3588 = 2,
    e_Chip_soft = 100,
    e_Chip_soft_completeCodec = 101,
    e_Chip_soft_h265 = 102,
    e_Chip_soft_h265_completeCodec = 103,
    e_Chip_Undefine = 10000
};

// VCS会议类型
enum class VCSConfType {
    e_LiveMeeting,
    e_LiveBroadcast
};

// VCS会议控制模式
enum class VCSConfControlMode {
    e_VCSFreeMode,
    e_VCSChairControlMode,
    e_VCSChairSyncMode,
    e_VCSChairBroadcastMode
};

// VCS会议信息
struct VCSConfInfo {
    string roomAddress;
    string roomAddressIPv6;
    string roomName;
    string startTime;
    string endTime;
    string password;
    int32_t capacity;
    int32_t onlineUsers;
    VCSConfType confType;
    string liveBroadcastAddress;
    ProtoType protoType;
    string extRoomID;
    ConfStatus confStatus;
    string remarks;
    string roomAddress_sip;
    string roomAddressIPv6_sip;
};

// VCS平台
enum class VCSPlatform {
    e_Linux,
    e_Windows,
    e_Mac,
    e_Android,
    e_IOS
};

// VCS成员
struct VCSMember {
    long terminalID;
    string userName;
    bool isJoin;
    bool isFloor;
    uint32_t bitrate[2];
    VCSPlatform platform;
};

// VCS主席
struct VCSFloor {
    long terminalID;
    bool isFloor;
};

// VCS通道类型
enum class VCSChannelType {
    e_SignalChannel,
    e_DataChannel,
    e_VideoTCPChannel,
    e_AudioRTPChannel,
    e_VideoRTPChannel,
    e_RTCPChannel
};

// WiFi接入点参数
struct WifiAccessPointParam {
    string address;
    int quality;
    int maxQuality;
    int signalLevel;
    bool isEncryption;
    string essid;
};

// 辅屏视频类型
enum class AuxScreenVideoType {
    e_LocalMainVideo,
    e_LocalOrRemoteAuxVideo,
    e_AutoSelect
};

// 音频通道
enum class AudioChannel {
    e_LeftChannel,
    e_RightChannel,
    e_Stereo
};

// 重拨条件
enum class RedialCondition {
    e_RedailAllUnexpect,
    e_RedialAllExceptDropByMyself
};

// H264能力信息
struct H264CapabilityInfo {
    int level;
    int maxMBPS;
    int maxFS;
};

// 直播成员信息
struct LiveMemberInfo {
    string roomID;
    string extRoomID;
    string id;
    string name;
    VCSPlatform platform;
    bool isJoin;
};

// 视频自动布局模式
enum class VideoAutoLayoutMode {
    e_NoAutoLayout,
    e_AutoSplitMode,
    e_AutoFillMode
};

// LT视频拼接模式
enum class LTVideoStitchMode {
    e_Stitch_Undefined = -1,
    e_Stitch_HDMI1,
    e_Stitch_HDMI2,
    e_stitch_TL_TR,
    e_stitch_BL_BR,
    e_Stitch_TL_BR,
    e_Stitch_BL_TR,
    e_Stitch_POP,
    e_Stitch_PIP_TL,
    e_Stitch_PIP_TR,
    e_Stitch_PIP_BL,
    e_Stitch_PIP_BR
};

// MP卡信息
struct MPCardInfo {
    uint32_t mpID;
    string mpIP;
    bool HDMIs[MAX_HDMI_COUNT];
    string version;
    ChipType chipType;
};

// TV墙信息
struct TVWallInfo {
    string mpIP;
    string roomID;
    long srcVideoID;
    uint32_t streamID;
    string srcVideoName;
    bool isVCSMember;
    string describe;
};

// 媒体方向
enum class MediaDir {
    e_audioTran,
    e_audioRecv,
    e_videoTran,
    e_videoRecv,
    e_extVideoTran,
    e_extVideoRecv,
    e_max_dir
};

// EP网络信息
struct EPNetworkInfo {
    uint32_t rrt;
    float packetLostRate[4];  // 上行+下行，音频+视频
};

// LDAP认证方法
enum class LDAPAuthMethod {
    e_AuthSimple,
    e_AuthSASL,
    e_AuthKerberos,
    e_NumAuthenticationMethod
};

// LDAP配置
struct LDAPConfig {
    bool m_enable;
    string m_hostname;
    uint32_t m_port;
    string m_user;
    string m_passwd;
    LDAPAuthMethod m_authMethod;
    string m_dc;
};

// LDAP信息
struct LdapInfo {
    string name;
    string commUniqueId;
    string commOwner;
    bool commPrivate;
    string commURI;
    string h323IdentityGKDomain;
    string h323Identityh323ID;
    string h323IdentitydialedDigits;
    string h323IdentityemailID;
    string h323IdentityURLID;
    string h323IdentitytransportID;
    string h323IdentitypartyNumber;
    string h323IdentitymobileUIM;
    string h323IdentityEndpointType;
    string h323IdentityServiceLevel;
    string h235IdentityEndpointID;
    string h235IdentityPassword;
    string sipIdentitySIPURI;
    string sipIdentityRegistrarAddress;
    string sipIdentityProxyAddress;
    string sipIdentityAddress;
    string sipIdentityPassword;
    string sipIdentityUserName;
    string sipIdentityServiceLevel;
};

// 人脸信息
struct faceInfo {
    string faceID;
    string faceName;
    string featurePath;
};

// 通道策略
enum class PassagePolicy {
    e_nonWebRTC = 0,
    e_webRTC_noFEC,
    e_webRTC,
    e_webRTC_StandardRTPPacketization_noFEC,
    e_webRTC_StandardRTPPacketization
};

// 轮询状态
enum class PollingStatus {
    e_PollingStart,
    e_PollingPause,
    e_PollingStop
};

// 轮询信息
struct PollingInfo {
    uint32_t videoPollingInterval;
    vector<long> videoPollingList;
    long videoPollingMember;
    long guidelineMbr;
    time_t lastTime;
    bool bPollingNow;
    bool isManual;
    PollingStatus status;
    int forwardStep;
    bool isSetAllWins;
};

// 音频设备类型
enum class AudioDevType {
    e_LineIn = 0,
    e_LineOut,
    e_Mic,
    e_Hdmi,
    e_LineOut_Hdmi
};

// H235算法类型
enum class H235AlgType {
    e_H235AlgType_Auto = 0,
    e_H235AlgType_sm3,
    e_H235AlgType_sm4_cbc,
    e_H235AlgType_sm4_ecb,
    e_H235AlgType_sm4_ofb,
    e_H235AlgType_sm4_ctr,
    e_H235AlgType_aes,
    e_H235AlgTypeMax
};

// 设备信息
struct DeviceInfo {
    string deviceId;
    long memberID;
    string roomID;
    bool active;
    bool mainVideoActive;
    bool extVideoActive;
    void* info;  // 保留，需要时可替换为vmg::VMGDeviceInfo
};

// 网关EP信息
struct GWEPInfo {
    map<string, DeviceInfo> devices;
    string serverId;
    int index;
};

// 视频源成员
struct VideoSourceMember {
    long terminalID;
    string userName;
    bool isJoin;
    uint32_t channel;
};

// 视频模板
enum class VideoTemplet {
    e_undefined = -1,
    e_1p = 0,
    e_2p,
    e_2p_1,
    e_2p_2,
    e_2p_3,
    e_2p_4,
    e_3p,
    e_3p_1,
    e_3p_2,
    e_4p
};

// AI速度类型
enum class ITCAISpeedType {
    E_ITCAI_SPEED_1_0X,
    E_ITCAI_SPEED_1_5X,
    E_ITCAI_SPEED_2_0X
};

// AI音色类型
enum class ITCAIVoiceType {
    E_ITCAI_VOICE_MALE = 0,
    E_ITCAI_VOICE_FEMALE
};

// 翻译语言类型
enum class TranslateLanguageType {
    E_ITCAI_TRANSLATE_ZH = 0,
    E_ITCAI_TRANSLATE_EN,
    E_ITCAI_TRANSLATE_TH
};

// AI字幕显示类型
enum class ITCAISubtitlesShowType {
    E_ITCAI_SHOW_TRANSCRIPTION_ONLY = 0,
    E_ITCAI_SHOW_TRANSLATION_ONLY,
    E_ITCAI_SHOW_BOTH
};

// 同声传译信息
struct SimultInterpInfo {
    bool SimultInterpEnable;
    bool SubtitlesEnable;
    int SubtitlesShowSwitch;
    int SubtitlesFontsize;
    int SubtitlesFontstyle;
    int SubtitlesFontcolor;
    int SubtitlesBgcolor;
    int SubtitlesBgAlpha;
    TranslateLanguageType languageSrcType;
    TranslateLanguageType languageDstType;
    ITCAISpeedType speedType;
    ITCAIVoiceType voiceType;
};

// 呼叫协议类型
enum class CallProtoType {
    e_autoCall = 0,
    e_callSIP,
    e_callH323
};

// 视频输出接口
enum class VideoOutputInterface {
    e_DVI = 0,
    e_HDMI
};

// 视频输入端口信息
struct VideoInputPortInfo {
    int devID;
    uint32_t width;
    uint32_t height;
    uint32_t framerate;
    bool isAvailable;
};

// 用户名信息（EP用）
struct UserNameInfoEp {
    string name;
    int windowsX;
    int windowsY;
    int windowsWidth;
    int windowsHeight;
    float x_scale;
    float y_scale;
    uint32_t viWidth;
    uint32_t viHeight;
};

// 布局MP框信息（EP用）
struct LayoutMpBoxUseForEp {
    int x;
    int y;
    int width;
    int height;
    float x_scale;
    float y_scale;
};

// 远端布局信息（EP用）
struct RemoteLayoutInfoUseForEp {
    uint32_t layoutId;
    uint32_t winId;
    bool isMixerVideo;
    bool isLocalVideo;
};

// MP资源信息
struct MPResourceInfo {
    string mpID;
    string mpIP;
    bool isBound;
    string roomID;
};

// 视频引擎参数
struct VideoEngineParams {
    CoderType coderType;
    ProtoType protoType;
    VideoProfile profile[MAX_ENC_CHANNELS];
    ITCVideoCodecType vidCodecType[MAX_ENC_CHANNELS];
    uint32_t width[MAX_ENC_CHANNELS];
    uint32_t height[MAX_ENC_CHANNELS];
    uint32_t bitrate[MAX_ENC_CHANNELS];
    uint32_t framerate[MAX_ENC_CHANNELS];
    BitrateControlAlgorithm bitrateCtrlAlg[MAX_ENC_CHANNELS];
    VideoBitrateStrategy bitrateStrategy[MAX_ENC_CHANNELS];
    ITCEncColorFormat colorFormat[MAX_ENC_CHANNELS];
    uint32_t layoutId[MAX_ENC_CHANNELS];
    uint32_t maxSliceSize[MAX_ENC_CHANNELS];
    uint32_t initQP[MAX_ENC_CHANNELS];
    uint32_t minQP[MAX_ENC_CHANNELS];
    uint32_t maxQP[MAX_ENC_CHANNELS];
    uint32_t minIQP[MAX_ENC_CHANNELS];
    uint32_t maxIQP[MAX_ENC_CHANNELS];
    uint32_t keyframeInterval[MAX_ENC_CHANNELS];
    VideoGopMode gopMode[MAX_ENC_CHANNELS];
    ITCOSDParams bannerOSDParams;
    ITCOSDParams rollingTextOSDParams;
    ITCOSDParams userNameOSDParams;
    ITCOSDParams localNameOSDParams;
    ITCOSDParams userLogoOSDParams;
    ITCOSDParams subtitleOSDParams;
    ITCOSDParams NameBadgeParams;
    uint32_t channelCount;
    bool enableDecAntiFlicker;
    bool forceScreenSplit;
};

#endif  // ABSPHONE_DEF_H
