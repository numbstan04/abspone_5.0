#pragma once

#include "IAbsPhone.h"
#include "absphone_export.h"
#include "IConferenceObserver.h"
#include <memory>
#include <string>

namespace vcs {
namespace absphone {



class VCS_ABSPHONE_API AbsPhoneEngine : public AbsPhone,
                                        public vcs::conference::IConferenceObserver {
public:
    AbsPhoneEngine();
    ~AbsPhoneEngine() override;

    //=======================================================================================================
    //                                         Common APIs                                                  =
    //=======================================================================================================

    ErrCode Initialize(ITCTerminalType terminaltype, AbsPhoneObserver* observer,
                       void* androidJVM = NULL, void* androidObj = NULL) override;
    ErrCode Release() override;
    ITCTerminalType GetTerminalType() override;
    ErrCode SetTerminalType(ITCTerminalType terminaltype) override;
    string GetUsername() override;
    ErrCode SetUsername(string userName) override;
    ErrCode EnableMediaEncryption(bool bEnable, H235AlgType h235AlgType) override;
    bool IsMediaEncryptionEnabled() override;
    ErrCode EnableFEC(bool bEnable) override;
    bool IsFECEnabled() override;
    ErrCode ExecuteCommand(string cmd, string args) override;
    ErrCode EnableRS485(bool enable) override;
    bool IsRS485Enabled() override;
    ErrCode EnableRTPSendPortDetection(bool enable) override;
    bool IsRTPSendPortDetectionEnabled() override;
    ErrCode GetSystemTime(int& second, int& minute, int& hour,
                           int& day, int& month, int& year) override;
    ErrCode SetSystemTime(int second, int minute, int hour,
                           int day, int month, int year) override;
    ErrCode GetNATRouterInfo(bool& bNATEnabled, string& natIP) override;
    ErrCode SetNATRouter(bool bNATEnabled, string natIP) override;
    ErrCode GetPortsInfo(bool& enablePortsSetting,
                          uint32_t& tcpPortBase, uint32_t& tcpPortMax,
                          uint32_t& rtpPortBase, uint32_t& rtpPortMax,
                          uint32_t& sipListenPort) override;
    ErrCode SetPorts(bool enablePortsSetting,
                      uint32_t tcpPortBase, uint32_t tcpPortMax,
                      uint32_t rtpPortBase, uint32_t rtpPortMax,
                      uint32_t sipListenPort) override;
    bool IsCorrectIPV4Address(string strIP) override;
    bool IsConferenceControlEnabled() override;
    ErrCode EnableH245Tunneling(bool bEnable) override;
    bool IsH245TunnelingEnabled() override;
    ErrCode EnableFastStart(bool bEnable) override;
    bool IsFastStartEnabled() override;
    ErrCode GetMaxRTPPayloadSize(uint32_t& size) override;
    ErrCode SetMaxRTPPayloadSize(uint32_t size) override;
    ErrCode SaveConfigs() override;
    string GetGkName() override;
    string GetGkHostName() override;
    ErrCode UpdateGK() override;
    ErrCode SetLogLevel(uint32_t level) override;
    ErrCode GetLogLevel(uint32_t& level) override;
    ErrCode SetLogSize(uint32_t logSize) override;
    ErrCode GetLogSize(uint32_t& logSize) override;
    ErrCode ClearLog() override;
    ErrCode SetGKParameters(bool bEnableGK, string ipAddr, string registrationNumber,
                             string registrationName, string registrationPassword) override;
    ErrCode GetGKParameters(bool& bEnableGK, bool& bGKRegistered,
                             string& ipAddr, string& registrationNumber,
                             string& registrationName, string& registrationPassword,
                             bool& isLocalGK) override;
    ErrCode RemoveGatekeeper() override;
    ErrCode ConfigSetInteger(string key, int32_t value) override;
    int32_t ConfigGetInteger(string key, int32_t defaultValue) override;
    ErrCode ConfigSetString(string key, string value) override;
    string ConfigGetString(string key, string defaultValue) override;
    ErrCode ConfigSetBoolean(string key, bool value) override;
    bool ConfigGetBoolean(string key, bool defaultValue) override;
    ErrCode EnableH264ExtendCapabilityInfo(bool enable) override;
    ErrCode GetH264ExtendCapabilityInfoEnableStatus(bool& enable) override;
    ErrCode EnableVideoJitterBuffer(bool enable) override;
    ErrCode GetVideoJitterBufferStatus(bool& enabled) override;
    ErrCode EnableAudioJitterBuffer(bool enable) override;
    ErrCode GetAudioJitterBufferStatus(bool& enabled) override;
    ErrCode WriteLog(int level, string content) override;
    ErrCode EnableAutoMatchingVideoCapability(bool enable) override;
    ErrCode GetAutoMatchingVideoCapabilityStatus(bool& enabled) override;
    ErrCode GetAudioJitterDelay(list<ItemInfo>& jitterList, uint32_t& curJitterIndex) override;
    ErrCode SetAudioJitterDelay(uint32_t curJitterIndex) override;
    ErrCode EnableSendGRQ(bool enable) override;
    ErrCode GetGRQStatus(bool& enabled) override;

    //=======================================================================================================
    //                                         Endpoint APIs                                               =
    //=======================================================================================================

    /*
    Login VCS server
    @param account: Login account, must be [VCS account / phone number / email]
    @param password: Login password
    @param VCSAddress: VCS server address, must be [ip / ip:port]
    @return: e_ErrNone on success, other value on error
    */
    virtual ErrCode LoginVCS(string account, string password, string VCSAddress, string registerKey = "") ;

    /* Get last VCS loging error description */
    virtual string GetLastVCSLoginErrorDescription() ;

    /*
    Logout VCS server
    @return: e_ErrNone on success, other value on error
    */
    virtual ErrCode LogoutVCS() ;

    /*
    Check if the server has been logged in
    @return: true on logged, false on offline
    */
    virtual bool IsVCSLogged() ;

    /*
    Make call to remote party
    @param remoteParty: The input format must be H323:XXX / SIP:XXX / VCS:XXX
    @return: e_ErrNone on success, other value on error
    */
    virtual ErrCode MakeCall(string remoteParty) ;

    virtual ErrCode AnswerCall(AnswerCallResponse response) ;

    virtual ErrCode InviteVCSMember(string account) ;

    virtual ErrCode ClearCall() ;

    virtual ErrCode OpenH239Channel() ;

    virtual ErrCode CloseH239Channel() ;

    virtual ErrCode GetH239ChannelOpenStatus(bool& isOpen) ;

    virtual ErrCode EnableH239Protocol(bool bEnable) ;

    virtual bool    IsH239ProtocolEnabled() ;

    virtual ErrCode GetForwardCall(ForwardCallMode& mode, string& busyForwardParty) ;

    virtual ErrCode SetForwardCall(ForwardCallMode mode, string busyForwardParty) ;

    virtual ErrCode GetRingSoudFileList(list<ItemInfo>& fileList, uint32_t& curFileIndex) ;

    virtual ErrCode PlayRingSoundFile(uint32_t fileIndex) ;

    virtual ErrCode SetRingSoundFile(uint32_t fileIndex) ;

    virtual ErrCode Beep() ;

    virtual ErrCode EnableBeep(bool bEnable) ;

    virtual bool    IsBeepEnabled() ;

    virtual ErrCode SetAnswerMode(AnswerMode mode, uint32_t maxWaitTime ) ;

    virtual ErrCode GetAnswerMode(AnswerMode& mode, uint32_t& maxWaitTime) ;

    virtual ErrCode EnableOutputCallRecord(bool bEnable) ;

    virtual bool    IsCallRecordCanOutput() ;

    virtual ErrCode EnableAutoAnswerMute(bool bEnable) ;

    virtual bool    IsAutoAnswerMuteEnabled() ;

    virtual ErrCode GetMicrophoneVolume(uint32_t& value) ;

    virtual ErrCode GetSpeakerVolume(uint32_t& value) ;

    virtual ErrCode SetMicrophoneVolume(uint32_t value) ;

    virtual ErrCode SetSpeakerVolume(uint32_t value) ;

    virtual ErrCode StartSpeakerTest() ;

    virtual ErrCode StopSpeakerTest() ;

    virtual ErrCode StartAudioLoopbackTest() ;

    virtual ErrCode StopAudioLoopbackTest() ;

    virtual ErrCode GetAudioRecordDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) ;

