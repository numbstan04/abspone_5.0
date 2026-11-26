#ifndef IABSPHONE_H
#define IABSPHONE_H


#include "absphone_def.h"
#include "absphone_export.h"



namespace vcs {
namespace absphone {

class AbsPhoneObserver;


class VCS_ABSPHONE_API AbsPhone {
public:
    AbsPhone() {}

    virtual ~AbsPhone() {}

    //=======================================================================================================
    //                                                                                                      =
    //                                         Common APIs                                                  =
    //                                          通用接口                                                     =
    //=======================================================================================================

    /// 初始化系统
    /// @param terminaltype 终端类型
    /// @param observer 观察者回调
    /// @param androidJVM Android JVM指针（可选）
    /// @param androidObj Android对象指针（可选）
    virtual ErrCode Initialize(ITCTerminalType terminaltype, AbsPhoneObserver* observer,
                               void* androidJVM = NULL, void* androidObj = NULL) = 0;

    /// 释放系统资源
    virtual ErrCode Release() = 0;

    /// 设置终端类型
    virtual ErrCode SetTerminalType(ITCTerminalType terminaltype) = 0;

    /// 获取终端类型
    virtual ITCTerminalType GetTerminalType() = 0;

    /// 设置用户名
    virtual ErrCode SetUsername(string userName) = 0;

    /// 获取用户名
    virtual string GetUsername() = 0;

    /// 启用媒体加密
    /// @param bEnable 是否启用
    /// @param h235AlgType 加密算法类型
    virtual ErrCode EnableMediaEncryption(bool bEnable, H235AlgType h235AlgType) = 0;

    /// 检查媒体加密是否启用
    virtual bool IsMediaEncryptionEnabled() = 0;

    /// 启用FEC前向纠错
    virtual ErrCode EnableFEC(bool bEnable) = 0;

    /// 检查FEC是否启用
    virtual bool IsFECEnabled() = 0;

    /// 执行命令
    /// @param cmd 命令名称
    /// @param args 命令参数
    virtual ErrCode ExecuteCommand(string cmd, string args) = 0;

    /// 启用RS485
    virtual ErrCode EnableRS485(bool enable) = 0;

    /// 检查RS485是否启用
    virtual bool IsRS485Enabled() = 0;

    /// 启用RTP发送端口检测
    virtual ErrCode EnableRTPSendPortDetection(bool enable) = 0;

    /// 检查RTP发送端口检测是否启用
    virtual bool IsRTPSendPortDetectionEnabled() = 0;

    /// 获取系统时间
    /// @param second 秒 (0-59)
    /// @param minute 分 (0-59)
    /// @param hour 时 (0-23)
    /// @param day 日 (1-31)
    /// @param month 月 (1-12)
    /// @param year 年 (1970-2038)
    virtual ErrCode GetSystemTime(int& second, int& minute, int& hour,
                                   int& day, int& month, int& year) = 0;

    /// 设置系统时间
    virtual ErrCode SetSystemTime(int second, int minute, int hour,
                                   int day, int month, int year) = 0;

    /// 获取NAT路由信息
    /// @param bNATEnabled 是否启用NAT
    /// @param natIP NAT IP地址
    virtual ErrCode GetNATRouterInfo(bool& bNATEnabled, string& natIP) = 0;

    /// 设置NAT路由
    virtual ErrCode SetNATRouter(bool bNATEnabled, string natIP) = 0;

    /// 获取端口信息
    /// @param enablePortsSetting 是否启用端口设置
    /// @param tcpPortBase TCP端口起始
    /// @param tcpPortMax TCP端口最大
    /// @param rtpPortBase RTP端口起始
    /// @param rtpPortMax RTP端口最大
    /// @param sipListenPort SIP监听端口
    virtual ErrCode GetPortsInfo(bool& enablePortsSetting,
                                  uint32_t& tcpPortBase,
                                  uint32_t& tcpPortMax,
                                  uint32_t& rtpPortBase,
                                  uint32_t& rtpPortMax,
                                  uint32_t& sipListenPort) = 0;

    /// 设置端口
    virtual ErrCode SetPorts(bool enablePortsSetting,
                              uint32_t tcpPortBase,
                              uint32_t tcpPortMax,
                              uint32_t rtpPortBase,
                              uint32_t rtpPortMax,
                              uint32_t sipListenPort) = 0;

    /// 检查IPv4地址格式是否正确
    virtual bool IsCorrectIPV4Address(string strIP) = 0;

    /// 检查会议控制是否启用
    virtual bool IsConferenceControlEnabled() = 0;

    /// 启用H245隧道
    virtual ErrCode EnableH245Tunneling(bool bEnable) = 0;

    /// 检查H245隧道是否启用
    virtual bool IsH245TunnelingEnabled() = 0;

    /// 启用快速启动
    virtual ErrCode EnableFastStart(bool bEnable) = 0;

    /// 检查快速启动是否启用
    virtual bool IsFastStartEnabled() = 0;

    /// 获取最大RTP负载大小
    virtual ErrCode GetMaxRTPPayloadSize(uint32_t& size) = 0;

    /// 设置最大RTP负载大小
    virtual ErrCode SetMaxRTPPayloadSize(uint32_t size) = 0;

    /// 保存配置
    virtual ErrCode SaveConfigs() = 0;

    /// 获取GK名称
    virtual string GetGkName() = 0;

    /// 获取GK主机名
    virtual string GetGkHostName() = 0;

    /// 更新GK
    virtual ErrCode UpdateGK() = 0;

    /// 设置日志级别
    /// @param level 0:disabled 1:fatal 2:critical 3:warning 4:info 5:debug
    virtual ErrCode SetLogLevel(uint32_t level) = 0;

