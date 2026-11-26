#include "Config.h"
#include "ILogger.h"
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>   // std::snprintf
#include <climits>  // INT_MIN, INT_MAX
#include <cstdlib>  // std::strtoll, std::strtod
#include <cerrno>   // errno
#include <cstring>  // std::strlen

namespace vcs { namespace base {

static bool TryParseBool(const std::string& s, bool* out);
static bool TryParseLong(const std::string& s, long long* out);
static bool TryParseDouble(const std::string& s, double* out);
static bool TryParseInt(const std::string& s, int* out);

std::string Config::GetString(const std::string& key, const std::string& defaultValue) const {
    std::string raw;
    std::string warn;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = LookupStringLocked(key, &raw, &warn);
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    return found ? raw : defaultValue;
}

int Config::GetInt(const std::string& key, int defaultValue) const {
    std::string raw;
    std::string warn;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = LookupStringLocked(key, &raw, &warn);
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    if (!found) {
        return defaultValue;
    }
    int value = 0;
    return TryParseInt(raw, &value) ? value : defaultValue;
}

bool Config::GetBool(const std::string& key, bool defaultValue) const {
    std::string raw;
    std::string warn;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = LookupStringLocked(key, &raw, &warn);
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    if (!found) {
        return defaultValue;
    }
    bool value = false;
    return TryParseBool(raw, &value) ? value : defaultValue;
}

double Config::GetDouble(const std::string& key, double defaultValue) const {
    std::string raw;
    std::string warn;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = LookupStringLocked(key, &raw, &warn);
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    if (!found) {
        return defaultValue;
    }
    double value = 0.0;
    return TryParseDouble(raw, &value) ? value : defaultValue;
}

void Config::SetString(const std::string& key, const std::string& value) {
    std::string warn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        warn = PutOverlayLocked(key, value);
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    PersistAfterSet();
}

void Config::SetInt(const std::string& key, int value) {
    std::string warn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        warn = PutOverlayLocked(key, std::to_string(value));
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    PersistAfterSet();
}

void Config::SetBool(const std::string& key, bool value) {
    std::string warn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        warn = PutOverlayLocked(key, value ? "True" : "False");
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    PersistAfterSet();
}

void Config::SetDouble(const std::string& key, double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    std::string warn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        warn = PutOverlayLocked(key, std::string(buf));
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    PersistAfterSet();
}

// Helper: try to parse a string as a bool. Returns true if it parses
// successfully and writes the result to *out. Recognised forms:
//   true / false / yes / no / on / off / 1 / 0 (case-insensitive).
static bool TryParseBool(const std::string& s, bool* out) {
    std::string v = s;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (v == "true" || v == "yes" || v == "on"  || v == "1") { *out = true;  return true; }
    if (v == "false"|| v == "no"  || v == "off" || v == "0") { *out = false; return true; }
    return false;
}

// Helper: try to parse a string as a long long. Out-param is only written
// on success.
static bool TryParseLong(const std::string& s, long long* out) {
    if (s.empty()) return false;
    char* endp = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &endp, 10);
    if (errno != 0 || endp == s.c_str() || (endp && *endp != '\0')) return false;
    *out = v;
    return true;
}

// Helper: strict int parse (whole string, range-checked).
static bool TryParseInt(const std::string& s, int* out) {
    long long v = 0;
    if (!TryParseLong(s, &v)) return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    *out = static_cast<int>(v);
    return true;
}

// Helper: try to parse a string as a double.
static bool TryParseDouble(const std::string& s, double* out) {
    if (s.empty()) return false;
    char* endp = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &endp);
    if (errno != 0 || endp == s.c_str() || (endp && *endp != '\0')) return false;
    *out = v;
    return true;
}

// ============================================================
// 别名变换（docs/10 §3.1.7）：旧文件值 <-> 规范视图值
// ============================================================

