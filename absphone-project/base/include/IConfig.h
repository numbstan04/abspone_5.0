#pragma once

#include "base_export.h"
#include "Response.h"
#include <string>
#include <memory>
#include <vector>

namespace vcs { namespace base {

	// Configuration manager interface
	class VCS_BASE_API IConfig {
		public:
			virtual ~IConfig() = default;

			// 获取配置值
			virtual std::string GetString(const std::string& key, const std::string& defaultValue = "") const = 0;
			virtual int GetInt(const std::string& key, int defaultValue = 0) const = 0;
			virtual bool GetBool(const std::string& key, bool defaultValue = false) const = 0;
			virtual double GetDouble(const std::string& key, double defaultValue = 0.0) const = 0;

			// 设置配置值
			virtual void SetString(const std::string& key, const std::string& value) = 0;
			virtual void SetInt(const std::string& key, int value) = 0;
			virtual void SetBool(const std::string& key, bool value) = 0;
			virtual void SetDouble(const std::string& key, double value) = 0;

			// 加载/保存配置
			virtual Response LoadFromFile(const std::string& filePath) = 0;
			virtual Response SaveToFile(const std::string& filePath) const = 0;

			// 检查键是否存在
			virtual bool HasKey(const std::string& key) const = 0;

			// 获取所有键
			virtual std::vector<std::string> GetKeys() const = 0;

			// =====================================================
			// AbsPhone 扩展接口 - 系统配置
			// =====================================================

			/**
			 * @brief 获取系统版本信息
			 * @param version 输出版本号
			 * @return Response 响应结果
			 */
			virtual Response GetSystemVersion(std::string& version) const = 0;

			/**
			 * @brief 获取产品型号
			 * @param model 输出产品型号
			 * @return Response 响应结果
			 */
			virtual Response GetProductModel(std::string& model) const = 0;

			/**
			 * @brief 获取芯片类型
			 * @param chipType 输出芯片类型标识
			 * @return Response 响应结果
			 */
			virtual Response GetChipType(int& chipType) const = 0;

			/**
			 * @brief 获取最大会议数量
     * @param maxConferences 输出最大会议数
     * @return Response 响应结果
     */
    virtual Response GetMaxConferences(int& maxConferences) const = 0;

    /**
     * @brief 获取最大成员数量
     * @param maxMembers 输出最大成员数
     * @return Response 响应结果
     */
    virtual Response GetMaxMembers(int& maxMembers) const = 0;

    /**
     * @brief 设置系统语言
     * @param language 语言标识
     * @return Response 响应结果
     */
    virtual Response SetSystemLanguage(const std::string& language) = 0;

    /**
     * @brief 获取系统语言
     * @param language 输出语言标识
     * @return Response 响应结果
     */
    virtual Response GetSystemLanguage(std::string& language) const = 0;

    /**
     * @brief 获取系统时区
     * @param timezone 输出时区信息
     * @return Response 响应结果
     */
    virtual Response GetSystemTimezone(std::string& timezone) const = 0;

    /**
     * @brief 设置系统时区
     * @param timezone 时区信息
     * @return Response 响应结果
     */
    virtual Response SetSystemTimezone(const std::string& timezone) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 网络配置
    // =====================================================

    /**
     * @brief 获取本地IP地址
     * @param ipAddress 输出IP地址
     * @return Response 响应结果
     */
    virtual Response GetLocalIPAddress(std::string& ipAddress) const = 0;

    /**
     * @brief 获取子网掩码
     * @param subnetMask 输出子网掩码
     * @return Response 响应结果
     */
    virtual Response GetSubnetMask(std::string& subnetMask) const = 0;

    /**
     * @brief 获取网关地址
     * @param gateway 输出网关地址
     * @return Response 响应结果
     */
    virtual Response GetGatewayAddress(std::string& gateway) const = 0;

    /**
     * @brief 获取DNS服务器地址
     * @param dnsServer 输出DNS服务器地址
     * @return Response 响应结果
     */
    virtual Response GetDNSServer(std::string& dnsServer) const = 0;

    /**
     * @brief 获取MAC地址
     * @param macAddress 输出MAC地址
     * @return Response 响应结果
     */
    virtual Response GetMACAddress(std::string& macAddress) const = 0;

    /**
     * @brief 获取网络类型(DHCP/静态)
     * @param isDhcp 输出是否DHCP
     * @return Response 响应结果
     */
    virtual Response IsDhcpEnabled(bool& isDhcp) const = 0;