    /// 获取日志级别
    virtual ErrCode GetLogLevel(uint32_t& level) = 0;

    /// 设置日志大小
    virtual ErrCode SetLogSize(uint32_t logSize) = 0;

    /// 获取日志大小
    virtual ErrCode GetLogSize(uint32_t& logSize) = 0;

    /// 清除日志
    virtual ErrCode ClearLog() = 0;

    /// 设置GK参数
    /// @param bEnableGK 是否启用GK
    /// @param ipAddr GK IP地址
    /// @param registrationNumber 注册号码
    /// @param registrationName 注册名称
    /// @param registrationPassword 注册密码
    virtual ErrCode SetGKParameters(bool bEnableGK,
                                     string ipAddr,
                                     string registrationNumber,
                                     string registrationName,
                                     string registrationPassword) = 0;

    /// 获取GK参数
    virtual ErrCode GetGKParameters(bool& bEnableGK,
                                     bool& bGKRegistered,
                                     string& ipAddr,
                                     string& registrationNumber,
                                     string& registrationName,
                                     string& registrationPassword,
                                     bool& isLocalGK) = 0;

    /// 删除GK
    virtual ErrCode RemoveGatekeeper() = 0;

    /// 设置整数配置
    virtual ErrCode ConfigSetInteger(string key, int32_t value) = 0;

    /// 获取整数配置
    virtual int32_t ConfigGetInteger(string key, int32_t defaultValue) = 0;

    /// 设置字符串配置
    virtual ErrCode ConfigSetString(string key, string value) = 0;

    /// 获取字符串配置
    virtual string ConfigGetString(string key, string defaultValue) = 0;

    /// 设置布尔配置
    virtual ErrCode ConfigSetBoolean(string key, bool value) = 0;

    /// 获取布尔配置
    virtual bool ConfigGetBoolean(string key, bool defaultValue) = 0;

    /// 启用H264扩展能力信息
    virtual ErrCode EnableH264ExtendCapabilityInfo(bool enable) = 0;

    /// 获取H264扩展能力信息状态
    virtual ErrCode GetH264ExtendCapabilityInfoEnableStatus(bool& enable) = 0;

    /// 启用视频抖动缓冲
    virtual ErrCode EnableVideoJitterBuffer(bool enable) = 0;

    /// 获取视频抖动缓冲状态
    virtual ErrCode GetVideoJitterBufferStatus(bool& enabled) = 0;

    /// 启用音频抖动缓冲
    virtual ErrCode EnableAudioJitterBuffer(bool enable) = 0;

    /// 获取音频抖动缓冲状态
    virtual ErrCode GetAudioJitterBufferStatus(bool& enabled) = 0;

    /// 写日志
    /// @param level 日志级别 0:disabled 1:fatal 2:critical 3:warning 4:info 5:debug
    /// @param content 日志内容
    virtual ErrCode WriteLog(int level, string content) = 0;

    /// 启用自动匹配视频能力
    virtual ErrCode EnableAutoMatchingVideoCapability(bool enable) = 0;

    /// 获取自动匹配视频能力状态
    virtual ErrCode GetAutoMatchingVideoCapabilityStatus(bool& enabled) = 0;

    /// 获取音频抖动延迟列表
    virtual ErrCode GetAudioJitterDelay(list<ItemInfo>& jitterList, uint32_t& curJitterIndex) = 0;

    /// 设置音频抖动延迟
    virtual ErrCode SetAudioJitterDelay(uint32_t curJitterIndex) = 0;

    /// 启用发送GRQ
    virtual ErrCode EnableSendGRQ(bool enable) = 0;

    /// 获取GRQ状态
    virtual ErrCode GetGRQStatus(bool& enabled) = 0;

    //=======================================================================================================
    //                                         Endpoint APIs                                                 =
    //                                         终端接口                                                  =
    //=======================================================================================================

    /*
    Login VCS server
    @param account: Login account, must be [VCS account / phone number / email]
    @param password: Login password
    @param VCSAddress: VCS server address, must be [ip / ip:port]
    @return: e_ErrNone on success, other value on error
    */
    virtual ErrCode LoginVCS(string account, string password, string VCSAddress, string registerKey = "") = 0;

    /* Get last VCS loging error description */
    virtual string GetLastVCSLoginErrorDescription() = 0;

    /*
    Logout VCS server
    @return: e_ErrNone on success, other value on error
    */
    virtual ErrCode LogoutVCS() = 0;

    /*
    Check if the server has been logged in
    @return: true on logged, false on offline
    */
    virtual bool IsVCSLogged() = 0;

    /*
    Make call to remote party
    @param remoteParty: The input format must be H323:XXX / SIP:XXX / VCS:XXX
    @return: e_ErrNone on success, other value on error
    */
    virtual ErrCode MakeCall(string remoteParty) = 0;

    virtual ErrCode AnswerCall(AnswerCallResponse response) = 0;

    virtual ErrCode InviteVCSMember(string account) = 0;

    virtual ErrCode ClearCall() = 0;

    virtual ErrCode OpenH239Channel() = 0;

    virtual ErrCode CloseH239Channel() = 0;

    virtual ErrCode GetH239ChannelOpenStatus(bool& isOpen) = 0;

    virtual ErrCode EnableH239Protocol(bool bEnable) = 0;

    virtual bool    IsH239ProtocolEnabled() = 0;

    virtual ErrCode GetForwardCall(ForwardCallMode& mode, string& busyForwardParty) = 0;

    virtual ErrCode SetForwardCall(ForwardCallMode mode, string busyForwardParty) = 0;

    virtual ErrCode GetRingSoudFileList(list<ItemInfo>& fileList, uint32_t& curFileIndex) = 0;

    virtual ErrCode PlayRingSoundFile(uint32_t fileIndex) = 0;