static std::string LowerAscii(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(r[i]);
        if (c >= 'A' && c <= 'Z') r[i] = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

static std::string TrimAscii(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// docs/10 §3.1.4：键名以 password/passwd/token/secret 结尾，
// 或等于 license.key；MachineCode 是设备标识，按 2.36 一并脱敏。
static bool IsSensitiveKey(const std::string& key) {
    const std::string k = LowerAscii(TrimAscii(key));
    if (k.empty()) return false;
    const char* suffixes[] = {"password", "passwd", "token", "secret"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        const size_t n = std::strlen(suffixes[i]);
        if (k.size() >= n && k.compare(k.size() - n, n, suffixes[i]) == 0) {
            return true;
        }
    }
    return k == "license.key" || k == "licensekey" || k == "machinecode";
}

struct SrtpAlgEntry { int value; const char* name; };
// 与旧系统 absphone_def.h:300-310 的 SRTPAlgorithm 逐项对应，文件名即写入值。
static const SrtpAlgEntry kSrtpAlgNames[] = {
    {0, "Auto"},
    {1, "SM4_128_ECB_HMAC_SM3"},
    {2, "SM4_128_CBC_HMAC_SM3"},
    {3, "SM4_128_OFB_HMAC_SM3"},
    {4, "SM4_128_CTR_HMAC_SM3"},
    {5, "AES_256_CM_HMAC_SHA1_80"},
    {6, "AES_256_CM_HMAC_SHA1_32"},
    {7, "AES_CM_128_HMAC_SHA1_80"},
    {8, "AES_CM_128_HMAC_SHA1_32"}
};
static const size_t kSrtpAlgCount = sizeof(kSrtpAlgNames) / sizeof(kSrtpAlgNames[0]);

// 旧文件值 -> 规范视图值。解析失败时原样返回并给出告警（视图侧仍有值）。
static bool AliasFileToCanonical(Config::AliasTransform t, const std::string& raw,
                                 std::string* out, std::string* warn) {
    switch (t) {
    case Config::AliasTransform::InvertBool: {
        bool b = false;
        if (!TryParseBool(TrimAscii(raw), &b)) {
            if (warn) *warn = "cannot parse bool '" + raw + "'";
            *out = raw;
            return true;
        }
        *out = b ? "False" : "True";
        return true;
    }
    case Config::AliasTransform::SrtpUse: {
        int v = 0;
        if (!TryParseInt(TrimAscii(raw), &v)) {
            if (warn) *warn = "cannot parse SRTP use '" + raw + "'";
            *out = raw;
            return true;
        }
        *out = (v != 0) ? "True" : "False";
        return true;
    }
    case Config::AliasTransform::SrtpAlg: {
        int v = 0;
        if (!TryParseInt(TrimAscii(raw), &v) || v < 0 ||
            v >= static_cast<int>(kSrtpAlgCount)) {
            if (warn) *warn = "unknown SRTP algorithm value '" + raw + "'";
            *out = raw;
            return true;
        }
        *out = kSrtpAlgNames[v].name;
        return true;
    }
    case Config::AliasTransform::GatekeeperMode:
        *out = (TrimAscii(raw) == "No gatekeeper") ? "False" : "True";
        return true;
    case Config::AliasTransform::None:
    default:
        *out = raw;
        return true;
    }
}

// 规范视图值 -> 旧文件值。返回 false 表示不应写入（值无法映射）。
static bool AliasCanonicalToFile(Config::AliasTransform t, const std::string& value,
                                 const std::string& currentRaw,
                                 std::string* out, std::string* warn) {
    switch (t) {
    case Config::AliasTransform::InvertBool: {
        bool b = false;
        if (!TryParseBool(TrimAscii(value), &b)) {
            if (warn) *warn = "invalid bool '" + value + "' (expected True/False)";
            return false;
        }
        *out = b ? "False" : "True";
        return true;
    }
    case Config::AliasTransform::SrtpUse: {
        bool enable = false;
        if (!TryParseBool(TrimAscii(value), &enable)) {
            if (warn) *warn = "invalid bool '" + value + "' (expected True/False)";
            return false;
        }
        if (!enable) {
            *out = "0";
            return true;
        }
        // 旧值 1/2 是 optional/mandatory，Set(true) 不能把 mandatory 降级为 optional。
        int current = 0;
        if (TryParseInt(TrimAscii(currentRaw), &current) && current > 0) {
            return false;   // 已启用：无需改写
        }
        *out = "1";
        return true;
    }
    case Config::AliasTransform::SrtpAlg: {
        std::string v = TrimAscii(value);
        int numeric = 0;
        if (TryParseInt(v, &numeric) && numeric >= 0 &&
            numeric < static_cast<int>(kSrtpAlgCount)) {
            *out = std::to_string(numeric);
            return true;
        }
        std::string lower = LowerAscii(v);
        for (size_t i = 0; i < kSrtpAlgCount; ++i) {
            if (lower == LowerAscii(kSrtpAlgNames[i].name)) {
                *out = std::to_string(kSrtpAlgNames[i].value);
                return true;
            }
        }
        if (warn) *warn = "unknown SRTP algorithm '" + value
                        + "' (known: 0..8 or a legacy algorithm name)";
        return false;
    }
    case Config::AliasTransform::GatekeeperMode: {
        bool b = false;
        if (!TryParseBool(TrimAscii(value), &b)) {
            if (warn) *warn = "invalid bool '" + value + "' (expected True/False)";
            return false;
        }
        *out = b ? "Use gatekeeper" : "No gatekeeper";
        return true;
    }
    case Config::AliasTransform::None:
    default:
        *out = value;
        return true;
    }
}

// docs/10 §3.1.3：tmp → bak → rename。成功后删除 .bak / .tmp（A5：不留残留）；
// 失败时原文件保持不变，并清理临时产物。调用方不得持有配置锁。
static bool AtomicWriteFile(const std::string& path, const std::string& content,
                            std::string* err) {
    namespace fs = boost::filesystem;
    boost::system::error_code ec;

    const std::string tmpPath = path + ".tmp";
    const std::string bakPath = path + ".bak";

    {
        std::ofstream out(tmpPath.c_str(),
                          std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            if (err) *err = "cannot open temp file: " + tmpPath;
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            boost::system::error_code ecRemove;
            fs::remove(tmpPath, ecRemove);
            if (err) *err = "write failed: " + tmpPath;
            return false;
        }
        out.close();
    }

    // boost::filesystem::exists(p, ec) 在"文件不存在"时也会置 ec，必须先看状态
    bool existed = false;
    {
        fs::file_status st = fs::status(path, ec);
        if (st.type() != fs::file_not_found) {
            if (ec) {
                boost::system::error_code ecRemove;
                fs::remove(tmpPath, ecRemove);
                if (err) *err = "cannot stat target: " + path;
                return false;
            }
            existed = fs::exists(st);
        }
    }

    bool backedUp = false;
    if (existed) {
        fs::copy_file(path, bakPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            boost::system::error_code ecRemove;
            fs::remove(tmpPath, ecRemove);
            if (err) *err = "cannot back up target: " + path;
            return false;
        }
        backedUp = true;
    }

    fs::rename(tmpPath, path, ec);
    if (ec) {
        boost::system::error_code ecCleanup;
        fs::remove(tmpPath, ecCleanup);
        if (backedUp) {
            boost::system::error_code ecExists;
            if (!fs::exists(path, ecExists)) {
                boost::system::error_code ecRestore;
                fs::rename(bakPath, path, ecRestore);
            } else {
                boost::system::error_code ecRemove;
                fs::remove(bakPath, ecRemove);
            }
        }
        if (err) *err = "rename failed: " + tmpPath + " -> " + path;
        return false;
    }

    if (backedUp) {
        boost::system::error_code ecRemove;
        fs::remove(bakPath, ecRemove);
    }
    return true;
}

// ============================================================
// 原文行模型辅助
// ============================================================

static void LogConfigWarning(const std::string& msg) {
    GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + msg);
}

std::string Config::ToLower(const std::string& s) {
    return LowerAscii(s);
}

std::string Config::TrimCopy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::string Config::MakeIdentity(const std::string& sectionLower,
                                 const std::string& keyLower) {
    std::string id;
    id.reserve(sectionLower.size() + 1 + keyLower.size());
    id += sectionLower;
    id.push_back('\x1f');
    id += keyLower;
    return id;
}

std::string Config::WarnOnceLocked(const std::string& msg) const {
    if (warnedKeys_.find(msg) != warnedKeys_.end()) return std::string();
    warnedKeys_.insert(msg);
    return msg;
}

void Config::SetKeyAliases(const std::vector<ConfigKeyAlias>& aliases) {
    std::vector<std::string> warnings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aliases_.clear();
        for (size_t i = 0; i < aliases.size(); ++i) {
            const ConfigKeyAlias& a = aliases[i];
            if (a.canonical.empty() || a.section.empty() || a.legacyKey.empty()) {
                warnings.push_back("invalid alias entry (empty canonical/section/legacyKey)");
                continue;
            }
            AliasEntry entry;
            entry.canonical = a.canonical;
            entry.section = a.section;
            entry.legacyKey = a.legacyKey;
            entry.sectionLower = ToLower(a.section);
            entry.legacyLower = ToLower(a.legacyKey);
            entry.transform = a.transform;
            aliases_[ToLower(a.canonical)] = entry;
        }
        if (!loadedPath_.empty()) {
            CollectMissingAliasTargetsLocked(&warnings);
        }
    }
    if (!warnings.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        loadWarnings_.insert(loadWarnings_.end(), warnings.begin(), warnings.end());
    }
}

std::vector<std::string> Config::TakeLoadWarnings() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(loadWarnings_);
    return out;
}

void Config::CollectMissingAliasTargetsLocked(std::vector<std::string>* warnings) const {
    for (std::unordered_map<std::string, AliasEntry>::const_iterator it = aliases_.begin();
         it != aliases_.end(); ++it) {
        const AliasEntry& a = it->second;
        if (index_.find(MakeIdentity(a.sectionLower, a.legacyLower)) != index_.end()) {
            continue;
        }
        warnings->push_back("alias target missing: '" + a.canonical + "' -> ["
                            + a.section + "] " + a.legacyKey
                            + " (memory-only; file has no such key)");
    }
}