    /**
     * @brief 获取MTU值
     * @param mtu 输出MTU值
     * @return Response 响应结果
     */
    virtual Response GetMTU(int& mtu) const = 0;

    /**
     * @brief 设置网络配置
     * @param ipAddress IP地址
     * @param subnetMask 子网掩码
     * @param gateway 网关
     * @param dns DNS服务器
     * @param isDhcp 是否启用DHCP
     * @return Response 响应结果
     */
    virtual Response SetNetworkConfig(const std::string& ipAddress,
                                     const std::string& subnetMask,
                                     const std::string& gateway,
                                     const std::string& dns,
                                     bool isDhcp) = 0;

    /**
     * @brief 获取NAT穿透配置
     * @param enableICE 输出是否启用ICE
     * @param enableSTUN 输出是否启用STUN
     * @param stunServer 输出STUN服务器地址
     * @param enableTURN 输出是否启用TURN
     * @param turnServer 输出TURN服务器地址
     * @return Response 响应结果
     */
    virtual Response GetNatConfig(bool& enableICE, bool& enableSTUN,
                                 std::string& stunServer, bool& enableTURN,
                                 std::string& turnServer) const = 0;

    /**
     * @brief 设置NAT穿透配置
     * @param enableICE 是否启用ICE
     * @param enableSTUN 是否启用STUN
     * @param stunServer STUN服务器地址
     * @param enableTURN 是否启用TURN
     * @param turnServer TURN服务器地址
     * @param turnUsername TURN用户名
     * @param turnPassword TURN密码
     * @return Response 响应结果
     */
    virtual Response SetNatConfig(bool enableICE, bool enableSTUN,
                                  const std::string& stunServer,
                                  bool enableTURN,
                                  const std::string& turnServer,
                                  const std::string& turnUsername,
                                  const std::string& turnPassword) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 视频配置
    // =====================================================

    /**
     * @brief 获取默认视频编码参数
     * @param width 输出视频宽度
     * @param height 输出视频高度
     * @param framerate 输出帧率
     * @param bitrate 输出码率
     * @return Response 响应结果
     */
    virtual Response GetDefaultVideoParams(int& width, int& height,
                                         int& framerate, int& bitrate) const = 0;

    /**
     * @brief 设置默认视频编码参数
     * @param width 视频宽度
     * @param height 视频高度
     * @param framerate 帧率
     * @param bitrate 码率
     * @return Response 响应结果
     */
    virtual Response SetDefaultVideoParams(int width, int height,
                                          int framerate, int bitrate) = 0;

    /**
     * @brief 获取支持的视频分辨率列表
     * @param resolutions 输出支持的分辨率列表(格式: "1920x1080,1280x720,...")
     * @return Response 响应结果
     */
    virtual Response GetSupportedResolutions(std::string& resolutions) const = 0;

    /**
     * @brief 获取视频编码器数量
     * @param encoderCount 输出编码器数量
     * @return Response 响应结果
     */
    virtual Response GetVideoEncoderCount(int& encoderCount) const = 0;

    /**
     * @brief 获取视频输入设备列表
     * @param devices 输出视频输入设备列表(JSON格式)
     * @return Response 响应结果
     */
    virtual Response GetVideoInputDevices(std::string& devices) const = 0;

    /**
     * @brief 获取视频输出设备列表
     * @param devices 输出视频输出设备列表(JSON格式)
     * @return Response 响应结果
     */
    virtual Response GetVideoOutputDevices(std::string& devices) const = 0;

    /**
     * @brief 设置选定的视频输入设备
     * @param deviceId 设备ID
     * @return Response 响应结果
     */
    virtual Response SetVideoInputDevice(const std::string& deviceId) = 0;

    /**
     * @brief 获取选定的视频输入设备
     * @param deviceId 输出设备ID
     * @return Response 响应结果
     */
    virtual Response GetVideoInputDevice(std::string& deviceId) const = 0;

    /**
     * @brief 获取默认视频编码类型
     * @param codecType 输出编码类型(H264/H265)
     * @return Response 响应结果
     */
    virtual Response GetDefaultVideoCodec(std::string& codecType) const = 0;

    /**
     * @brief 设置默认视频编码类型
     * @param codecType 编码类型(H264/H265)
     * @return Response 响应结果
     */
    virtual Response SetDefaultVideoCodec(const std::string& codecType) = 0;