    virtual ErrCode SetRingSoundFile(uint32_t fileIndex) = 0;

    virtual ErrCode Beep() = 0;

    virtual ErrCode EnableBeep(bool bEnable) = 0;

    virtual bool    IsBeepEnabled() = 0;

    virtual ErrCode SetAnswerMode(AnswerMode mode, uint32_t maxWaitTime = 0) = 0;

    virtual ErrCode GetAnswerMode(AnswerMode& mode, uint32_t& maxWaitTime) = 0;

    virtual ErrCode EnableOutputCallRecord(bool bEnable) = 0;

    virtual bool    IsCallRecordCanOutput() = 0;

    virtual ErrCode EnableAutoAnswerMute(bool bEnable) = 0;

    virtual bool    IsAutoAnswerMuteEnabled() = 0;

    virtual ErrCode GetMicrophoneVolume(uint32_t& value) = 0;

    virtual ErrCode GetSpeakerVolume(uint32_t& value) = 0;

    virtual ErrCode SetMicrophoneVolume(uint32_t value) = 0;

    virtual ErrCode SetSpeakerVolume(uint32_t value) = 0;

    virtual ErrCode StartSpeakerTest() = 0;

    virtual ErrCode StopSpeakerTest() = 0;

    virtual ErrCode StartAudioLoopbackTest() = 0;

    virtual ErrCode StopAudioLoopbackTest() = 0;

    virtual ErrCode GetAudioRecordDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) = 0;

    virtual ErrCode SetAudioRecordDevice(uint32_t devIndex) = 0;

    virtual ErrCode GetAudioPlayoutDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) = 0;

    virtual ErrCode SetAudioPlayoutDevice(uint32_t devIndex) = 0;