bool Config::LookupAliasLocked(const AliasEntry& alias, std::string* out,
                               std::string* warn) const {
    if (out) out->clear();
    std::string id = MakeIdentity(alias.sectionLower, alias.legacyLower);

    std::string raw;
    bool found = false;
    std::unordered_map<std::string, OverlayEntry>::const_iterator ov = overlay_.find(id);
    if (ov != overlay_.end()) {
        raw = ov->second.value;
        found = true;
    } else {
        std::unordered_map<std::string, size_t>::const_iterator it = index_.find(id);
        if (it != index_.end()) {
            raw = records_[it->second].value;
            found = true;
        }
    }
    if (!found) return false;

    std::string transformWarn;
    AliasFileToCanonical(alias.transform, raw, out, &transformWarn);
    if (warn && !transformWarn.empty()) {
        *warn = WarnOnceLocked("alias '" + alias.canonical + "': " + transformWarn);
    }
    return true;
}

std::string Config::CurrentRawLocked(const std::string& identity) const {
    std::unordered_map<std::string, OverlayEntry>::const_iterator ov = overlay_.find(identity);
    if (ov != overlay_.end()) return ov->second.value;
    std::unordered_map<std::string, size_t>::const_iterator it = index_.find(identity);
    if (it != index_.end()) return records_[it->second].value;
    return std::string();
}

bool Config::StatFile(const std::string& path, int64_t* mtime, uint64_t* size,
                      bool* exists) {
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    fs::file_status st = fs::status(path, ec);
    if (st.type() == fs::file_not_found) {
        *exists = false;
        return true;
    }
    if (ec) return false;

    *exists = fs::exists(st);
    if (!*exists) return true;

    uintmax_t sz = fs::file_size(path, ec);
    if (ec) return false;
    std::time_t t = fs::last_write_time(path, ec);
    if (ec) return false;

    *size = static_cast<uint64_t>(sz);
    *mtime = static_cast<int64_t>(t);
    return true;
}

bool Config::BuildContentLocked(bool maskSensitive, std::string* content,
                                std::string* err) const {
    content->clear();
    std::vector<std::string> lines = rawLines_;

    if (maskSensitive) {
        // 覆盖所有解析出的敏感行（含重复键的落选行），避免导出泄漏。
        for (size_t i = 0; i < records_.size(); ++i) {
            const Record& rec = records_[i];
            if (!IsSensitiveKey(rec.key) || rec.line >= lines.size()) continue;
            const std::string& orig = rawLines_[rec.line];
            if (rec.eqPos >= orig.size() || orig[rec.eqPos] != '=') continue;
            lines[rec.line] = orig.substr(0, rec.eqPos + 1) + "***";
        }
    }

    for (std::unordered_map<std::string, OverlayEntry>::const_iterator it = overlay_.begin();
         it != overlay_.end(); ++it) {
        std::unordered_map<std::string, size_t>::const_iterator ri =
            index_.find(it->first);
        if (ri == index_.end()) continue;   // 文件里没有的键不写（不造键）
        const Record& rec = records_[ri->second];
        if (rec.line >= lines.size()) continue;
        const std::string& orig = rawLines_[rec.line];
        if (rec.eqPos >= orig.size() || orig[rec.eqPos] != '=') continue;

        if (it->second.value.find('\n') != std::string::npos ||
            it->second.value.find('\r') != std::string::npos) {
            if (err) {
                *err = "config value contains a line break: key '"
                     + it->second.displayKey + "'";
            }
            return false;
        }

        std::string value = it->second.value;
        if (maskSensitive && IsSensitiveKey(rec.key)) value = "***";
        lines[rec.line] = orig.substr(0, rec.eqPos + 1) + value;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        *content += lines[i];
        if (i + 1 < lines.size() || trailingEol_) {
            bool cr = i < lineCr_.size() ? lineCr_[i] : false;
            *content += cr ? "\r\n" : "\n";
        }
    }
    return true;
}

void Config::PersistAfterSet() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!persistAllowed_ || !dirty_ || loadedPath_.empty()) return;
        path = loadedPath_;
    }
    Response r = SaveToFile(path);
    if (!r.IsSuccess()) {
        GetLogManager()->GetLogger(LogModule::Base)->Error(
            "[Config] write-through failed: " + path + ": " + r.GetMsg());
    }
}

Config::ReadStatus Config::ReadAllLines(const std::string& path,
                                        std::vector<std::string>* lines,
                                        std::vector<bool>* lineCr,
                                        bool* trailingEol,
                                        std::string* err) {
    lines->clear();
    lineCr->clear();
    *trailingEol = false;

    namespace fs = boost::filesystem;
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        boost::system::error_code ec;
        fs::file_status st = fs::status(path, ec);
        if (st.type() == fs::file_not_found) {
            return ReadStatus::NotFound;
        }
        if (ec) {
            if (err) *err = "cannot stat file";
            return ReadStatus::Error;
        }
        if (err) *err = "cannot open file";
        return ReadStatus::Error;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    if (file.bad()) {
        if (err) *err = "read failure";
        return ReadStatus::Error;
    }
    file.close();
    std::string data = buf.str();

    // 仅内存去 BOM；写回不再添加
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }

    size_t start = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != '\n') continue;
        std::string line = data.substr(start, i - start);
        bool cr = false;
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
            cr = true;
        }
        lines->push_back(line);
        lineCr->push_back(cr);
        start = i + 1;
    }
    if (start < data.size()) {
        std::string line = data.substr(start);
        bool cr = false;
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
            cr = true;
        }
        lines->push_back(line);
        lineCr->push_back(cr);
    } else if (!data.empty()) {
        *trailingEol = true;
    }
    return ReadStatus::Ok;
}

void Config::ParseLines(const std::vector<std::string>& lines,
                        std::vector<Record>* records,
                        std::unordered_map<std::string, size_t>* index,
                        std::unordered_map<std::string, std::vector<size_t>>* keyIndex,
                        std::unordered_map<std::string, std::string>* sectionNames,
                        std::vector<std::string>* warnings) {
    records->clear();
    index->clear();
    keyIndex->clear();
    sectionNames->clear();

    std::string currentSection;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;   // 空行：原样保留
        char c = line[s];
        if (c == ';' || c == '#') continue;     // 注释：原样保留
        if (c == '"') continue;                 // 旧文件脏行：保留原文但不解析
        if (c == '[') {
            size_t rb = line.find(']', s + 1);
            if (rb != std::string::npos) {
                currentSection = TrimCopy(line.substr(s + 1, rb - s - 1));
                if (!currentSection.empty()) {
                    (*sectionNames)[ToLower(currentSection)] = currentSection;
                }
            }
            continue;
        }

        size_t eq = line.find('=', s);
        if (eq == std::string::npos) continue;  // 非 key=value：原样保留

        std::string key = TrimCopy(line.substr(s, eq - s));
        if (key.empty()) continue;

        Record rec;
        rec.section = currentSection;
        rec.key = key;
        rec.value = TrimCopy(line.substr(eq + 1));
        rec.line = i;
        rec.eqPos = eq;

        // 同 (section,key) 重复：索引取最后一条，原文全部保留
        std::string id = MakeIdentity(ToLower(rec.section), ToLower(rec.key));
        std::unordered_map<std::string, size_t>::iterator it = index->find(id);
        if (it != index->end() && warnings) {
            std::ostringstream oss;
            oss << "duplicate key '" << (rec.section.empty() ? std::string() : rec.section + ".")
                << rec.key << "': line " << (i + 1) << " overrides line "
                << ((*records)[it->second].line + 1);
            warnings->push_back(oss.str());
        }
        (*index)[id] = records->size();
        records->push_back(rec);
    }

    // keyIndex_ 只收录获胜记录，保持文件顺序
    for (size_t i = 0; i < records->size(); ++i) {
        const Record& rec = (*records)[i];
        std::string id = MakeIdentity(ToLower(rec.section), ToLower(rec.key));
        std::unordered_map<std::string, size_t>::const_iterator it = index->find(id);
        if (it == index->end() || it->second != i) continue;
        (*keyIndex)[ToLower(rec.key)].push_back(i);
    }
}