    virtual ErrCode SetAudioRecordDevice(uint32_t devIndex) ;

    virtual ErrCode GetAudioPlayoutDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) ;

    virtual ErrCode SetAudioPlayoutDevice(uint32_t devIndex) ;

    virtual ErrCode GetAuxAudioRecordDevices(list<ItemInfo>& deviceList, uint32_t& curDevIndex) ;

    virtual ErrCode SetAuxAudioRecordDevice(uint32_t devIndex) ;

    virtual ErrCode EnableAudioPhasePower(bool bEnable) ;

    virtual bool    IsAudioPhasePowerEnable() ;

    virtual ErrCode GetAudioMixDevices(list<ItemInfo>& deviceList, list<int>& selectedIndex) ;

    virtual ErrCode SetAudioMixDevices(map<int,bool>& deviceMap) ;

    virtual ErrCode GetAudioEncoderParameters(list<ItemInfo>& codecList,
            uint32_t& curCodecIndex,
            bool& bEnableAEC,
            bool& bEnableNS,
            bool& bEnableAGC,
            bool& bEnableVAD,
            bool& bEnableMusicMode,
            bool& bEnableDSPProcess) ;

    virtual ErrCode SetAudioEncoderParameters(uint32_t codecIndex,
            bool bEnableAEC,
            bool bEnableNS,
            bool bEnableAGC,
            bool bEnableVAD,
            bool bEnableMusicMode,
            bool bEnableDSPProcess) ;

