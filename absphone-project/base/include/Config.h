#pragma once

#include "base_export.h"
#include "IConfig.h"
#include "Response.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include <mutex>
#include <cstdint>

namespace vcs { namespace base {

// 配置管理器实现
class VCS_BASE_API Config : public IConfig {
public:
    Config() = default;

    // 获取配置值
    std::string GetString(const std::string& key, const std::string& defaultValue = "") const override;
    int GetInt(const std::string& key, int defaultValue = 0) const override;
    bool GetBool(const std::string& key, bool defaultValue = false) const override;
    double GetDouble(const std::string& key, double defaultValue = 0.0) const override;

    // 设置配置值
    void SetString(const std::string& key, const std::string& value) override;
    void SetInt(const std::string& key, int value) override;
    void SetBool(const std::string& key, bool value) override;
    void SetDouble(const std::string& key, double value) override;

    // 加载/保存配置
    Response LoadFromFile(const std::string& filePath) override;
    Response SaveToFile(const std::string& filePath) const override;

    // 检查键是否存在
    bool HasKey(const std::string& key) const override;

    // 获取所有键
    std::vector<std::string> GetKeys() const override;

    // ============================================================
    // 旧键名别名（docs/10 §3.1.7/§3.1.8）
    // 规范键只在内存中作为视图；读写都落到旧文件的真实键上。
    // ============================================================
    enum class AliasTransform {
        None,            // 原值直通
        InvertBool,      // 文件里是 "Disable X"，规范侧取反
        SrtpUse,         // 文件三态 0/1/2，规范侧 bool（写 true 不得把 2 降级）
        SrtpAlg,         // 文件整数 0..8，规范侧算法名
        GatekeeperMode   // 文件 "Use gatekeeper"/"No gatekeeper"，规范侧 bool
    };

    struct ConfigKeyAlias {
        std::string canonical;   // 代码使用的规范键
        std::string section;     // 旧文件节区（原大小写）
        std::string legacyKey;   // 旧文件键（原大小写）
        AliasTransform transform = AliasTransform::None;
    };

    // 必须在 LoadFromFile 之前注入；加载时会校验别名目标是否存在。
    void SetKeyAliases(const std::vector<ConfigKeyAlias>& aliases);

    // 取走加载期警告（缺失别名目标、行解析失败、文件缺失等）。组合根在
    // 日志初始化之后调用并统一输出，保证 LogLevel=Off 时零输出、且不写入
    // 工作目录下的默认日志文件（docs/10 §3.2.10）。
    std::vector<std::string> TakeLoadWarnings();

    // ============================================================
    // 【AbsPhone扩展】系统配置
    // ============================================================
    Response GetSystemVersion(std::string& version) const override;
    Response GetProductModel(std::string& model) const override;
    Response GetChipType(int& chipType) const override;
    Response GetMaxConferences(int& maxConferences) const override;
    Response GetMaxMembers(int& maxMembers) const override;
    Response SetSystemLanguage(const std::string& language) override;
    Response GetSystemLanguage(std::string& language) const override;
    Response GetSystemTimezone(std::string& timezone) const override;
    Response SetSystemTimezone(const std::string& timezone) override;

    // ============================================================
    // 【AbsPhone扩展】网络配置
    // ============================================================
    Response GetLocalIPAddress(std::string& ipAddress) const override;
    Response GetSubnetMask(std::string& subnetMask) const override;
    Response GetGatewayAddress(std::string& gateway) const override;
    Response GetDNSServer(std::string& dnsServer) const override;
    Response GetMACAddress(std::string& macAddress) const override;
    Response IsDhcpEnabled(bool& isDhcp) const override;
    Response GetMTU(int& mtu) const override;
    Response SetNetworkConfig(const std::string& ipAddress, const std::string& subnetMask,
                              const std::string& gateway, const std::string& dns, bool isDhcp) override;
    Response GetNatConfig(bool& enableICE, bool& enableSTUN, std::string& stunServer,
                         bool& enableTURN, std::string& turnServer) const override;
    Response SetNatConfig(bool enableICE, bool enableSTUN, const std::string& stunServer,
                          bool enableTURN, const std::string& turnServer,
                          const std::string& turnUsername, const std::string& turnPassword) override;