    virtual ErrCode GetAuxAudioRecordDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) =0;

    virtual ErrCode SetAuxAudioRecordDevice(uint32_t devIndex) = 0;

    virtual ErrCode EnableAudioPhasePower(bool bEnable) = 0;

    virtual bool    IsAudioPhasePowerEnable() = 0;

    virtual ErrCode GetAudioMixDevices(list<ItemInfo>& deviceList, list<int>& selectedIndex) = 0;

    virtual ErrCode SetAudioMixDevices(map<int,bool>& deviceMap) = 0;

    virtual ErrCode GetAudioEncoderParameters(list<ItemInfo>& codecList,
            uint32_t& curCodecIndex,
            bool& bEnableAEC,
            bool& bEnableNS,
            bool& bEnableAGC,
            bool& bEnableVAD,
            bool& bEnableMusicMode,
            bool& bEnableDSPProcess) = 0;

    virtual ErrCode SetAudioEncoderParameters(uint32_t codecIndex,
            bool bEnableAEC,
            bool bEnableNS,
            bool bEnableAGC,
            bool bEnableVAD,
            bool bEnableMusicMode,
            bool bEnableDSPProcess) = 0;

    virtual ErrCode GetAudioStreamDelayMs(list<ItemInfo>& delayList, uint32_t& curDelayIndex) = 0;

    virtual ErrCode SetAudioStreamDelayMs(uint32_t curDelayIndex) = 0;

    virtual ErrCode GetVideoInputStitchMode(LTVideoStitchMode& stitchMode, bool& bStitchSwap) = 0;

    virtual ErrCode GetVideoInputStitchMode(bool bMainVideo, VideoTemplet& videoTemplet, uint32_t& devicesNameCount, vector<string>& mixerDevicesName) = 0;

    virtual ErrCode GetVideoInputDeviceInfo(list<ItemInfo>& portList,
            uint32_t& curPortIndex,
            bool bMainVideo) = 0;
    virtual ErrCode SetVideoInputDevice(uint32_t portIndex, bool bMainVideo, LTVideoStitchMode stitchMode = LTVideoStitchMode::e_Stitch_Undefined, bool stitchSwap = false) = 0;
   
    virtual ErrCode SetVideoInputDevice(uint32_t portIndex,
            bool bMainVideo,
            VideoTemplet videoTemplet,
            vector<string> mixerDivicesName) =0;

    virtual ErrCode GetVideoInputFramerate(list<ItemInfo>& framerateList, uint32_t& curFramerateIndex, bool bMainVideo) = 0;

    virtual ErrCode SetVideoInputFramerate(uint32_t framerateIndex, bool bMainVideo) = 0;

    virtual ErrCode GetPTZControlInfo(list<ItemInfo>& protocolList,
            uint32_t& curProtocolIndex,
            bool& bEnableRemoteControl,
            list<ItemInfo>& deviceList,
            uint32_t& curDeviceIndex,
            list<ItemInfo>& baudrateList,
            uint32_t& curBaudrateIndex,
            list<ItemInfo> &presetList,
            uint32_t& curPresetIndex) = 0;

    virtual ErrCode SetPTZControl(uint32_t protocolIndex,
            bool bEnableRemoteControl,
            uint32_t deviceIndex,
            uint32_t baudrateIndex,
            uint32_t presetIndex) = 0;

    virtual ErrCode GetVideoOutputDeviceInfo(list<ItemInfo>& portList,
            uint32_t& curPortIndex,
            list<ItemInfo>& resolutionList,
            uint32_t& curResolutionIndex,
            bool bMainVideo) = 0;

    virtual ErrCode SetVideoOutputDevice(uint32_t portIndex,
            uint32_t resolutionIndex,
            bool bMainVideo) = 0;
    
    virtual ErrCode GetVideoOutputColorMode(list<ItemInfo>& colorModeList, uint32_t& curColorModeIndex) = 0;

    virtual ErrCode SetVideoOutputColorMode(uint32_t colorModeIndex) = 0;

    virtual ErrCode GetVideoEncoderParameters(list<ItemInfo>& codecList, 
            uint32_t& curCodecIndex, 
            list<ItemInfo>& bitrateList,
            uint32_t& curBitrateIndex,
            list<ItemInfo>& bitrateControlList,
            uint32_t& curBitrateControlIndex,
            list<ItemInfo>& keyframeIntervalList,
            uint32_t& curKeyframeIntervalIndex,
            bool bMainVideo) = 0;

    virtual ErrCode SetVideoEncoderParameters(uint32_t codecIndex, 
            uint32_t bitrateIndex,
            uint32_t bitrateControlIndex,
            uint32_t keyframeIntervalIndex,
            bool bMainVideo) = 0;

    virtual ErrCode StartVideoLoopbackTest() = 0;

    virtual ErrCode StopVideoLoopbackTest() = 0;

    virtual ErrCode GetImageAdjustingParameters(uint32_t& brightness,  //[0-256]
            uint32_t& contrast,    //[0-256]
            uint32_t& saturation,  //[0-256]
            uint32_t& hue) = 0;    //[0-256]

    virtual ErrCode SetImageAdjustingParameters(uint32_t controId, uint32_t controlVal) = 0;

    virtual ErrCode GetLocalNetworkConnectionStatus(string ifName,
            bool& bEnableDHCP, 
            string& ip, 
            string& subnetMask, 
            string& gateway, 
            string& preferedDNS, 
            string& standbyDNS,
            string& macAddress) = 0;

    virtual ErrCode SetLocalNetworkConnection(string ifName,
            bool bEnableDHCP, 
            string ip, 
            string subnetMask, 
            string gateway, 
            string preferedDNS, 
            string standbyDNS) = 0;

    virtual ErrCode GetLocalNetworkConnectionStatusForIPV6(string ifName,
            bool& bEnableIpv6,
            bool& bEnableDHCP,
            string& ip,
            string& subnetMask,
            string& gateway,
            string& preferedDNS,
            string& standbyDNS,
            string& macAddress) = 0;

    virtual ErrCode SetLocalNetworkConnectionForIPV6(string ifName,
            bool bEnableIpv6,
            bool bEnableDHCP,
            string ip,
            string subnetMask,
            string gateway,
            string preferedDNS,
            string standbyDNS) = 0;

    virtual ErrCode GetQoSInfo(uint32_t& audioDSCP, uint32_t& videoDSCP, uint32_t& dataDSCP) = 0;

    virtual ErrCode SetQoS(uint32_t audioDSCP, uint32_t videoDSCP, uint32_t dataDSCP) = 0;

    virtual ErrCode SendPTZCommand(PTZCmd cmd, int32_t param = 0, long terminalID = -1) = 0;

    virtual ErrCode SetPTZControlDirection(PTZDirection dir) = 0;

    virtual ErrCode GetPTZControlDirection(PTZDirection& dir) = 0;

    virtual ErrCode RestoreFactorySettings() = 0;

    virtual ErrCode GetMediaInformation(MediaInfo& mediaInfo, MediaType mtype) = 0;

    virtual ErrCode Ping(string destHost,       
            uint32_t& delay,
            string& remoteAddr,      /// Source address of reply packet.
            string& localAddr,       /// Destination address of reply packet.
            PingStatus& status ) = 0;   /// Status of the last ping operation

    virtual ErrCode MuteAudio(AudioDeviceType type, bool bMute) = 0;

    virtual ErrCode GetAudioMuteStatus(AudioDeviceType type, bool& bMute) = 0;

    virtual ErrCode MuteVideo(bool bMute) = 0;

    virtual bool IsVideoMute() = 0;

    virtual long GetLocalTerminalID() = 0;

    virtual long GetChairmanID() = 0;

    virtual ErrCode RequestChairman(bool bRequest) = 0;

    virtual ErrCode StartChairmanSync() = 0;

    virtual ErrCode StopChairmanSync() = 0;

    virtual ErrCode SendChairmanSyncCmd(LayoutInfo layout, long targetTerminalID = 0) = 0;

    virtual ErrCode StartChairmanBroadcast() = 0;

    virtual ErrCode StopChairmanBroadcast() = 0;

    virtual ErrCode LockConference() = 0;

    virtual ErrCode UnlockConference() = 0;

    virtual ErrCode RequestFloor(bool bRequest) = 0;

    virtual ErrCode RequestVideoSource(long terminalID, uint32_t streamID, bool bRequest) = 0;

    virtual ErrCode RequestFloorAssign(long terminalID, bool bRequest) = 0;

    virtual ErrCode RequestBroadcastTerminal(long terminalID, bool bRequest) = 0;

    virtual ErrCode RequestBroadcastMultiPicture(bool bRequest) = 0;

    virtual ErrCode DropTerminal(long terminalID) = 0;

    virtual ErrCode DropConference() = 0;

    virtual ErrCode GetTerminalListInfo(list<TerminalInfo>& terminalList) = 0;

    virtual ErrCode SendConferencePassword(string password) = 0;

    virtual ErrCode SendUserInput(string cmd) = 0;

    virtual ErrCode SetVideoLayout(uint32_t layoutID) = 0;  //[0, 8]

    virtual ErrCode GetVideoLayoutId(uint32_t& layoutID) = 0;

    virtual ErrCode GetFastCallStatus(bool& bEnabled,  string& callAddress) = 0;

    virtual ErrCode SetFastCall(bool bEnable, string callAddress) = 0;

    virtual ErrCode StartFastCall() = 0;

    virtual ErrCode GetHDDeviceType(HDDeviceType& type) = 0;

    virtual ErrCode GetConferenceControlMode(ConfControlMode& mode) = 0;

    virtual bool IsFlowControlEnabled() = 0;

    virtual ErrCode EnableFlowControl(bool bEnable) = 0;

    virtual string GetAppVersion(VersionType type = VersionType::e_ver_APP) = 0;

    virtual ErrCode SetNTPStatus(bool enable, string serverAddr) = 0;

    virtual ErrCode GetNTPStatus(bool& enabled, string& serverAddr) = 0;

    virtual ErrCode GetRemoteApplication(string& application) = 0;

    virtual string GetTimeZone() = 0;

    virtual ErrCode SetTimeZone(string timeZone) = 0;

    virtual uint32_t GetCallDuration() = 0;

    virtual uint32_t GetMiniMCUMaxParticipants() = 0;

    virtual ErrCode GetProductModel(ProductModel& model) = 0;

    virtual ErrCode SetProductModel(ProductModel model) = 0;

    virtual ErrCode GetSystemLanguage(list<ItemInfo>& langList, uint32_t& curLangIndex) = 0;

    virtual ErrCode SetSystemLanguage(uint32_t langIndex) = 0;

    virtual ErrCode SendPCMAudioFrame(uint8_t* frameBuf, uint32_t frameSize) = 0;  //just for ios/Android

    virtual ErrCode SendH264VideoFrame(uint8_t* frameBuf, uint32_t frameSize, uint32_t streamID = 0) = 0;  //just for ios/Android

    virtual ErrCode SendH265VideoFrame(uint8_t* frameBuf, uint32_t frameSize, uint32_t streamID = 0) = 0;  //just for ios/Android

    virtual ErrCode SendYUV420VideoFrame(uint8_t* frameBuf, uint32_t width, uint32_t height, uint32_t streamID = 0) = 0;  //just for Android/Windows

    virtual ErrCode SendRGB24VideoFrame(uint8_t* frameBuf, uint32_t width, uint32_t height, uint32_t streamID = 0) = 0;  //just for  Android/Windows

    virtual ErrCode EnableAndroidHardwareDecoder(bool enable) = 0;  //just for Android

    virtual ErrCode GetAndroidHardwareDecodeStatus(bool& enabled) = 0;

    virtual ErrCode GetWindowsHardwareCodecStatus(bool& enableEncoder, bool& enableDecoder) = 0;

    virtual ErrCode EnableWindowsHardwareCodec(bool enableEncoder, bool enableDecoder) = 0;

    virtual ErrCode SendWhiteboardMessage(uint8_t* msgBuf, uint32_t msgSize) = 0;

    virtual ErrCode SendDesktopControlMessage(uint8_t* msgBuf, uint32_t msgSize) = 0;

    virtual ErrCode SendVoteMessage(uint8_t* msgBuf, uint32_t msgSize) = 0;

    virtual ErrCode SendCollaborativeBrowsingMessage(uint8_t* msgBuf, uint32_t msgSize) = 0;

    virtual ErrCode ClearWhiteboard(bool eraseBGPicture) = 0;

    virtual ErrCode SendWhiteboardBackgroundPicture(uint8_t* data, uint32_t size) = 0;

    virtual ErrCode GetVideoLayoutInfo(LayoutInfo & layout) = 0;

    virtual ErrCode StartVideoPolling(uint32_t destWindowID, uint32_t timeInterval, list<long> pollingList) = 0;

    virtual ErrCode StopVideoPolling() = 0;

    virtual bool IsVideoPollingStarted() = 0;

    virtual ErrCode GetVideoPollingWindow(uint32_t& windowId) = 0;

    virtual ErrCode StartRecording() = 0;

    virtual ErrCode StopRecording() = 0;

    virtual bool IsRecorderStarted() = 0;

    virtual bool IsWifiEnable() = 0;

    virtual ErrCode SetWifiEnable(bool isEnable) = 0;

    virtual ErrCode ScanWifiAccessPoint(list<WifiAccessPointParam> &wifiAccessPointParam) = 0;

    virtual ErrCode GetWifiConnectionStatus(string &essid, string &accessPoint) = 0;

    virtual ErrCode ConnectWifi(string essid, string passwd) = 0;

    virtual ErrCode DisconnectWifi() = 0;

    virtual bool Is4GEnable() = 0;

    virtual ErrCode Set4GEnable(bool isEnable) = 0;

    virtual ErrCode Get4GSiagnalPower(int &signalPower) = 0;

    virtual bool IsNetCardExist(string ifName) = 0;

    virtual ErrCode SetAuxScreenVideo(AuxScreenVideoType videoType) = 0;

    virtual ErrCode GetAuxScreenVideo(AuxScreenVideoType& videoType) = 0;

    virtual ErrCode GetTerminalEncoderBitrate(long terminalID, bool bMainVideo, list<uint32_t>& bitrateList, uint32_t& curBitrateIndex) = 0;

    virtual ErrCode SetTerminalEncoderBitrate(long terminalID, bool bMainVideo, uint32_t curBitrateIndex) = 0;

    virtual ErrCode SendBannerMessage(string message) = 0;

    virtual ErrCode SendScrollingMessage(string message) = 0;

    virtual ErrCode SendShortMessage(string message, long terminalID) = 0;

    virtual ErrCode EnableExecuteChairmanSyncAction(bool enable) = 0;

    virtual ErrCode GetExecuteChairmanSyncActionStatus(bool& enable) = 0;

    virtual ErrCode ExchangeWindow(uint32_t winId1, uint32_t winId2) = 0;

    virtual ErrCode CaptureOneAuxVideoFrameAsWBPicture() = 0;
    
    virtual ErrCode CaptureOneAuxI420VideoFrame(uint8_t* buf, uint32_t& width, uint32_t& height, uint32_t& outSize) = 0;

    virtual ErrCode EnableEliminateMosaic(bool enable) = 0;

    virtual ErrCode GetEliminateMosaicStatus(bool& enable) = 0;

    virtual ErrCode GetBootPresetNumber(list<ItemInfo>& presetNumList, uint32_t& curPresetIndex) = 0;

    virtual ErrCode SetBootPresetNumber(uint32_t presetIndex) = 0;

    virtual ErrCode EnableSyncPolling(bool enable) = 0;

    virtual ErrCode GetSyncPollingStatus(bool& enabled) = 0;

    virtual ErrCode EnableMultiSliceMode(bool enable) = 0;

    virtual ErrCode GetMultiSliceMode(bool& enabled) = 0;

    virtual ErrCode SetVADWindow(uint32_t winId) = 0;

    virtual ErrCode GetVADWindow(uint32_t& winId) = 0;

    virtual ErrCode StartMonitoringMode() = 0;

    virtual ErrCode StopMonitoringMode() = 0;

    virtual ErrCode OpenRTSP(string url, string name) = 0;

    virtual ErrCode StopRTSP(string url) = 0;

    virtual ErrCode GetLocalNameOSDParam(ITCOSDParams& params) = 0;

    virtual ErrCode SetLocalNameOSDParam(ITCOSDParams params) = 0;

    virtual ErrCode GetConferenceNameOSDParam(ITCOSDParams& params) = 0;

    virtual ErrCode SetConferenceNameOSDParam(ITCOSDParams params) = 0;

    virtual ErrCode EnableRunningStatusLED(bool enable) = 0;

    virtual ErrCode EnableH264SoftDecoding(bool enable) = 0;

    virtual ErrCode GetH264SoftDecodingStatus(bool& enabled) = 0;

    virtual ErrCode EnableVideoRTPOverTCPMode(bool enable) = 0;

    virtual ErrCode GetVideoRTPOverTCPModeStatus(bool& enabled) = 0;

    virtual ErrCode EnableAudioRTPOverTCPMode(bool enable) = 0;

    virtual ErrCode GetAudioRTPOverTCPModeStatus(bool& enabled) = 0;

    virtual ErrCode SetAudioPlayerChannel(AudioChannel channel) = 0;

    virtual ErrCode GetAudioPlayerChannel(AudioChannel& channel) = 0;

    virtual ErrCode StartWebRTCVoiceEngine() = 0;  //Only for Android 

    virtual ErrCode StopWebRTCVoiceEngine() = 0;  //Only for Android

    virtual ErrCode SetVideoAutoLayoutMode(VideoAutoLayoutMode mode) = 0;

    virtual ErrCode GetVideoAutoLayoutMode(VideoAutoLayoutMode& mode) = 0;

    virtual ErrCode EnableVCSOverWriteLocalUsername(bool enable) = 0;

    virtual ErrCode GetVCSOverWriteLocalUsernameStatus(bool& enabled) = 0;

    virtual ErrCode GetMicrophoneSignalLevel(uint32_t& level) = 0;

    virtual ErrCode GetSpeakerSignalLevel(uint32_t& level) = 0;

    virtual ErrCode GetMicrophoneGain(list<ItemInfo>& gainList, uint32_t& curGainIndex) = 0;

    virtual ErrCode SetMicrophoneGain(uint32_t curGainIndex) = 0;

    virtual ErrCode EnableLTMixHdmiAudio(bool enable) = 0;

    virtual ErrCode GetEnableLTMixHdmiAudio(bool& enable) = 0;

    virtual ErrCode EableProtocol(ProtoType protoType,bool enable) = 0;

    virtual ErrCode IsEnableProtocol(ProtoType protoType,bool& enable) = 0;

    virtual ErrCode PcmVolumeControl(int16_t* pcmSrc, uint32_t pcmSrcByteSize, int16_t* pcmDst, float coefficient) = 0;

    //add by EF 
    virtual ErrCode ChangeLiveMemberToInteract(string dstLiveMemberId) =0;

    virtual ErrCode ChangeMemberToLive(long dstMemberId) = 0;

    virtual ErrCode AnswerApplyForInteract(ErrCode code, string applyLiveMemberId, bool result) = 0;

    virtual ErrCode InforMemberChangeToLiveResult(bool result, ErrCode code) = 0;
    
    virtual ErrCode SetLocalContent(string content) = 0;
    //end
    //for sip Enpoint
    virtual ErrCode SetSipParams(bool enableSipProtocol, SipAccountInfo accInfo) = 0;

    virtual ErrCode GetSipParams(bool& enableSipProtocol, vector<SipAccountInfo>& accs) = 0;  

    virtual ErrCode SipAnswerCall(bool ok) = 0;

    virtual ErrCode SendSipDTMF(string dtmf) = 0;
    //end 
    
    virtual ErrCode EnableHDMIHotPlugMode(bool enable) = 0;

    virtual ErrCode GetHDMIHotPlugStatus(bool& enabled) = 0;

    virtual ErrCode VCSEnableFullScreenForAuxVideo(bool enable) = 0;

    virtual ErrCode VCSGetFullScreenModeStatus(bool& enabled) = 0;

    virtual ErrCode SetRTMPLocalCacheSec(int sec) = 0;
    
    virtual ErrCode GetRTMPLocalCacheSec(int& sec) = 0;

    virtual ErrCode OpenRTMP(string rtmpAddress, bool bEncodedVideoCallback) = 0;

    virtual ErrCode CloseRTMP() = 0;

    virtual ErrCode LoginRelatedInfoRequest(string url, int brand) = 0;

    virtual ErrCode InformStopVCSExtvideo(long srcMemberId, long dstMemberId) = 0;
    
    virtual ErrCode UpdateUIImage(uint8_t* buf, uint32_t size, uint32_t portID = 0) = 0;

    virtual bool SetMainStreamMarkEnable(bool bMark) = 0;

     virtual ErrCode AssignVideoSource(long terminalID, uint32_t streamID, bool bRequest) = 0;

     virtual ErrCode SetCallProtoType(CallProtoType type) = 0;

     virtual ErrCode GetCallProtoType(CallProtoType& type) = 0;

     virtual ErrCode IndicateVerifyPassword(bool isNeedToVerifyPassword) = 0;

     virtual ErrCode SetOutputInterface(uint32_t streamID, VideoOutputInterface VO) =0;

     virtual ErrCode GetOutputInterface(uint32_t streamID, VideoOutputInterface& VO) = 0;
    
     virtual ErrCode CloseSubSystems() = 0;

     virtual bool IsMixCapatyEnough(bool& mainOut4k, bool& extOut4k, bool& mainIn4k1, bool&  mainIn4k2,bool& extIn4k) = 0;
     /*========================================== HTTP APIs (Blocking APIs)==================================*/
    virtual ErrCode CreateQuickMeeting(ITCHttpParam param,         // not null, url info
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
                                       string &retMeetingNumber) = 0;

    virtual ErrCode StopMeeting(ITCHttpParam param, string meetingId, ErrCode &retCode) = 0;

    virtual ErrCode RemoveMeeting(ITCHttpParam param, string meetingId, ErrCode &retCode) = 0;

    virtual ErrCode ListMeetingMembers(ITCHttpParam param, string meetingId, ErrCode &retCode, list<ITCMeetingMemberInfo> &membersInfoList) = 0;

    virtual ErrCode JoinMeeting(ITCHttpParam param, string meetingNumber, string ipAddr, ErrCode &retCode, string &confAddress, string &meetingPasswdMd5) = 0;

    virtual ErrCode GetConferenceLiveAddress(ITCHttpParam param, 
                                             string meetingID, 
                                             string meetingNumber, 
                                             string ipaddr, 
                                             ErrCode &retCode, 
                                             string &retMeetingId, 
                                             string &retMeetingNumber,
                                             ProtoType &retProtoType,
                                             string &retLiveAddress, 
                                             string &retLivePasswdMd5) = 0;

    virtual ErrCode PreUploadShareFile(ITCHttpParam param, int userId, int size, ErrCode &retCode) = 0;

    virtual ErrCode EncodeYUVVideoFrame(uint8_t* buffer, uint32_t width, uint32_t height, uint32_t streamId, ITCYuvType yuvType) = 0;
      
    virtual ErrCode SetRenderWindow(uint32_t streamId, void* windows, uint32_t width, uint32_t height) = 0;
   /*=======================================================================================================*/

    /*========================================== Websocket APIs ===========================================*/
    virtual ErrCode WebsocketConnect(string ip, uint16_t port, string path, bool bWSS) = 0;

    virtual ErrCode WebsocketDisconnect() = 0;

    virtual bool IsWebsocketConnected() = 0;

    virtual ErrCode WebsocketSendMsg(string msg) = 0;

    virtual ErrCode RequestJoinConferenceFromLive(string liveUserId) = 0;

    virtual ErrCode SendJoinConferenceFromLiveResult(string liveUserId, int result) = 0;

    virtual ErrCode SendChatMessage(string liveUserId, string content) = 0;
   /*=======================================================================================================*/
   
    virtual ErrCode EnableWebRTCAudioProcessing(bool enable) = 0;

    virtual ErrCode GetWebRTCAudioProcessingStatus(bool& enabled) = 0;

    virtual ErrCode RequestKeyframe(uint32_t streamID) = 0;   
    
};