    virtual ErrCode GetAudioStreamDelayMs(list<ItemInfo>& delayList, uint32_t& curDelayIndex) ;

    virtual ErrCode SetAudioStreamDelayMs(uint32_t curDelayIndex) ;

    virtual ErrCode GetVideoInputStitchMode(LTVideoStitchMode& stitchMode, bool& bStitchSwap) ;

    virtual ErrCode GetVideoInputStitchMode(bool bMainVideo, VideoTemplet& videoTemplet, uint32_t& devicesNameCount, vector<string>& mixerDevicesName) ;

    virtual ErrCode GetVideoInputDeviceInfo(list<ItemInfo>& portList,
            uint32_t& curPortIndex,
            bool bMainVideo) ;
    virtual ErrCode SetVideoInputDevice(uint32_t portIndex, bool bMainVideo, LTVideoStitchMode stitchMode = LTVideoStitchMode::e_Stitch_Undefined, bool stitchSwap = false) ;
   
    virtual ErrCode SetVideoInputDevice(uint32_t portIndex,
            bool bMainVideo,
            VideoTemplet videoTemplet,
            vector<string> mixerDivicesName) ;

    virtual ErrCode GetVideoInputFramerate(list<ItemInfo>& framerateList, uint32_t& curFramerateIndex, bool bMainVideo) ;

    virtual ErrCode SetVideoInputFramerate(uint32_t framerateIndex, bool bMainVideo) ;

    virtual ErrCode GetPTZControlInfo(list<ItemInfo>& protocolList,
            uint32_t& curProtocolIndex,
            bool& bEnableRemoteControl,
            list<ItemInfo>& deviceList,
            uint32_t& curDeviceIndex,
            list<ItemInfo>& baudrateList,
            uint32_t& curBaudrateIndex,
            list<ItemInfo> &presetList,
            uint32_t& curPresetIndex) ;

    virtual ErrCode SetPTZControl(uint32_t protocolIndex,
            bool bEnableRemoteControl,
            uint32_t deviceIndex,
            uint32_t baudrateIndex,
            uint32_t presetIndex) ;

    virtual ErrCode GetVideoOutputDeviceInfo(list<ItemInfo>& portList,
            uint32_t& curPortIndex,
            list<ItemInfo>& resolutionList,
            uint32_t& curResolutionIndex,
            bool bMainVideo) ;

    virtual ErrCode SetVideoOutputDevice(uint32_t portIndex,
            uint32_t resolutionIndex,
            bool bMainVideo) ;
    