    /**
     * @brief 获取视频关键帧间隔
     * @param interval 输出关键帧间隔(秒)
     * @return Response 响应结果
     */
    virtual Response GetKeyFrameInterval(int& interval) const = 0;

    /**
     * @brief 设置视频关键帧间隔
     * @param interval 关键帧间隔(秒)
     * @return Response 响应结果
     */
    virtual Response SetKeyFrameInterval(int interval) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 音频配置
    // =====================================================

    /**
     * @brief 获取音频输入设备列表
     * @param devices 输出音频输入设备列表(JSON格式)
     * @return Response 响应结果
     */
    virtual Response GetAudioInputDevices(std::string& devices) const = 0;

    /**
     * @brief 获取音频输出设备列表
     * @param devices 输出音频输出设备列表(JSON格式)
     * @return Response 响应结果
     */
    virtual Response GetAudioOutputDevices(std::string& devices) const = 0;

    /**
     * @brief 设置选定的音频输入设备
     * @param deviceId 设备ID
     * @return Response 响应结果
     */
    virtual Response SetAudioInputDevice(const std::string& deviceId) = 0;

    /**
     * @brief 获取选定的音频输入设备
     * @param deviceId 输出设备ID
     * @return Response 响应结果
     */
    virtual Response GetAudioInputDevice(std::string& deviceId) const = 0;

    /**
     * @brief 设置选定的音频输出设备
     * @param deviceId 设备ID
     * @return Response 响应结果
     */
    virtual Response SetAudioOutputDevice(const std::string& deviceId) = 0;

    /**
     * @brief 获取选定的音频输出设备
     * @param deviceId 输出设备ID
     * @return Response 响应结果
     */
    virtual Response GetAudioOutputDevice(std::string& deviceId) const = 0;

    /**
     * @brief 获取麦克风音量
     * @param volume 输出音量值(0-100)
     * @return Response 响应结果
     */
    virtual Response GetMicrophoneVolume(int& volume) const = 0;

    /**
     * @brief 设置麦克风音量
     * @param volume 音量值(0-100)
     * @return Response 响应结果
     */
    virtual Response SetMicrophoneVolume(int volume) = 0;

    /**
     * @brief 获取扬声器音量
     * @param volume 输出音量值(0-100)
     * @return Response 响应结果
     */
    virtual Response GetSpeakerVolume(int& volume) const = 0;

    /**
     * @brief 设置扬声器音量
     * @param volume 音量值(0-100)
     * @return Response 响应结果
     */
    virtual Response SetSpeakerVolume(int volume) = 0;

    /**
     * @brief 获取默认音频编码器
     * @param codec 输出音频编码器
     * @return Response 响应结果
     */
    virtual Response GetDefaultAudioCodec(std::string& codec) const = 0;

    /**
     * @brief 设置默认音频编码器
     * @param codec 音频编码器
     * @return Response 响应结果
     */
    virtual Response SetDefaultAudioCodec(const std::string& codec) = 0;

    /**
     * @brief 获取回声消除状态
     * @param enabled 输出是否启用
     * @return Response 响应结果
     */
    virtual Response IsAecEnabled(bool& enabled) const = 0;

    /**
     * @brief 设置回声消除状态
     * @param enabled 是否启用
     * @return Response 响应结果
     */
    virtual Response SetAecEnabled(bool enabled) = 0;

    /**
     * @brief 获取噪声抑制状态
     * @param enabled 输出是否启用
     * @return Response 响应结果
     */
    virtual Response IsNsEnabled(bool& enabled) const = 0;

    /**
     * @brief 设置噪声抑制状态
     * @param enabled 是否启用
     * @return Response 响应结果
     */
    virtual Response SetNsEnabled(bool enabled) = 0;

    /**
     * @brief 获取自动增益控制状态
     * @param enabled 输出是否启用
     * @return Response 响应结果
     */
    virtual Response IsAgcEnabled(bool& enabled) const = 0;

    /**
     * @brief 设置自动增益控制状态
     * @param enabled 是否启用
     * @return Response 响应结果
     */
    virtual Response SetAgcEnabled(bool enabled) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 会议配置
    // =====================================================

    /**
     * @brief 获取会议默认参数
     * @param maxDuration 输出最大会议时长(分钟)
     * @param maxParticipants 输出最大参会人数
     * @param enableRecording 输出是否启用录制
     * @return Response 响应结果
     */
    virtual Response GetConferenceDefaults(int& maxDuration, int& maxParticipants,
                                          bool& enableRecording) const = 0;