//--------------------------------------------------------------------------------------------------------------------
// 工厂函数 - 创建 AbsPhone 实例
// 外部代码通过此函数创建引擎，隐藏实现类细节
//--------------------------------------------------------------------------------------------------------------------


VCS_ABSPHONE_API AbsPhone* CreateAbsPhone();
VCS_ABSPHONE_API void DestroyAbsPhone(AbsPhone* phone);


class VCS_ABSPHONE_API AbsPhoneObserver {
public:
    AbsPhoneObserver() {}

    virtual ~AbsPhoneObserver() {}

    //--------------------------------------------------------------------------------------------------------------------
    //                                               Common callbacks
    //                                               通用回调
    //--------------------------------------------------------------------------------------------------------------------

    /// 获取端点参数回调
    /// @return true 允许获取参数, false 拒绝
    virtual bool OnGetEndpointParam(string keyUsername,
                                     string keyHostname,
                                     string param,
                                     string& value) = 0;

    //--------------------------------------------------------------------------------------------------------------------
    //                                               Endpoint callbacks
    //                                               终端回调
    //--------------------------------------------------------------------------------------------------------------------

    virtual void OnAnswerCall(string caller) = 0;

    virtual void OnConnectionEstablished(string remoteParty, 
            string remoteAddress,
            ProtoType proto,
            bool hasVideo) = 0;