    virtual ErrCode GetVideoOutputColorMode(list<ItemInfo>& colorModeList, uint32_t& curColorModeIndex) ;

    virtual ErrCode SetVideoOutputColorMode(uint32_t colorModeIndex) ;

    virtual ErrCode GetVideoEncoderParameters(list<ItemInfo>& codecList, 
            uint32_t& curCodecIndex, 
            list<ItemInfo>& bitrateList,
            uint32_t& curBitrateIndex,
            list<ItemInfo>& bitrateControlList,
            uint32_t& curBitrateControlIndex,
            list<ItemInfo>& keyframeIntervalList,
            uint32_t& curKeyframeIntervalIndex,
            bool bMainVideo) ;

    virtual ErrCode SetVideoEncoderParameters(uint32_t codecIndex, 
            uint32_t bitrateIndex,
            uint32_t bitrateControlIndex,
            uint32_t keyframeIntervalIndex,
            bool bMainVideo) ;

    virtual ErrCode StartVideoLoopbackTest() ;

    virtual ErrCode StopVideoLoopbackTest() ;

    virtual ErrCode GetImageAdjustingParameters(uint32_t& brightness,  //[0-256]
            uint32_t& contrast,    //[0-256]
            uint32_t& saturation,  //[0-256]
            uint32_t& hue) ;    //[0-256]

    virtual ErrCode SetImageAdjustingParameters(uint32_t controId, uint32_t controlVal) ;

    virtual ErrCode GetLocalNetworkConnectionStatus(string ifName,
            bool& bEnableDHCP, 
            string& ip, 
            string& subnetMask, 
            string& gateway, 
            string& preferedDNS, 
            string& standbyDNS,
            string& macAddress) ;

    virtual ErrCode SetLocalNetworkConnection(string ifName,
            bool bEnableDHCP, 
            string ip, 
            string subnetMask, 
            string gateway, 
            string preferedDNS, 
            string standbyDNS) ;

    virtual ErrCode GetLocalNetworkConnectionStatusForIPV6(string ifName,
            bool& bEnableIpv6,
            bool& bEnableDHCP,
            string& ip,
            string& subnetMask,
            string& gateway,
            string& preferedDNS,
            string& standbyDNS,
            string& macAddress) ;

    virtual ErrCode SetLocalNetworkConnectionForIPV6(string ifName,
            bool bEnableIpv6,
            bool bEnableDHCP,
            string ip,
            string subnetMask,
            string gateway,
            string preferedDNS,
            string standbyDNS) ;

    virtual ErrCode GetQoSInfo(uint32_t& audioDSCP, uint32_t& videoDSCP, uint32_t& dataDSCP) ;

    virtual ErrCode SetQoS(uint32_t audioDSCP, uint32_t videoDSCP, uint32_t dataDSCP) ;

    virtual ErrCode SendPTZCommand(PTZCmd cmd, int32_t param , long terminalID = -1) ;

    virtual ErrCode SetPTZControlDirection(PTZDirection dir) ;

    virtual ErrCode GetPTZControlDirection(PTZDirection& dir) ;

    virtual ErrCode RestoreFactorySettings() ;

    virtual ErrCode GetMediaInformation(MediaInfo& mediaInfo, MediaType mtype) ;

    virtual ErrCode Ping(string destHost,       
            uint32_t& delay,
            string& remoteAddr,      /// Source address of reply packet.
            string& localAddr,       /// Destination address of reply packet.
            PingStatus& status ) ;   /// Status of the last ping operation

    virtual ErrCode MuteAudio(AudioDeviceType type, bool bMute) ;

    virtual ErrCode GetAudioMuteStatus(AudioDeviceType type, bool& bMute) ;

    virtual ErrCode MuteVideo(bool bMute) ;

    virtual bool IsVideoMute() ;

    virtual long GetLocalTerminalID() ;

    virtual long GetChairmanID() ;

    virtual ErrCode RequestChairman(bool bRequest) ;

    virtual ErrCode StartChairmanSync() ;

    virtual ErrCode StopChairmanSync() ;

    virtual ErrCode SendChairmanSyncCmd(LayoutInfo layout, long targetTerminalID ) ;