bool Config::LookupStringLocked(const std::string& inputKey, std::string* out,
                                std::string* warn) const {
    if (out) out->clear();
    if (warn) warn->clear();

    // 0) 旧键名别名：规范键直接映射到旧文件键并做视图变换
    std::unordered_map<std::string, AliasEntry>::const_iterator aliasIt =
        aliases_.find(ToLower(inputKey));
    if (aliasIt != aliases_.end()) {
        return LookupAliasLocked(aliasIt->second, out, warn);
    }

    // 1) 节区限定："Section.Key"（节区名大小写不敏感）
    size_t dot = inputKey.find('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < inputKey.size()) {
        std::string secLower = ToLower(inputKey.substr(0, dot));
        std::unordered_map<std::string, std::string>::const_iterator secIt =
            sectionNames_.find(secLower);
        if (secIt != sectionNames_.end()) {
            std::string id = MakeIdentity(secLower, ToLower(inputKey.substr(dot + 1)));
            std::unordered_map<std::string, OverlayEntry>::const_iterator ov =
                overlay_.find(id);
            if (ov != overlay_.end()) {
                *out = ov->second.value;
                return true;
            }
            std::unordered_map<std::string, size_t>::const_iterator it = index_.find(id);
            if (it != index_.end()) {
                *out = records_[it->second].value;
                return true;
            }
            return false;
        }
    }

    // 2) 裸键：先看裸 overlay（歧义键/未知键的 Set 落点）
    std::string bareLower = ToLower(inputKey);
    std::unordered_map<std::string, OverlayEntry>::const_iterator ov =
        overlay_.find(bareLower);
    if (ov != overlay_.end()) {
        *out = ov->second.value;
        return true;
    }

    std::unordered_map<std::string, std::vector<size_t>>::const_iterator ki =
        keyIndex_.find(bareLower);
    if (ki == keyIndex_.end() || ki->second.empty()) {
        return false;
    }
    if (ki->second.size() > 1) {
        if (warn) {
            std::string secs;
            for (size_t i = 0; i < ki->second.size(); ++i) {
                if (i) secs += ", ";
                secs += records_[ki->second[i]].section;
            }
            *warn = WarnOnceLocked("ambiguous key '" + inputKey + "' found in sections ["
                                   + secs + "]; use 'Section.Key'");
        }
        return false;
    }

    const Record& rec = records_[ki->second[0]];
    std::string id = MakeIdentity(ToLower(rec.section), ToLower(rec.key));
    ov = overlay_.find(id);
    if (ov != overlay_.end()) {
        *out = ov->second.value;
        return true;
    }
    *out = rec.value;
    return true;
}

std::string Config::PutOverlayLocked(const std::string& inputKey, const std::string& value) {
    // 0) 旧键名别名：写入旧键 identity，值为反向变换后的旧文件值
    std::unordered_map<std::string, AliasEntry>::const_iterator aliasIt =
        aliases_.find(ToLower(inputKey));
    if (aliasIt != aliases_.end()) {
        const AliasEntry& alias = aliasIt->second;
        std::string id = MakeIdentity(alias.sectionLower, alias.legacyLower);

        const std::string currentRaw = CurrentRawLocked(id);

        std::string fileValue;
        std::string warn;
        if (!AliasCanonicalToFile(alias.transform, value, currentRaw, &fileValue, &warn)) {
            return warn.empty() ? std::string()
                                : ("alias '" + alias.canonical + "': " + warn);
        }
        if (fileValue == currentRaw) {
            return std::string();   // 值未变化：不落盘
        }

        OverlayEntry entry;
        entry.value = fileValue;
        entry.displayKey = inputKey;
        overlay_[id] = entry;
        dirty_ = true;
        ++dirtyGen_;
        return std::string();
    }

    size_t dot = inputKey.find('.');
    std::string identity;
    bool resolved = false;

    if (dot != std::string::npos && dot > 0 && dot + 1 < inputKey.size()) {
        std::string secLower = ToLower(inputKey.substr(0, dot));
        if (sectionNames_.find(secLower) != sectionNames_.end()) {
            identity = MakeIdentity(secLower, ToLower(inputKey.substr(dot + 1)));
            resolved = true;
        }
    }

    if (!resolved) {
        std::string bareLower = ToLower(inputKey);
        std::unordered_map<std::string, std::vector<size_t>>::const_iterator ki =
            keyIndex_.find(bareLower);
        if (ki != keyIndex_.end() && ki->second.size() == 1) {
            const Record& rec = records_[ki->second[0]];
            identity = MakeIdentity(ToLower(rec.section), ToLower(rec.key));
        } else {
            // 歧义键或文件里不存在的键：落裸键 overlay，保存时不会造键
            identity = bareLower;
        }
    }

    if (CurrentRawLocked(identity) == value) {
        return std::string();   // 值未变化：不落盘
    }

    OverlayEntry entry;
    entry.value = value;
    entry.displayKey = inputKey;
    overlay_[identity] = entry;
    dirty_ = true;
    ++dirtyGen_;
    return std::string();
}