    // ============================================================
    // 【AbsPhone扩展】视频配置
    // ============================================================
    Response GetDefaultVideoParams(int& width, int& height, int& framerate, int& bitrate) const override;
    Response SetDefaultVideoParams(int width, int height, int framerate, int bitrate) override;
    Response GetSupportedResolutions(std::string& resolutions) const override;
    Response GetVideoEncoderCount(int& encoderCount) const override;
    Response GetVideoInputDevices(std::string& devices) const override;
    Response GetVideoOutputDevices(std::string& devices) const override;
    Response SetVideoInputDevice(const std::string& deviceId) override;
    Response GetVideoInputDevice(std::string& deviceId) const override;
    Response GetDefaultVideoCodec(std::string& codecType) const override;
    Response SetDefaultVideoCodec(const std::string& codecType) override;
    Response GetKeyFrameInterval(int& interval) const override;
    Response SetKeyFrameInterval(int interval) override;

    // ============================================================
    // 【AbsPhone扩展】音频配置
    // ============================================================
    Response GetAudioInputDevices(std::string& devices) const override;
    Response GetAudioOutputDevices(std::string& devices) const override;
    Response SetAudioInputDevice(const std::string& deviceId) override;
    Response GetAudioInputDevice(std::string& deviceId) const override;
    Response SetAudioOutputDevice(const std::string& deviceId) override;
    Response GetAudioOutputDevice(std::string& deviceId) const override;
    Response GetMicrophoneVolume(int& volume) const override;
    Response SetMicrophoneVolume(int volume) override;
    Response GetSpeakerVolume(int& volume) const override;
    Response SetSpeakerVolume(int volume) override;
    Response GetDefaultAudioCodec(std::string& codec) const override;
    Response SetDefaultAudioCodec(const std::string& codec) override;
    Response IsAecEnabled(bool& enabled) const override;
    Response SetAecEnabled(bool enabled) override;
    Response IsNsEnabled(bool& enabled) const override;
    Response SetNsEnabled(bool enabled) override;
    Response IsAgcEnabled(bool& enabled) const override;
    Response SetAgcEnabled(bool enabled) override;

    // ============================================================
    // 【AbsPhone扩展】会议配置
    // ============================================================
    Response GetConferenceDefaults(int& maxDuration, int& maxParticipants, bool& enableRecording) const override;
    Response SetConferenceDefaults(int maxDuration, int maxParticipants, bool enableRecording) override;
    Response GetConferencePasswordConfig(bool& requireChairPassword, bool& requireGuestPassword) const override;
    Response GetDefaultLayoutTemplate(std::string& layoutTemplate) const override;
    Response SetDefaultLayoutTemplate(const std::string& layoutTemplate) override;
    Response GetRecordingSavePath(std::string& path) const override;
    Response SetRecordingSavePath(const std::string& path) override;

    // ============================================================
    // 【AbsPhone扩展】SIP/H323配置
    // ============================================================
    Response GetSipServer(std::string& server) const override;
    Response SetSipServer(const std::string& server) override;
    Response GetSipTransport(std::string& transport) const override;
    Response SetSipTransport(const std::string& transport) override;
    Response GetSipPort(int& port) const override;
    Response SetSipPort(int port) override;
    Response GetH323Gatekeeper(std::string& gatekeeper) const override;
    Response SetH323Gatekeeper(const std::string& gatekeeper) override;
    Response GetH323GatekeeperPort(int& port) const override;
    Response SetH323GatekeeperPort(int port) override;
    Response GetSipAccount(std::string& userName, std::string& displayName, std::string& authName) const override;
    Response SetSipAccount(const std::string& userName, const std::string& displayName,
                          const std::string& authName, const std::string& password) override;
    Response GetSrtpConfig(bool& enable, std::string& algorithm) const override;
    Response SetSrtpConfig(bool enable, const std::string& algorithm) override;