    virtual ErrCode StartChairmanBroadcast() ;

    virtual ErrCode StopChairmanBroadcast() ;

    virtual ErrCode LockConference() ;

    virtual ErrCode UnlockConference() ;

    virtual ErrCode RequestFloor(bool bRequest) ;

    virtual ErrCode RequestVideoSource(long terminalID, uint32_t streamID, bool bRequest) ;

    virtual ErrCode RequestFloorAssign(long terminalID, bool bRequest) ;

    virtual ErrCode RequestBroadcastTerminal(long terminalID, bool bRequest) ;

    virtual ErrCode RequestBroadcastMultiPicture(bool bRequest) ;

    virtual ErrCode DropTerminal(long terminalID) ;

    virtual ErrCode DropConference() ;

    virtual ErrCode GetTerminalListInfo(list<TerminalInfo>& terminalList) ;

    virtual ErrCode SendConferencePassword(string password) ;

    virtual ErrCode SendUserInput(string cmd) ;

    virtual ErrCode SetVideoLayout(uint32_t layoutID) ;  //[0, 8]

    virtual ErrCode GetVideoLayoutId(uint32_t& layoutID) ;

    virtual ErrCode GetFastCallStatus(bool& bEnabled,  string& callAddress) ;

    virtual ErrCode SetFastCall(bool bEnable, string callAddress) ;

    virtual ErrCode StartFastCall() ;

    virtual ErrCode GetHDDeviceType(HDDeviceType& type) ;

    virtual ErrCode GetConferenceControlMode(ConfControlMode& mode) ;

    virtual bool IsFlowControlEnabled() ;

    virtual ErrCode EnableFlowControl(bool bEnable) ;

    virtual string GetAppVersion(VersionType type = VersionType::e_ver_APP) ;

    virtual ErrCode SetNTPStatus(bool enable, string serverAddr) ;

    virtual ErrCode GetNTPStatus(bool& enabled, string& serverAddr) ;

    virtual ErrCode GetRemoteApplication(string& application) ;

    virtual string GetTimeZone() ;

    virtual ErrCode SetTimeZone(string timeZone) ;

    virtual uint32_t GetCallDuration() ;

    virtual uint32_t GetMiniMCUMaxParticipants() ;

    virtual ErrCode GetProductModel(ProductModel& model) ;

    virtual ErrCode SetProductModel(ProductModel model) ;

    virtual ErrCode GetSystemLanguage(list<ItemInfo>& langList, uint32_t& curLangIndex) ;

    virtual ErrCode SetSystemLanguage(uint32_t langIndex) ;

    virtual ErrCode SendPCMAudioFrame(uint8_t* frameBuf, uint32_t frameSize) ;  //just for ios/Android

    virtual ErrCode SendH264VideoFrame(uint8_t* frameBuf, uint32_t frameSize, uint32_t streamID ) ;  //just for ios/Android

    virtual ErrCode SendH265VideoFrame(uint8_t* frameBuf, uint32_t frameSize, uint32_t streamID ) ;  //just for ios/Android

    virtual ErrCode SendYUV420VideoFrame(uint8_t* frameBuf, uint32_t width, uint32_t height, uint32_t streamID ) ;  //just for Android/Windows

    virtual ErrCode SendRGB24VideoFrame(uint8_t* frameBuf, uint32_t width, uint32_t height, uint32_t streamID ) ;  //just for  Android/Windows

    virtual ErrCode EnableAndroidHardwareDecoder(bool enable) ;  //just for Android

    virtual ErrCode GetAndroidHardwareDecodeStatus(bool& enabled) ;

    virtual ErrCode GetWindowsHardwareCodecStatus(bool& enableEncoder, bool& enableDecoder) ;

    virtual ErrCode EnableWindowsHardwareCodec(bool enableEncoder, bool enableDecoder) ;

    virtual ErrCode SendWhiteboardMessage(uint8_t* msgBuf, uint32_t msgSize) ;

    virtual ErrCode SendDesktopControlMessage(uint8_t* msgBuf, uint32_t msgSize) ;

    virtual ErrCode SendVoteMessage(uint8_t* msgBuf, uint32_t msgSize) ;