    /**
     * @brief 设置会议默认参数
     * @param maxDuration 最大会议时长(分钟)
     * @param maxParticipants 最大参会人数
     * @param enableRecording 是否启用录制
     * @return Response 响应结果
     */
    virtual Response SetConferenceDefaults(int maxDuration, int maxParticipants,
                                          bool enableRecording) = 0;

    /**
     * @brief 获取会议密码配置
     * @param requireChairPassword 输出是否需要主席密码
     * @param requireGuestPassword 输出是否需要来宾密码
     * @return Response 响应结果
     */
    virtual Response GetConferencePasswordConfig(bool& requireChairPassword,
                                                 bool& requireGuestPassword) const = 0;

    /**
     * @brief 获取默认视频布局模板
     * @param layoutTemplate 输出布局模板名称
     * @return Response 响应结果
     */
    virtual Response GetDefaultLayoutTemplate(std::string& layoutTemplate) const = 0;

    /**
     * @brief 设置默认视频布局模板
     * @param layoutTemplate 布局模板名称
     * @return Response 响应结果
     */
    virtual Response SetDefaultLayoutTemplate(const std::string& layoutTemplate) = 0;

    /**
     * @brief 获取会议录制保存路径
     * @param path 输出保存路径
     * @return Response 响应结果
     */
    virtual Response GetRecordingSavePath(std::string& path) const = 0;

    /**
     * @brief 设置会议录制保存路径
     * @param path 保存路径
     * @return Response 响应结果
     */
    virtual Response SetRecordingSavePath(const std::string& path) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - SIP/H323配置
    // =====================================================

    /**
     * @brief 获取SIP服务器地址
     * @param server 输出SIP服务器地址
     * @return Response 响应结果
     */
    virtual Response GetSipServer(std::string& server) const = 0;

    /**
     * @brief 设置SIP服务器地址
     * @param server SIP服务器地址
     * @return Response 响应结果
     */
    virtual Response SetSipServer(const std::string& server) = 0;

    /**
     * @brief 获取SIP传输协议
     * @param transport 输出传输协议(UDP/TCP/TLS)
     * @return Response 响应结果
     */
    virtual Response GetSipTransport(std::string& transport) const = 0;

    /**
     * @brief 设置SIP传输协议
     * @param transport 传输协议(UDP/TCP/TLS)
     * @return Response 响应结果
     */
    virtual Response SetSipTransport(const std::string& transport) = 0;

    /**
     * @brief 获取SIP端口
     * @param port 输出SIP端口
     * @return Response 响应结果
     */
    virtual Response GetSipPort(int& port) const = 0;

    /**
     * @brief 设置SIP端口
     * @param port SIP端口
     * @return Response 响应结果
     */
    virtual Response SetSipPort(int port) = 0;

    /**
     * @brief 获取H323 GK地址
     * @param gatekeeper 输出GK地址
     * @return Response 响应结果
     */
    virtual Response GetH323Gatekeeper(std::string& gatekeeper) const = 0;

    /**
     * @brief 设置H323 GK地址
     * @param gatekeeper GK地址
     * @return Response 响应结果
     */
    virtual Response SetH323Gatekeeper(const std::string& gatekeeper) = 0;

    /**
     * @brief 获取H323 GK端口
     * @param port 输出GK端口
     * @return Response 响应结果
     */
    virtual Response GetH323GatekeeperPort(int& port) const = 0;

    /**
     * @brief 设置H323 GK端口
     * @param port GK端口
     * @return Response 响应结果
     */
    virtual Response SetH323GatekeeperPort(int port) = 0;

    /**
     * @brief 获取SIP账户信息
     * @param userName 输出用户名
     * @param displayName 输出显示名称
     * @param authName 输出认证名称
     * @return Response 响应结果
     */
    virtual Response GetSipAccount(std::string& userName, std::string& displayName,
                                   std::string& authName) const = 0;

    /**
     * @brief 设置SIP账户信息
     * @param userName 用户名
     * @param displayName 显示名称
     * @param authName 认证名称
     * @param password 密码
     * @return Response 响应结果
     */
    virtual Response SetSipAccount(const std::string& userName,
                                   const std::string& displayName,
                                   const std::string& authName,
                                   const std::string& password) = 0;

    /**
     * @brief 获取SRTP配置
     * @param enable 输出是否启用SRTP
     * @param algorithm 输出加密算法
     * @return Response 响应结果
     */
    virtual Response GetSrtpConfig(bool& enable, std::string& algorithm) const = 0;