    // ============================================================
    // 【AbsPhone扩展】安全配置
    // ============================================================
    Response GetEncryptionConfig(bool& enableH235, std::string& algorithm) const override;
    Response SetEncryptionConfig(bool enableH235, const std::string& algorithm) override;
    Response GetFirewallConfig(bool& enabled, std::string& rules) const override;
    Response SetFirewallConfig(bool enabled, const std::string& rules) override;

    // ============================================================
    // 【AbsPhone扩展】存储配置
    // ============================================================
    Response GetStorageInfo(long long& totalSpace, long long& freeSpace) const override;
    Response GetLogPath(std::string& path) const override;
    Response SetLogPath(const std::string& path) override;
    Response GetLogLevel(int& level) const override;
    Response SetLogLevel(int level) override;
    Response GetLogRetentionDays(int& days) const override;
    Response SetLogRetentionDays(int days) override;
    Response GetDiagnosticPath(std::string& path) const override;
    Response SetDiagnosticPath(const std::string& path) override;

    // ============================================================
    // 【AbsPhone扩展】许可证配置
    // ============================================================
    Response GetLicenseInfo(std::string& licenseKey, std::string& expireDate, bool& isValid) const override;
    Response SetLicense(const std::string& licenseKey) override;
    Response ValidateLicense(bool& isValid, std::string& message) const override;

    // ============================================================
    // 【AbsPhone扩展】系统维护
    // ============================================================
    Response GetSystemUptime(int& uptime) const override;
    Response GetCpuUsage(double& usage) const override;
    Response GetMemoryUsage(long long& total, long long& used, double& usage) const override;
    Response RebootSystem() override;
    Response FactoryReset(bool keepNetwork, bool keepLicense) override;
    Response ExportConfig(const std::string& filePath) const override;
    Response ImportConfig(const std::string& filePath) override;
    Response CheckSystemUpdate(bool& updateAvailable, std::string& version, std::string& description) const override;
    Response PerformSystemUpdate(const std::string& updatePackagePath) override;

    // ============================================================
    // 【AbsPhone扩展】LDAP/认证配置
    // ============================================================
    Response GetLdapConfig(bool& enabled, std::string& server, int& port, std::string& baseDN) const override;
    Response SetLdapConfig(bool enabled, const std::string& server, int port, const std::string& baseDN,
                          const std::string& bindDN, const std::string& bindPassword) override;
    Response TestLdapConnection(bool& success, std::string& message) const override;
    Response GetAuthMode(std::string& mode) const override;
    Response SetAuthMode(const std::string& mode) override;

    // ============================================================
    // 【AbsPhone扩展】QoS配置
    // ============================================================
    Response GetQosConfig(bool& enable, int& dscpValue, int& priority) const override;
    Response SetQosConfig(bool enable, int dscpValue, int priority) override;
    Response GetBandwidthConfig(int& defaultBandwidth, int& maxBandwidth) const override;
    Response SetBandwidthConfig(int defaultBandwidth, int maxBandwidth) override;

private:
    // 一条可解析的 key=value 行。value 是去掉首尾空白后的原文，
    // eqPos 是 '=' 在 rawLines_[line] 中的字节偏移，写回时只替换
    // 该行 '=' 之后的内容。
    struct Record {
        std::string section;   // 原大小写；空 = 节区外
        std::string key;       // 原大小写
        std::string value;
        size_t line = 0;
        size_t eqPos = 0;
    };

    // 未落盘的内存覆盖值。identity 为 "section\x1fkey"（大小写不敏感）
    // 或裸键（文件里找不到、或跨节区歧义时）。
    struct OverlayEntry {
        std::string value;
        std::string displayKey;
    };