    virtual ErrCode SendCollaborativeBrowsingMessage(uint8_t* msgBuf, uint32_t msgSize) ;

    virtual ErrCode ClearWhiteboard(bool eraseBGPicture) ;

    virtual ErrCode SendWhiteboardBackgroundPicture(uint8_t* data, uint32_t size) ;

    virtual ErrCode GetVideoLayoutInfo(LayoutInfo & layout) ;

    virtual ErrCode StartVideoPolling(uint32_t destWindowID, uint32_t timeInterval, list<long> pollingList) ;

    virtual ErrCode StopVideoPolling() ;

    virtual bool IsVideoPollingStarted() ;

    virtual ErrCode GetVideoPollingWindow(uint32_t& windowId) ;

    virtual ErrCode StartRecording() ;

    virtual ErrCode StopRecording() ;

    virtual bool IsRecorderStarted() ;

    virtual bool IsWifiEnable() ;

    virtual ErrCode SetWifiEnable(bool isEnable) ;

    virtual ErrCode ScanWifiAccessPoint(list<WifiAccessPointParam> &wifiAccessPointParam) ;

    virtual ErrCode GetWifiConnectionStatus(string &essid, string &accessPoint) ;

    virtual ErrCode ConnectWifi(string essid, string passwd) ;

    virtual ErrCode DisconnectWifi() ;

    virtual bool Is4GEnable() ;

    virtual ErrCode Set4GEnable(bool isEnable) ;

    virtual ErrCode Get4GSiagnalPower(int &signalPower) ;

    virtual bool IsNetCardExist(string ifName) ;

    virtual ErrCode SetAuxScreenVideo(AuxScreenVideoType videoType) ;

    virtual ErrCode GetAuxScreenVideo(AuxScreenVideoType& videoType) ;

    virtual ErrCode GetTerminalEncoderBitrate(long terminalID, bool bMainVideo, list<uint32_t>& bitrateList, uint32_t& curBitrateIndex) ;

    virtual ErrCode SetTerminalEncoderBitrate(long terminalID, bool bMainVideo, uint32_t curBitrateIndex) ;

    virtual ErrCode SendBannerMessage(string message) ;

    virtual ErrCode SendScrollingMessage(string message) ;

    virtual ErrCode SendShortMessage(string message, long terminalID) ;

    virtual ErrCode EnableExecuteChairmanSyncAction(bool enable) ;

    virtual ErrCode GetExecuteChairmanSyncActionStatus(bool& enable) ;

    virtual ErrCode ExchangeWindow(uint32_t winId1, uint32_t winId2) ;

    virtual ErrCode CaptureOneAuxVideoFrameAsWBPicture() ;
    
    virtual ErrCode CaptureOneAuxI420VideoFrame(uint8_t* buf, uint32_t& width, uint32_t& height, uint32_t& outSize) ;

    virtual ErrCode EnableEliminateMosaic(bool enable) ;

    virtual ErrCode GetEliminateMosaicStatus(bool& enable) ;

    virtual ErrCode GetBootPresetNumber(list<ItemInfo>& presetNumList, uint32_t& curPresetIndex) ;

    virtual ErrCode SetBootPresetNumber(uint32_t presetIndex) ;

    virtual ErrCode EnableSyncPolling(bool enable) ;

    virtual ErrCode GetSyncPollingStatus(bool& enabled) ;

    virtual ErrCode EnableMultiSliceMode(bool enable) ;

    virtual ErrCode GetMultiSliceMode(bool& enabled) ;

    virtual ErrCode SetVADWindow(uint32_t winId) ;

    virtual ErrCode GetVADWindow(uint32_t& winId) ;

    virtual ErrCode StartMonitoringMode() ;

    virtual ErrCode StopMonitoringMode() ;

    virtual ErrCode OpenRTSP(string url, string name) ;

    virtual ErrCode StopRTSP(string url) ;

    virtual ErrCode GetLocalNameOSDParam(ITCOSDParams& params) ;

    virtual ErrCode SetLocalNameOSDParam(ITCOSDParams params) ;

    virtual ErrCode GetConferenceNameOSDParam(ITCOSDParams& params) ;