    /**
     * @brief 设置SRTP配置
     * @param enable 是否启用SRTP
     * @param algorithm 加密算法
     * @return Response 响应结果
     */
    virtual Response SetSrtpConfig(bool enable, const std::string& algorithm) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 安全配置
    // =====================================================

    /**
     * @brief 获取加密配置
     * @param enableH235 输出是否启用H235加密
     * @param algorithm 输出加密算法
     * @return Response 响应结果
     */
    virtual Response GetEncryptionConfig(bool& enableH235, std::string& algorithm) const = 0;

    /**
     * @brief 设置加密配置
     * @param enableH235 是否启用H235加密
     * @param algorithm 加密算法
     * @return Response 响应结果
     */
    virtual Response SetEncryptionConfig(bool enableH235, const std::string& algorithm) = 0;

    /**
     * @brief 获取防火墙配置
     * @param enabled 输出是否启用
     * @param rules 输出防火墙规则(JSON格式)
     * @return Response 响应结果
     */
    virtual Response GetFirewallConfig(bool& enabled, std::string& rules) const = 0;

    /**
     * @brief 设置防火墙配置
     * @param enabled 是否启用
     * @param rules 防火墙规则(JSON格式)
     * @return Response 响应结果
     */
    virtual Response SetFirewallConfig(bool enabled, const std::string& rules) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 存储配置
    // =====================================================

    /**
     * @brief 获取存储设备信息
     * @param totalSpace 输出总空间(MB)
     * @param freeSpace 输出可用空间(MB)
     * @return Response 响应结果
     */
    virtual Response GetStorageInfo(long long& totalSpace, long long& freeSpace) const = 0;

    /**
     * @brief 获取日志保存路径
     * @param path 输出日志路径
     * @return Response 响应结果
     */
    virtual Response GetLogPath(std::string& path) const = 0;

    /**
     * @brief 设置日志保存路径
     * @param path 日志路径
     * @return Response 响应结果
     */
    virtual Response SetLogPath(const std::string& path) = 0;

    /**
     * @brief 获取日志级别
     * @param level 输出日志级别(0-6: Trace-Debug-Info-Warn-Error-Fatal-Off)
     * @return Response 响应结果
     */
    virtual Response GetLogLevel(int& level) const = 0;

    /**
     * @brief 设置日志级别
     * @param level 日志级别(0-6)
     * @return Response 响应结果
     */
    virtual Response SetLogLevel(int level) = 0;

    /**
     * @brief 获取日志保留天数
     * @param days 输出保留天数
     * @return Response 响应结果
     */
    virtual Response GetLogRetentionDays(int& days) const = 0;

    /**
     * @brief 设置日志保留天数
     * @param days 保留天数
     * @return Response 响应结果
     */
    virtual Response SetLogRetentionDays(int days) = 0;

    /**
     * @brief 获取诊断文件保存路径
     * @param path 输出路径
     * @return Response 响应结果
     */
    virtual Response GetDiagnosticPath(std::string& path) const = 0;

    /**
     * @brief 设置诊断文件保存路径
     * @param path 路径
     * @return Response 响应结果
     */
    virtual Response SetDiagnosticPath(const std::string& path) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 许可证配置
    // =====================================================

    /**
     * @brief 获取许可证信息
     * @param licenseKey 输出许可证密钥
     * @param expireDate 输出过期日期
     * @param isValid 输出是否有效
     * @return Response 响应结果
     */
    virtual Response GetLicenseInfo(std::string& licenseKey, std::string& expireDate,
                                   bool& isValid) const = 0;

    /**
     * @brief 设置许可证
     * @param licenseKey 许可证密钥
     * @return Response 响应结果
     */
    virtual Response SetLicense(const std::string& licenseKey) = 0;

    /**
     * @brief 验证许可证
     * @param isValid 输出是否有效
     * @param message 输出验证消息
     * @return Response 响应结果
     */
    virtual Response ValidateLicense(bool& isValid, std::string& message) const = 0;

    // =====================================================
    // AbsPhone 扩展接口 - 系统维护
    // =====================================================

    /**
     * @brief 获取系统运行时间
     * @param uptime 输出运行时间(秒)
     * @return Response 响应结果
     */
    virtual Response GetSystemUptime(int& uptime) const = 0;

    /**
     * @brief 获取CPU使用率
     * @param usage 输出CPU使用率(百分比)
     * @return Response 响应结果
     */
    virtual Response GetCpuUsage(double& usage) const = 0;