    virtual void OnConnectionCleared(ErrCode endedReason,
            ProtoType protoType,
            string remoteParty,
            string remoteAddress,
            uint32_t duration) = 0;
			
	virtual void OnSetCodecName(uint32_t streamID, string codecName, bool isSend) = 0;

    virtual void OnAddNewCallRecord(CallDirection dir,
            string remoteParty,
            string remoteAddress,
            string E164Number,
            string startTime,
            uint32_t duration,
            CallResult result) = 0;

    virtual void OnUpdateConferenceList(list<VCSConfInfo> confList) = 0;

    virtual void OnUpdateTerminalList(list<VCSMember> terminalList) = 0;

    virtual void OnUpdateFloorList(list<VCSFloor> floorList) = 0;

    virtual void OnUpdateConferenceChairmanID(long chairmanID) = 0;

    virtual void OnAnswerVCSCall(string caller, string roomAddress, string roomAddress_sip) = 0;

    virtual void OnReceivedWhiteboardCmd(uint8_t* data, uint32_t size) = 0;

    virtual void OnReceivedWhiteboardBGPicture(uint8_t* data, uint32_t size) = 0;

    virtual void OnReceivedDesktopControlActionCmd(uint8_t* data, uint32_t size) = 0;

    virtual void OnChangeVCSConfControlMode(VCSConfControlMode mode) = 0;