    virtual ErrCode SetConferenceNameOSDParam(ITCOSDParams params) ;

    virtual ErrCode EnableRunningStatusLED(bool enable) ;

    virtual ErrCode EnableH264SoftDecoding(bool enable) ;

    virtual ErrCode GetH264SoftDecodingStatus(bool& enabled) ;

    virtual ErrCode EnableVideoRTPOverTCPMode(bool enable) ;

    virtual ErrCode GetVideoRTPOverTCPModeStatus(bool& enabled) ;

    virtual ErrCode EnableAudioRTPOverTCPMode(bool enable) ;

    virtual ErrCode GetAudioRTPOverTCPModeStatus(bool& enabled) ;

    virtual ErrCode SetAudioPlayerChannel(AudioChannel channel) ;

    virtual ErrCode GetAudioPlayerChannel(AudioChannel& channel) ;

    virtual ErrCode StartWebRTCVoiceEngine() ;  //Only for Android 

    virtual ErrCode StopWebRTCVoiceEngine() ;  //Only for Android

    virtual ErrCode SetVideoAutoLayoutMode(VideoAutoLayoutMode mode) ;

    virtual ErrCode GetVideoAutoLayoutMode(VideoAutoLayoutMode& mode) ;

    virtual ErrCode EnableVCSOverWriteLocalUsername(bool enable) ;

    virtual ErrCode GetVCSOverWriteLocalUsernameStatus(bool& enabled) ;

    virtual ErrCode GetMicrophoneSignalLevel(uint32_t& level) ;

    virtual ErrCode GetSpeakerSignalLevel(uint32_t& level) ;

    virtual ErrCode GetMicrophoneGain(list<ItemInfo>& gainList, uint32_t& curGainIndex) ;

    virtual ErrCode SetMicrophoneGain(uint32_t curGainIndex) ;

    virtual ErrCode EnableLTMixHdmiAudio(bool enable) ;

    virtual ErrCode GetEnableLTMixHdmiAudio(bool& enable) ;

    virtual ErrCode EableProtocol(ProtoType protoType,bool enable) ;

    virtual ErrCode IsEnableProtocol(ProtoType protoType,bool& enable) ;

    virtual ErrCode PcmVolumeControl(int16_t* pcmSrc, uint32_t pcmSrcByteSize, int16_t* pcmDst, float coefficient) ;

    //add by EF 
    virtual ErrCode ChangeLiveMemberToInteract(string dstLiveMemberId) ;

    virtual ErrCode ChangeMemberToLive(long dstMemberId) ;

    virtual ErrCode AnswerApplyForInteract(ErrCode code, string applyLiveMemberId, bool result) ;

    virtual ErrCode InforMemberChangeToLiveResult(bool result, ErrCode code) ;
    
    virtual ErrCode SetLocalContent(string content) ;
    //end
    //for sip Enpoint
    virtual ErrCode SetSipParams(bool enableSipProtocol, SipAccountInfo accInfo) ;

    virtual ErrCode GetSipParams(bool& enableSipProtocol, vector<SipAccountInfo>& accs) ;  

    virtual ErrCode SipAnswerCall(bool ok) ;

    virtual ErrCode SendSipDTMF(string dtmf) ;
    //end 
    
    virtual ErrCode EnableHDMIHotPlugMode(bool enable) ;

    virtual ErrCode GetHDMIHotPlugStatus(bool& enabled) ;

    virtual ErrCode VCSEnableFullScreenForAuxVideo(bool enable) ;

    virtual ErrCode VCSGetFullScreenModeStatus(bool& enabled) ;

    virtual ErrCode SetRTMPLocalCacheSec(int sec) ;
    
    virtual ErrCode GetRTMPLocalCacheSec(int& sec) ;

    virtual ErrCode OpenRTMP(string rtmpAddress, bool bEncodedVideoCallback) ;

    virtual ErrCode CloseRTMP() ;

    virtual ErrCode LoginRelatedInfoRequest(string url, int brand) ;

    virtual ErrCode InformStopVCSExtvideo(long srcMemberId, long dstMemberId) ;
    