    /**
     * @brief 获取内存使用率
     * @param total 输出总内存(MB)
     * @param used 输出已用内存(MB)
     * @param usage 输出使用率(百分比)
     * @return Response 响应结果
     */
    virtual Response GetMemoryUsage(long long& total, long long& used,
                                   double& usage) const = 0;

    /**
     * @brief 重启系统
     * @return Response 响应结果
     */
    virtual Response RebootSystem() = 0;

    /**
     * @brief 恢复出厂设置
     * @param keepNetwork 是否保留网络配置
     * @param keepLicense 是否保留许可证
     * @return Response 响应结果
     */
    virtual Response FactoryReset(bool keepNetwork, bool keepLicense) = 0;

    /**
     * @brief 导出系统配置
     * @param filePath 输出配置文件路径
     * @return Response 响应结果
     */
    virtual Response ExportConfig(const std::string& filePath) const = 0;

    /**
     * @brief 导入系统配置
     * @param filePath 配置文件路径
     * @return Response 响应结果
     */
    virtual Response ImportConfig(const std::string& filePath) = 0;

    /**
     * @brief 检查系统更新
     * @param updateAvailable 输出是否有可用更新
     * @param version 输出新版本号
     * @param description 输出更新描述
     * @return Response 响应结果
     */
    virtual Response CheckSystemUpdate(bool& updateAvailable, std::string& version,
                                      std::string& description) const = 0;

    /**
     * @brief 执行系统更新
     * @param updatePackagePath 更新包路径
     * @return Response 响应结果
     */
    virtual Response PerformSystemUpdate(const std::string& updatePackagePath) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - LDAP/认证配置
    // =====================================================

    /**
     * @brief 获取LDAP配置
     * @param enabled 输出是否启用
     * @param server 输出服务器地址
     * @param port 输出端口
     * @param baseDN 输出基础DN
     * @return Response 响应结果
     */
    virtual Response GetLdapConfig(bool& enabled, std::string& server,
                                  int& port, std::string& baseDN) const = 0;

    /**
     * @brief 设置LDAP配置
     * @param enabled 是否启用
     * @param server 服务器地址
     * @param port 端口
     * @param baseDN 基础DN
     * @param bindDN 绑定DN
     * @param bindPassword 绑定密码
     * @return Response 响应结果
     */
    virtual Response SetLdapConfig(bool enabled, const std::string& server,
                                  int port, const std::string& baseDN,
                                  const std::string& bindDN,
                                  const std::string& bindPassword) = 0;

    /**
     * @brief 测试LDAP连接
     * @param success 输出是否成功
     * @param message 输出测试消息
     * @return Response 响应结果
     */
    virtual Response TestLdapConnection(bool& success, std::string& message) const = 0;

    /**
     * @brief 获取认证模式
     * @param mode 输出认证模式(local/ldap/radius)
     * @return Response 响应结果
     */
    virtual Response GetAuthMode(std::string& mode) const = 0;

    /**
     * @brief 设置认证模式
     * @param mode 认证模式
     * @return Response 响应结果
     */
    virtual Response SetAuthMode(const std::string& mode) = 0;

    // =====================================================
    // AbsPhone 扩展接口 - QoS配置
    // =====================================================

    /**
     * @brief 获取QoS配置
     * @param enable 输出是否启用QoS
     * @param dscpValue 输出DSCP值
     * @param priority 输出优先级
     * @return Response 响应结果
     */
    virtual Response GetQosConfig(bool& enable, int& dscpValue, int& priority) const = 0;

    /**
     * @brief 设置QoS配置
     * @param enable 是否启用QoS
     * @param dscpValue DSCP值
     * @param priority 优先级
     * @return Response 响应结果
     */
    virtual Response SetQosConfig(bool enable, int dscpValue, int priority) = 0;

    /**
     * @brief 获取带宽配置
     * @param defaultBandwidth 输出默认带宽(Kbps)
     * @param maxBandwidth 输出最大带宽(Kbps)
     * @return Response 响应结果
     */
    virtual Response GetBandwidthConfig(int& defaultBandwidth, int& maxBandwidth) const = 0;

    /**
     * @brief 设置带宽配置
     * @param defaultBandwidth 默认带宽(Kbps)
     * @param maxBandwidth 最大带宽(Kbps)
     * @return Response 响应结果
     */
    virtual Response SetBandwidthConfig(int defaultBandwidth, int maxBandwidth) = 0;
};

} // namespace base
} // namespace vcs