Response Config::LoadFromFile(const std::string& filePath) {
    if (filePath.empty()) {
        return Response::Error(ResponseCode::InvalidParams, "config path is empty");
    }

    // 与保存/导出共用 L1s：避免"读到旧文件 → 换入 → 保存写回过期内容"的交错
    std::lock_guard<std::mutex> saveLock(saveMutex_);

    std::string sourcePath = filePath;
    std::string sourceWarn;
    std::vector<std::string> lines;
    std::vector<bool> lineCr;
    bool trailingEol = false;
    std::string err;

    ReadStatus status = ReadAllLines(filePath, &lines, &lineCr, &trailingEol, &err);
    if (status == ReadStatus::NotFound) {
        // 崩溃残留：主文件缺失且存在 .bak 时加载备份
        std::string backupPath = filePath + ".bak";
        std::string backupErr;
        ReadStatus backupStatus = ReadAllLines(backupPath, &lines, &lineCr,
                                               &trailingEol, &backupErr);
        if (backupStatus == ReadStatus::Ok) {
            status = ReadStatus::Ok;
            sourcePath = backupPath;
            sourceWarn = "config file missing: " + filePath + "; loaded backup " + backupPath;
        } else if (backupStatus == ReadStatus::Error) {
            status = ReadStatus::Error;
            err = backupErr;
        }
    }

    if (status == ReadStatus::NotFound) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rawLines_.clear();
            lineCr_.clear();
            trailingEol_ = false;
            records_.clear();
            index_.clear();
            keyIndex_.clear();
            sectionNames_.clear();
            persistAllowed_ = false;
            dirty_ = false;
            loadedPath_.clear();
            loadWarnings_.assign(1, "config file not found: " + filePath
                                     + " (using built-in defaults, saving disabled)");
        }
        return Response::Error(ResponseCode::NotFound, "Cannot open file: " + filePath);
    }

    if (status == ReadStatus::Error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rawLines_.clear();
            lineCr_.clear();
            trailingEol_ = false;
            records_.clear();
            index_.clear();
            keyIndex_.clear();
            sectionNames_.clear();
            persistAllowed_ = false;
            dirty_ = false;
            loadedPath_.clear();
            loadWarnings_.assign(1, "config file unreadable: " + filePath + " (" + err
                                     + ") (saving disabled)");
        }
        return Response::Error(ResponseCode::InternalError,
                               "Cannot read config file: " + filePath + " (" + err + ")");
    }

    // 解析在锁外完成
    std::vector<Record> records;
    std::unordered_map<std::string, size_t> index;
    std::unordered_map<std::string, std::vector<size_t>> keyIndex;
    std::unordered_map<std::string, std::string> sectionNames;
    std::vector<std::string> warnings;
    ParseLines(lines, &records, &index, &keyIndex, &sectionNames, &warnings);

    const size_t recordCount = records.size();
    const size_t warningCount = warnings.size();

    int64_t mtime = 0;
    uint64_t size = 0;
    {
        namespace fs = boost::filesystem;
        boost::system::error_code ec;
        size = static_cast<uint64_t>(fs::file_size(sourcePath, ec));
        if (ec) size = 0;
        std::time_t t = fs::last_write_time(sourcePath, ec);
        if (!ec) mtime = static_cast<int64_t>(t);
    }

    std::vector<std::string> aliasWarnings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rawLines_.swap(lines);
        lineCr_.swap(lineCr);
        trailingEol_ = trailingEol;
        records_.swap(records);
        index_.swap(index);
        keyIndex_.swap(keyIndex);
        sectionNames_.swap(sectionNames);
        overlay_.clear();
        warnedKeys_.clear();
        persistAllowed_ = true;
        dirty_ = false;
        loadedPath_ = sourcePath;
        loadedMtime_ = mtime;
        loadedSize_ = size;
        CollectMissingAliasTargetsLocked(&aliasWarnings);
    }

    // 加载期警告先收集，由组合根在日志初始化后统一输出（docs/10 §3.2.10）：
    // 否则 LogLevel=Off 时仍会有告警输出，且会写进 CWD 下的默认日志。
    std::vector<std::string> loadWarnings;
    if (!sourceWarn.empty()) loadWarnings.push_back(sourceWarn);
    for (size_t i = 0; i < warnings.size(); ++i) {
        loadWarnings.push_back(warnings[i]);
    }
    for (size_t i = 0; i < aliasWarnings.size(); ++i) {
        loadWarnings.push_back(aliasWarnings[i]);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loadWarnings_.swap(loadWarnings);
    }

    std::ostringstream msg;
    msg << "loaded " << recordCount << " keys";
    if (warningCount > 0) msg << ", " << warningCount << " warning(s)";
    return Response::Success(msg.str());
}

Response Config::SaveToFile(const std::string& filePath) const {
    if (filePath.empty()) {
        return Response::Error(ResponseCode::InvalidParams, "config path is empty");
    }

    // L1s：串行化 stat → 外部改动合并 → 生成内容 → tmp/bak/rename
    std::lock_guard<std::mutex> saveLock(saveMutex_);

    int64_t expectedMtime = 0;
    uint64_t expectedSize = 0;
    bool sameFile = false;
    bool cleanSameFile = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!persistAllowed_) {
            return Response::Error(ResponseCode::InvalidOperation,
                                   "config not loaded cleanly; refuse to overwrite: " + filePath);
        }
        sameFile = !loadedPath_.empty() && filePath == loadedPath_;
        if (sameFile) {
            expectedMtime = loadedMtime_;
            expectedSize = loadedSize_;
            cleanSameFile = !dirty_;
        }
    }

    // 无修改且写回原文件：不写盘（A0：产物与原文逐字节相同）；
    // 但文件被外部删除时仍需重建，不能当作 "unchanged"。
    if (cleanSameFile) {
        int64_t cleanMtime = 0;
        uint64_t cleanSize = 0;
        bool cleanExists = true;
        if (StatFile(filePath, &cleanMtime, &cleanSize, &cleanExists) && cleanExists) {
            return Response::Success("unchanged");
        }
    }

    // 外部进程改过文件：以文件为准重建索引，overlay_ 随后重新叠加
    std::vector<std::string> mergeWarnings;
    if (sameFile) {
        int64_t mtime = 0;
        uint64_t size = 0;
        bool exists = true;
        if (!StatFile(filePath, &mtime, &size, &exists)) {
            return Response::Error(ResponseCode::InternalError,
                                   "cannot stat config file: " + filePath);
        }
        if (exists && (mtime != expectedMtime || size != expectedSize)) {
            std::vector<std::string> lines;
            std::vector<bool> lineCr;
            bool trailingEol = false;
            std::string readErr;
            if (ReadAllLines(filePath, &lines, &lineCr, &trailingEol, &readErr) != ReadStatus::Ok) {
                return Response::Error(ResponseCode::InternalError,
                                       "config file changed externally and cannot be re-read; "
                                       "refuse to overwrite: " + filePath
                                       + (readErr.empty() ? std::string() : " (" + readErr + ")"));
            }
            std::vector<Record> records;
            std::unordered_map<std::string, size_t> index;
            std::unordered_map<std::string, std::vector<size_t>> keyIndex;
            std::unordered_map<std::string, std::string> sectionNames;
            ParseLines(lines, &records, &index, &keyIndex, &sectionNames, &mergeWarnings);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                rawLines_.swap(lines);
                lineCr_.swap(lineCr);
                trailingEol_ = trailingEol;
                records_.swap(records);
                index_.swap(index);
                keyIndex_.swap(keyIndex);
                sectionNames_.swap(sectionNames);
                loadedMtime_ = mtime;
                loadedSize_ = size;
            }
        } else if (!exists) {
            std::lock_guard<std::mutex> lock(mutex_);
            loadedMtime_ = 0;
            loadedSize_ = 0;
        }
    }

    std::string content;
    std::string buildErr;
    uint64_t genAtBuild = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        genAtBuild = dirtyGen_;
        if (!BuildContentLocked(false, &content, &buildErr)) {
            return Response::Error(ResponseCode::InvalidParams, buildErr);
        }
    }

    // 文件 IO 在锁外
    std::string writeErr;
    if (!AtomicWriteFile(filePath, content, &writeErr)) {
        return Response::Error(ResponseCode::InternalError,
                               "failed to write config file: " + filePath + " (" + writeErr + ")");
    }

    int64_t newMtime = 0;
    uint64_t newSize = 0;
    bool newExists = true;
    if (!StatFile(filePath, &newMtime, &newSize, &newExists) || !newExists) {
        newSize = static_cast<uint64_t>(content.size());
        newMtime = 0;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 内容生成后又有新的 Set → 保留 dirty，等下一次写穿
        if (dirtyGen_ == genAtBuild) dirty_ = false;
        if (sameFile) {
            loadedMtime_ = newMtime;
            loadedSize_ = newSize;
        }
    }

    for (size_t i = 0; i < mergeWarnings.size(); ++i) {
        LogConfigWarning("external change merged: " + mergeWarnings[i]);
    }
    return Response::Success();
}