    virtual void OnReceivedVCSLoginResponse(string VCSAccount, 
                                            string VCSUsername, 
                                            string userHeadPictureURL,
                                            string enterpriseNumber) = 0;

    virtual void OnReceivedVCSConferenceParameters(string ftpAddress, 
                                                   string ftpAccount, 
                                                   string ftpPassword,
                                                   string extRoomID,
                                                   long  localTerminalID,
                                                   string chairmanPassword,
                                                   bool manuallySendH239,
                                                   uint32_t defaulVideoLayoutID,
                                                   uint32_t maxVideoWindows) = 0;

    virtual void OnReceivedVoteCmd(uint8_t* data, uint32_t size) = 0;

    virtual void OnReceivedCollaborativeBrowsingCmd(uint8_t* data, uint32_t size) = 0;

    virtual void OnClearWhiteboard(bool eraseBGPicture) = 0;

    virtual void OnReceivedChairmanSyncCmd(LayoutInfo layout) = 0;

    virtual void OnReceivedSystemMsg(string systemMsg) = 0;

    virtual void OnSipAnswerCall(string caller) = 0;

    virtual void OnSetEncoderProfile(VideoProfile profile, uint32_t streamID) = 0;

    virtual bool OnGetYUVFrame(uint32_t streamID, uint8_t* buf, uint32_t width, uint32_t height) = 0;