    virtual ErrCode UpdateUIImage(uint8_t* buf, uint32_t size, uint32_t portID ) ;

    virtual bool SetMainStreamMarkEnable(bool bMark) ;

     virtual ErrCode AssignVideoSource(long terminalID, uint32_t streamID, bool bRequest) ;

     virtual ErrCode SetCallProtoType(CallProtoType type) ;

     virtual ErrCode GetCallProtoType(CallProtoType& type) ;

     virtual ErrCode IndicateVerifyPassword(bool isNeedToVerifyPassword) ;

     virtual ErrCode SetOutputInterface(uint32_t streamID, VideoOutputInterface VO) ;

     virtual ErrCode GetOutputInterface(uint32_t streamID, VideoOutputInterface& VO) ;
    
     virtual ErrCode CloseSubSystems() ;

     virtual bool IsMixCapatyEnough(bool& mainOut4k, bool& extOut4k, bool& mainIn4k1, bool&  mainIn4k2,bool& extIn4k) ;
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
                                       string &retMeetingNumber) ;

    virtual ErrCode StopMeeting(ITCHttpParam param, string meetingId, ErrCode &retCode) ;

    virtual ErrCode RemoveMeeting(ITCHttpParam param, string meetingId, ErrCode &retCode) ;

    virtual ErrCode ListMeetingMembers(ITCHttpParam param, string meetingId, ErrCode &retCode, list<ITCMeetingMemberInfo> &membersInfoList) ;

    virtual ErrCode JoinMeeting(ITCHttpParam param, string meetingNumber, string ipAddr, ErrCode &retCode, string &confAddress, string &meetingPasswdMd5) ;

    virtual ErrCode GetConferenceLiveAddress(ITCHttpParam param, 
                                             string meetingID, 
                                             string meetingNumber, 
                                             string ipaddr, 
                                             ErrCode &retCode, 
                                             string &retMeetingId, 
                                             string &retMeetingNumber,
                                             ProtoType &retProtoType,
                                             string &retLiveAddress, 
                                             string &retLivePasswdMd5) ;

    virtual ErrCode PreUploadShareFile(ITCHttpParam param, int userId, int size, ErrCode &retCode) ;

    virtual ErrCode EncodeYUVVideoFrame(uint8_t* buffer, uint32_t width, uint32_t height, uint32_t streamId, ITCYuvType yuvType) ;
      
    virtual ErrCode SetRenderWindow(uint32_t streamId, void* windows, uint32_t width, uint32_t height) ;
   /*=======================================================================================================*/

    /*========================================== Websocket APIs ===========================================*/
    virtual ErrCode WebsocketConnect(string ip, uint16_t port, string path, bool bWSS) ;

    virtual ErrCode WebsocketDisconnect() ;

    virtual bool IsWebsocketConnected() ;

    virtual ErrCode WebsocketSendMsg(string msg) ;

    virtual ErrCode RequestJoinConferenceFromLive(string liveUserId) ;

    virtual ErrCode SendJoinConferenceFromLiveResult(string liveUserId, int result) ;

    virtual ErrCode SendChatMessage(string liveUserId, string content) ;
   /*=======================================================================================================*/
   
    virtual ErrCode EnableWebRTCAudioProcessing(bool enable) ;

    virtual ErrCode GetWebRTCAudioProcessingStatus(bool& enabled) ;

    virtual ErrCode RequestKeyframe(uint32_t streamID) ; 

    //=======================================================================================================
    //                                         内部方法                                                     =
    //=======================================================================================================

    /// 获取观察者
	AbsPhoneObserver* GetObserver() const;

    /// 设置观察者
	void SetObserver(AbsPhoneObserver* observer);

    // vcs::conference::IConferenceObserver - adapted onto AbsPhoneObserver.
    void OnIncomingCall(const std::string& remoteUri, bool hasVideo) override;
    void OnCallConnected(const std::string& remoteUri, bool hasVideo) override;
    void OnCallTerminated(const vcs::conference::CallTerminationInfo& info) override;

private:
    // 内部实现结构
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // 终端类型
    ITCTerminalType terminalType_ = ITCTerminalType::e_ITCEndpoint;
};

} // namespace absphone
} // namespace vcs