bool Config::HasKey(const std::string& key) const {
    std::string raw;
    std::string warn;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        found = LookupStringLocked(key, &raw, &warn);
    }
    if (!warn.empty()) {
        GetLogManager()->GetLogger(LogModule::Base)->Warning("[Config] " + warn);
    }
    return found;
}

std::vector<std::string> Config::GetKeys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    std::set<std::string> seen;

    for (size_t i = 0; i < records_.size(); ++i) {
        const Record& rec = records_[i];
        std::string id = MakeIdentity(ToLower(rec.section), ToLower(rec.key));
        std::unordered_map<std::string, size_t>::const_iterator it = index_.find(id);
        if (it == index_.end() || it->second != i) continue;
        std::string display = rec.section.empty() ? rec.key : rec.section + "." + rec.key;
        if (seen.insert(ToLower(display)).second) keys.push_back(display);
    }

    for (std::unordered_map<std::string, OverlayEntry>::const_iterator it = overlay_.begin();
         it != overlay_.end(); ++it) {
        if (index_.find(it->first) != index_.end()) continue;   // 文件键的覆盖
        if (seen.insert(ToLower(it->second.displayKey)).second) {
            keys.push_back(it->second.displayKey);
        }
    }
    return keys;
}

// ============================================================
// 【AbsPhone扩展】系统配置
// ============================================================
Response Config::GetSystemVersion(std::string& version) const {
    version = "1.0.0";
    return Response::Success();
}
Response Config::GetProductModel(std::string& model) const {
    model = "MCU-1000";
    return Response::Success();
}
Response Config::GetChipType(int& chipType) const { chipType = 0; return Response::Success(); }
Response Config::GetMaxConferences(int& maxConferences) const { maxConferences = 100; return Response::Success(); }
Response Config::GetMaxMembers(int& maxMembers) const { maxMembers = 1000; return Response::Success(); }
Response Config::SetSystemLanguage(const std::string& language) { 
    SetString("system.language", language); 
    return Response::Success(); 
}
Response Config::GetSystemLanguage(std::string& language) const {
    language = GetString("system.language", "en");
    return Response::Success();
}
Response Config::GetSystemTimezone(std::string& timezone) const {
    timezone = GetString("system.timezone", "UTC");
    return Response::Success();
}
Response Config::SetSystemTimezone(const std::string& timezone) {
    SetString("system.timezone", timezone);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】网络配置
// ============================================================
Response Config::GetLocalIPAddress(std::string& ipAddress) const {
    ipAddress = GetString("network.ip", "192.168.1.100");
    return Response::Success();
}
Response Config::GetSubnetMask(std::string& subnetMask) const {
    subnetMask = GetString("network.mask", "255.255.255.0");
    return Response::Success();
}
Response Config::GetGatewayAddress(std::string& gateway) const {
    gateway = GetString("network.gateway", "192.168.1.1");
    return Response::Success();
}
Response Config::GetDNSServer(std::string& dnsServer) const {
    dnsServer = GetString("network.dns", "8.8.8.8");
    return Response::Success();
}
Response Config::GetMACAddress(std::string& macAddress) const {
    macAddress = "00:11:22:33:44:55";
    return Response::Success();
}
Response Config::IsDhcpEnabled(bool& isDhcp) const {
    isDhcp = GetBool("network.dhcp", true);
    return Response::Success();
}
Response Config::GetMTU(int& mtu) const {
    mtu = GetInt("network.mtu", 1500);
    return Response::Success();
}
Response Config::SetNetworkConfig(const std::string& ipAddress, const std::string& subnetMask,
                                const std::string& gateway, const std::string& dns, bool isDhcp) {
    SetString("network.ip", ipAddress);
    SetString("network.mask", subnetMask);
    SetString("network.gateway", gateway);
    SetString("network.dns", dns);
    SetBool("network.dhcp", isDhcp);
    return Response::Success();
}
Response Config::GetNatConfig(bool& enableICE, bool& enableSTUN, std::string& stunServer,
                            bool& enableTURN, std::string& turnServer) const {
    enableICE = GetBool("nat.ice", false);
    enableSTUN = GetBool("nat.stun", false);
    stunServer = GetString("nat.stun.server", "");
    enableTURN = GetBool("nat.turn", false);
    turnServer = GetString("nat.turn.server", "");
    return Response::Success();
}
Response Config::SetNatConfig(bool enableICE, bool enableSTUN, const std::string& stunServer,
                            bool enableTURN, const std::string& turnServer,
                            const std::string& turnUsername, const std::string& turnPassword) {
    SetBool("nat.ice", enableICE);
    SetBool("nat.stun", enableSTUN);
    SetString("nat.stun.server", stunServer);
    SetBool("nat.turn", enableTURN);
    SetString("nat.turn.server", turnServer);
    SetString("nat.turn.username", turnUsername);
    SetString("nat.turn.password", turnPassword);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】视频配置
// ============================================================
Response Config::GetDefaultVideoParams(int& width, int& height, int& framerate, int& bitrate) const {
    width = GetInt("video.width", 1920);
    height = GetInt("video.height", 1080);
    framerate = GetInt("video.framerate", 30);
    bitrate = GetInt("video.bitrate", 2048);
    return Response::Success();
}
Response Config::SetDefaultVideoParams(int width, int height, int framerate, int bitrate) {
    SetInt("video.width", width);
    SetInt("video.height", height);
    SetInt("video.framerate", framerate);
    SetInt("video.bitrate", bitrate);
    return Response::Success();
}
Response Config::GetSupportedResolutions(std::string& resolutions) const {
    resolutions = GetString("video.resolutions", "1920x1080,1280x720,640x480");
    return Response::Success();
}
Response Config::GetVideoEncoderCount(int& encoderCount) const { encoderCount = 1; return Response::Success(); }
Response Config::GetVideoInputDevices(std::string& devices) const {
    devices = "[]";
    return Response::Success();
}
Response Config::GetVideoOutputDevices(std::string& devices) const {
    devices = "[]";
    return Response::Success();
}
Response Config::SetVideoInputDevice(const std::string& deviceId) {
    SetString("video.input.device", deviceId);
    return Response::Success();
}
Response Config::GetVideoInputDevice(std::string& deviceId) const {
    deviceId = GetString("video.input.device", "");
    return Response::Success();
}
Response Config::GetDefaultVideoCodec(std::string& codecType) const {
    codecType = GetString("video.codec", "H264");
    return Response::Success();
}
Response Config::SetDefaultVideoCodec(const std::string& codecType) {
    SetString("video.codec", codecType);
    return Response::Success();
}
Response Config::GetKeyFrameInterval(int& interval) const {
    interval = GetInt("video.keyframe", 2);
    return Response::Success();
}
Response Config::SetKeyFrameInterval(int interval) {
    SetInt("video.keyframe", interval);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】音频配置
// ============================================================
Response Config::GetAudioInputDevices(std::string& devices) const {
    devices = "[]";
    return Response::Success();
}
Response Config::GetAudioOutputDevices(std::string& devices) const {
    devices = "[]";
    return Response::Success();
}
Response Config::SetAudioInputDevice(const std::string& deviceId) {
    SetString("audio.input.device", deviceId);
    return Response::Success();
}
Response Config::GetAudioInputDevice(std::string& deviceId) const {
    deviceId = GetString("audio.input.device", "");
    return Response::Success();
}
Response Config::SetAudioOutputDevice(const std::string& deviceId) {
    SetString("audio.output.device", deviceId);
    return Response::Success();
}
Response Config::GetAudioOutputDevice(std::string& deviceId) const {
    deviceId = GetString("audio.output.device", "");
    return Response::Success();
}
Response Config::GetMicrophoneVolume(int& volume) const {
    volume = GetInt("audio.mic.volume", 80);
    return Response::Success();
}
Response Config::SetMicrophoneVolume(int volume) {
    SetInt("audio.mic.volume", volume);
    return Response::Success();
}
Response Config::GetSpeakerVolume(int& volume) const {
volume = GetInt("audio.speaker.volume", 80);
    return Response::Success();
}
Response Config::SetSpeakerVolume(int volume) {
    SetInt("audio.speaker.volume", volume);
    return Response::Success();
}
Response Config::GetDefaultAudioCodec(std::string& codec) const {
    codec = GetString("audio.codec", "G711U");
    return Response::Success();
}
Response Config::SetDefaultAudioCodec(const std::string& codec) {
    SetString("audio.codec", codec);
    return Response::Success();
}
Response Config::IsAecEnabled(bool& enabled) const { enabled = GetBool("audio.aec", true); return Response::Success(); }
Response Config::SetAecEnabled(bool enabled) { SetBool("audio.aec", enabled); return Response::Success(); }
Response Config::IsNsEnabled(bool& enabled) const { enabled = GetBool("audio.ns", true); return Response::Success(); }
Response Config::SetNsEnabled(bool enabled) { SetBool("audio.ns", enabled); return Response::Success(); }
Response Config::IsAgcEnabled(bool& enabled) const { enabled = GetBool("audio.agc", true); return Response::Success(); }
Response Config::SetAgcEnabled(bool enabled) { SetBool("audio.agc", enabled); return Response::Success(); }

// ============================================================
// 【AbsPhone扩展】会议配置
// ============================================================
Response Config::GetConferenceDefaults(int& maxDuration, int& maxParticipants, bool& enableRecording) const {
    maxDuration = GetInt("conf.max_duration", 480);
    maxParticipants = GetInt("conf.max_participants", 100);
    enableRecording = GetBool("conf.recording", true);
    return Response::Success();
}
Response Config::SetConferenceDefaults(int maxDuration, int maxParticipants, bool enableRecording) {
    SetInt("conf.max_duration", maxDuration);
    SetInt("conf.max_participants", maxParticipants);
    SetBool("conf.recording", enableRecording);
    return Response::Success();
}
Response Config::GetConferencePasswordConfig(bool& requireChairPassword, bool& requireGuestPassword) const {
    requireChairPassword = GetBool("conf.password.chair", false);
    requireGuestPassword = GetBool("conf.password.guest", false);
    return Response::Success();
}
Response Config::GetDefaultLayoutTemplate(std::string& layoutTemplate) const {
    layoutTemplate = GetString("conf.layout", "auto");
    return Response::Success();
}
Response Config::SetDefaultLayoutTemplate(const std::string& layoutTemplate) {
    SetString("conf.layout", layoutTemplate);
    return Response::Success();
}
Response Config::GetRecordingSavePath(std::string& path) const {
    path = GetString("conf.recording.path", "/ recordings");
    return Response::Success();
}
Response Config::SetRecordingSavePath(const std::string& path) {
    SetString("conf.recording.path", path);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】SIP/H323配置
// ============================================================
Response Config::GetSipServer(std::string& server) const {
    server = GetString("sip.server", "");
    return Response::Success();
}
Response Config::SetSipServer(const std::string& server) {
    SetString("sip.server", server);
    return Response::Success();
}
Response Config::GetSipTransport(std::string& transport) const {
    transport = GetString("sip.transport", "UDP");
    return Response::Success();
}
Response Config::SetSipTransport(const std::string& transport) {
    SetString("sip.transport", transport);
    return Response::Success();
}
Response Config::GetSipPort(int& port) const {
    port = GetInt("sip.port", 5060);
    return Response::Success();
}
Response Config::SetSipPort(int port) {
    SetInt("sip.port", port);
    return Response::Success();
}
Response Config::GetH323Gatekeeper(std::string& gatekeeper) const {
    gatekeeper = GetString("h323.gk", "");
    return Response::Success();
}
Response Config::SetH323Gatekeeper(const std::string& gatekeeper) {
    SetString("h323.gk", gatekeeper);
    return Response::Success();
}
Response Config::GetH323GatekeeperPort(int& port) const {
    port = GetInt("h323.gk.port", 1719);
    return Response::Success();
}
Response Config::SetH323GatekeeperPort(int port) {
    SetInt("h323.gk.port", port);
    return Response::Success();
}
Response Config::GetSipAccount(std::string& userName, std::string& displayName, std::string& authName) const {
    userName = GetString("sip.username", "");
    displayName = GetString("sip.displayname", "");
    authName = GetString("sip.authname", "");
    return Response::Success();
}
Response Config::SetSipAccount(const std::string& userName, const std::string& displayName,
                            const std::string& authName, const std::string& password) {
    SetString("sip.username", userName);
    SetString("sip.displayname", displayName);
    SetString("sip.authname", authName);
    SetString("sip.password", password);
    return Response::Success();
}
Response Config::GetSrtpConfig(bool& enable, std::string& algorithm) const {
    enable = GetBool("srtp.enable", true);
    algorithm = GetString("srtp.algorithm", "AES_CM_128_HMAC_SHA1_80");
    return Response::Success();
}
Response Config::SetSrtpConfig(bool enable, const std::string& algorithm) {
    SetBool("srtp.enable", enable);
    SetString("srtp.algorithm", algorithm);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】安全配置
// ============================================================
Response Config::GetEncryptionConfig(bool& enableH235, std::string& algorithm) const {
    enableH235 = GetBool("security.h235", false);
    algorithm = GetString("security.algorithm", "AES-128");
    return Response::Success();
}
Response Config::SetEncryptionConfig(bool enableH235, const std::string& algorithm) {
    SetBool("security.h235", enableH235);
    SetString("security.algorithm", algorithm);
    return Response::Success();
}
Response Config::GetFirewallConfig(bool& enabled, std::string& rules) const {
    enabled = GetBool("firewall.enable", false);
    rules = GetString("firewall.rules", "[]");
    return Response::Success();
}
Response Config::SetFirewallConfig(bool enabled, const std::string& rules) {
    SetBool("firewall.enable", enabled);
    SetString("firewall.rules", rules);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】存储配置
// ============================================================
Response Config::GetStorageInfo(long long& totalSpace, long long& freeSpace) const {
    totalSpace = GetInt("storage.total", 1000000);
    freeSpace = GetInt("storage.free", 500000);
    return Response::Success();
}
Response Config::GetLogPath(std::string& path) const {
    path = GetString("log.path", "/var/log/mcu");
    return Response::Success();
}
Response Config::SetLogPath(const std::string& path) {
    SetString("log.path", path);
    return Response::Success();
}
Response Config::GetLogLevel(int& level) const {
    level = GetInt("log.level", 2);
    return Response::Success();
}
Response Config::SetLogLevel(int level) {
    SetInt("log.level", level);
    return Response::Success();
}
Response Config::GetLogRetentionDays(int& days) const {
    days = GetInt("log.retention", 30);
    return Response::Success();
}
Response Config::SetLogRetentionDays(int days) {
    SetInt("log.retention", days);
    return Response::Success();
}
Response Config::GetDiagnosticPath(std::string& path) const {
    path = GetString("diagnostic.path", "/tmp/diagnostics");
    return Response::Success();
}
Response Config::SetDiagnosticPath(const std::string& path) {
    SetString("diagnostic.path", path);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】许可证配置
// ============================================================
Response Config::GetLicenseInfo(std::string& licenseKey, std::string& expireDate, bool& isValid) const {
    licenseKey = GetString("license.key", "");
    expireDate = GetString("license.expire", "");
    isValid = !licenseKey.empty();
    return Response::Success();
}
Response Config::SetLicense(const std::string& licenseKey) {
    SetString("license.key", licenseKey);
    return Response::Success();
}
Response Config::ValidateLicense(bool& isValid, std::string& message) const {
    isValid = !GetString("license.key", "").empty();
    message = isValid ? "License valid" : "License invalid";
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】系统维护
// ============================================================
Response Config::GetSystemUptime(int& uptime) const { uptime = 0; return Response::Success(); }
Response Config::GetCpuUsage(double& usage) const { usage = 0.0; return Response::Success(); }
Response Config::GetMemoryUsage(long long& total, long long& used, double& usage) const {
    total = 8192; used = 4096; usage = 50.0;
    return Response::Success();
}
Response Config::RebootSystem() { return Response::Success(); }
Response Config::FactoryReset(bool keepNetwork, bool keepLicense) { return Response::Success(); }
Response Config::ExportConfig(const std::string& filePath) const {
    if (filePath.empty()) {
        return Response::Error(ResponseCode::InvalidParams, "export path is empty");
    }

    std::lock_guard<std::mutex> saveLock(saveMutex_);

    std::string content;
    std::string err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!persistAllowed_) {
            return Response::Error(ResponseCode::InvalidOperation,
                                   "config not loaded cleanly; refuse to export: " + filePath);
        }
        if (filePath == loadedPath_) {
            return Response::Error(ResponseCode::InvalidOperation,
                                   "refuse to export over the live config file: " + filePath);
        }
        if (!BuildContentLocked(true, &content, &err)) {
            return Response::Error(ResponseCode::InvalidParams, err);
        }
    }

    if (!AtomicWriteFile(filePath, content, &err)) {
        return Response::Error(ResponseCode::InternalError,
                               "failed to export config: " + filePath + " (" + err + ")");
    }
    return Response::Success("exported (masked)");
}
Response Config::ImportConfig(const std::string& filePath) { return LoadFromFile(filePath); }
Response Config::CheckSystemUpdate(bool& updateAvailable, std::string& version, std::string& description) const {
    updateAvailable = false;
    version = "";
    description = "";
    return Response::Success();
}
Response Config::PerformSystemUpdate(const std::string& updatePackagePath) { return Response::Success(); }

// ============================================================
// 【AbsPhone扩展】LDAP/认证配置
// ============================================================
Response Config::GetLdapConfig(bool& enabled, std::string& server, int& port, std::string& baseDN) const {
    enabled = GetBool("ldap.enable", false);
    server = GetString("ldap.server", "");
    port = GetInt("ldap.port", 389);
    baseDN = GetString("ldap.basedn", "");
    return Response::Success();
}
Response Config::SetLdapConfig(bool enabled, const std::string& server, int port, const std::string& baseDN,
                            const std::string& bindDN, const std::string& bindPassword) {
    SetBool("ldap.enable", enabled);
    SetString("ldap.server", server);
    SetInt("ldap.port", port);
    SetString("ldap.basedn", baseDN);
    SetString("ldap.binddn", bindDN);
    SetString("ldap.bindpassword", bindPassword);
    return Response::Success();
}
Response Config::TestLdapConnection(bool& success, std::string& message) const {
    success = false;
    message = "LDAP not configured";
    return Response::Success();
}
Response Config::GetAuthMode(std::string& mode) const {
    mode = GetString("auth.mode", "local");
    return Response::Success();
}
Response Config::SetAuthMode(const std::string& mode) {
    SetString("auth.mode", mode);
    return Response::Success();
}

// ============================================================
// 【AbsPhone扩展】QoS配置
// ============================================================
Response Config::GetQosConfig(bool& enable, int& dscpValue, int& priority) const {
    enable = GetBool("qos.enable", false);
    dscpValue = GetInt("qos.dscp", 46);
    priority = GetInt("qos.priority", 5);
    return Response::Success();
}
Response Config::SetQosConfig(bool enable, int dscpValue, int priority) {
    SetBool("qos.enable", enable);
    SetInt("qos.dscp", dscpValue);
    SetInt("qos.priority", priority);
    return Response::Success();
}
Response Config::GetBandwidthConfig(int& defaultBandwidth, int& maxBandwidth) const {
    defaultBandwidth = GetInt("bw.default", 2048);
    maxBandwidth = GetInt("bw.max", 4096);
    return Response::Success();
}
Response Config::SetBandwidthConfig(int defaultBandwidth, int maxBandwidth) {
    SetInt("bw.default", defaultBandwidth);
    SetInt("bw.max", maxBandwidth);
    return Response::Success();
}

} // namespace base
} // namespace vcs