    // 规范键 -> 旧文件键。canonical/section 保留原大小写用于日志。
    struct AliasEntry {
        std::string canonical;
        std::string section;
        std::string legacyKey;
        std::string sectionLower;
        std::string legacyLower;
        AliasTransform transform = AliasTransform::None;
    };

    enum class ReadStatus { Ok, NotFound, Error };

    static ReadStatus ReadAllLines(const std::string& path,
                                   std::vector<std::string>* lines,
                                   std::vector<bool>* lineCr,
                                   bool* trailingEol,
                                   std::string* err);
    static void ParseLines(const std::vector<std::string>& lines,
                           std::vector<Record>* records,
                           std::unordered_map<std::string, size_t>* index,
                           std::unordered_map<std::string, std::vector<size_t>>* keyIndex,
                           std::unordered_map<std::string, std::string>* sectionNames,
                           std::vector<std::string>* warnings);
    static std::string ToLower(const std::string& s);
    static std::string MakeIdentity(const std::string& sectionLower,
                                    const std::string& keyLower);
    static std::string TrimCopy(const std::string& s);

    // 调用方必须持有 mutex_。
    bool LookupStringLocked(const std::string& inputKey, std::string* out,
                            std::string* warn) const;
    bool LookupAliasLocked(const AliasEntry& alias, std::string* out,
                           std::string* warn) const;
    // 返回需要在解锁后记录的告警（空 = 无）。写入被变换拒绝时不改 overlay。
    std::string PutOverlayLocked(const std::string& inputKey, const std::string& value);
    std::string WarnOnceLocked(const std::string& msg) const;
    // 别名目标在已加载文件中是否存在；返回缺失告警（调用方持锁）。
    void CollectMissingAliasTargetsLocked(std::vector<std::string>* warnings) const;

    // 当前生效的原文值：overlay 优先，其次文件记录（调用方持锁）。
    std::string CurrentRawLocked(const std::string& identity) const;
    // 把 overlay 叠加到 rawLines_ 生成完整文件内容（调用方持锁）。
    // maskSensitive=true 时敏感键的值替换为 "***"（导出副本，不改内存）。
    bool BuildContentLocked(bool maskSensitive, std::string* content,
                            std::string* err) const;
    // 写穿（docs/10 D2-A）：Set* 成功后立即落盘；失败记 Error 并保留 dirty。
    void PersistAfterSet();
    static bool StatFile(const std::string& path, int64_t* mtime, uint64_t* size,
                         bool* exists);

    mutable std::mutex mutex_;
    mutable std::mutex saveMutex_;   // L1s：串行化保存/导出（stat→tmp/bak/rename）
    mutable std::vector<std::string> rawLines_;           // 原文行（不含行尾）
    mutable std::vector<bool> lineCr_;                    // 每行行尾是否 CRLF
    mutable bool trailingEol_ = false;                    // 文件末尾是否有换行
    mutable std::vector<Record> records_;                 // 全部解析出的记录（含重复）
    mutable std::unordered_map<std::string, size_t> index_;  // identity -> records_ 下标（重复取最后）
    mutable std::unordered_map<std::string, std::vector<size_t>> keyIndex_;  // 小写键 -> 获胜记录下标
    mutable std::unordered_map<std::string, std::string> sectionNames_;      // 小写节区 -> 原大小写
    std::unordered_map<std::string, OverlayEntry> overlay_;
    std::unordered_map<std::string, AliasEntry> aliases_;   // 小写规范键 -> 旧键
    mutable std::set<std::string> warnedKeys_;
    std::vector<std::string> loadWarnings_;          // 等待组合根在日志初始化后输出
    bool persistAllowed_ = false;                    // 未干净加载时禁止写回
    mutable bool dirty_ = false;                     // overlay_ 有未落盘内容
    uint64_t dirtyGen_ = 0;                          // overlay_ 变更代数：并发写穿时不丢失后来者
    std::string loadedPath_;
    mutable int64_t loadedMtime_ = 0;
    mutable uint64_t loadedSize_ = 0;
};

} // namespace base
} // namespace vcs