    virtual void OnRenderYUVFrame(long memberID, uint32_t streamID, uint32_t width, uint32_t height, const uint8_t* frameBuf, uint32_t frameSize) = 0;

    virtual void OnStartReceiveH239Video(long sharerID = -1) = 0;

    virtual void OnStopReceiveH239Video() = 0;

    //The packet loss rate (0.0 to 1.0), only for main video
    virtual void OnPacketLossRateUpdate(float lossRate) = 0;

    virtual bool OnOpenAudioCaptureDevice(uint32_t samplerate, uint32_t channels) = 0;

    virtual bool OnReadAudioFrame(uint8_t* buf, uint32_t bufSize) = 0;

    virtual void OnCloseAudioCaptureDevice() = 0;

    virtual bool OnOpenAudioPlayerDevice(uint32_t samplerate, uint32_t channels) = 0;

    virtual bool OnWriteAudioFrame(uint8_t* buf, uint32_t bufSize) = 0;

    virtual void OnCloseAudioPlayerDevice() = 0;

    virtual void OnInitPTZControl(string protocol,
            uint32_t speed,
            uint32_t baudrate) = 0;

    virtual void OnSetPTZParameters(string protocol,
            uint32_t speed,
            uint32_t baudrate) = 0;

    virtual void OnExecutePTZCommand(uint32_t command) = 0;

    virtual void OnRtmpConnected() = 0;

    virtual void OnRtmpDisconnected() = 0;

    virtual void OnGetRtmpPlayerAudioData(uint8_t* audioData, uint32_t audioSize, uint32_t samplerate, uint32_t channels, uint32_t timestamp) = 0;

    virtual void OnGetRtmpPlayerVideoData(uint8_t* videoData, uint32_t videoSize, uint32_t width, uint32_t height, uint32_t timestamp, ITCVideoCodecType vt) = 0;

    virtual void OnGetLoginRelatedInfo(int serverVersion, bool https, uint16_t port, bool phonePushEnable, string appKey) = 0;

    virtual void OnWebsocketConnected(ErrCode result, string id, string ip, uint16_t port, string path, bool bWSS) = 0;

    virtual void OnWebsocketDisconnected(ITCWSDisconnectType closeType, string errDescription) = 0;

    virtual void OnWebsocketReceivedData(string json) = 0;

    virtual void OnWebsocketReplyJoinConfereceResult(ErrCode result) = 0;

    virtual void OnWebsocketAcceptLiveToConference(ProtoType type, string confNumber) = 0;

    virtual void OnWebsocketRejectLiveToConference(ErrCode result) = 0;

    virtual void OnWebsocketQuitLive() = 0;

    virtual void OnWebsocketGetChatMessage(string liveUserId, string name, string time, string content) = 0;

    virtual void OnForceKeyframe(uint32_t streamID) = 0;

    virtual void OnInfraredRemoteControlCallback(uint8_t* data, uint32_t size) = 0;

    virtual void OnReceivedH323ConferenceParameters(string roomID,string extRoomID, long memberID) = 0;

    virtual void OnSipBfcpAuxVideoStatusChange(bool bEnable) = 0;

    virtual void OnStartExtVideo() = 0;

    virtual void OnStopExtVideo() = 0;

    virtual void OnInformStopVCSExtVideoResp(bool result) = 0;
    
    virtual void OnInformEPBePolled(bool isSpeaker, bool isWatching) =0 ;
    
    virtual void OnAudioControlCMD(int cmd, string content) =0;

    virtual void OnReceivedStandarConfVideoSrcInd(long memberID, uint32_t channel) = 0;

    virtual void OnUpdateVideoSrcList(list<VideoSourceMember> mbrList) = 0;
    
};

} // namespace absphone
} // namespace vcs

#endif  // IABSPHONE_H
