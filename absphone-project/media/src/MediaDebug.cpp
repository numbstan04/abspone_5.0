#include "media_export.h"
#include "IMediaDebug.h"
#include "ILogger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

namespace vcs { namespace media {

namespace {

namespace fs = boost::filesystem;

// 队列容量按"最坏 3 路抽点 × 50 帧/秒"留 3 秒余量；满了丢新记录，
// 只加计数，绝不在采集/编码线程上等锁或落盘。
const size_t kQueueCapacity = 512;

// create_directories 会逐级创建父目录（outputdir 写成绝对路径时父目录
// 不一定存在）；已存在视为成功，已存在但不是目录则失败。
bool EnsureDirectory(const std::string& path) {
    if (path.empty()) return false;

    boost::system::error_code ec;
    const fs::path dir(path);
    if (fs::is_directory(dir, ec) && !ec) return true;

    fs::create_directories(dir, ec);
    if (ec) return false;

    ec.clear();
    return fs::is_directory(dir, ec) && !ec;
}

size_t GetDirSize(const std::string& dirPath) {
    boost::system::error_code ec;
    const fs::path dir(dirPath);
    if (!fs::is_directory(dir, ec) || ec) return 0;

    size_t total = 0;
    const fs::directory_iterator end;
    for (fs::directory_iterator it(dir, ec); !ec && it != end; it.increment(ec)) {
        boost::system::error_code itemEc;
        if (!fs::is_regular_file(it->path(), itemEc) || itemEc) continue;
        const boost::uintmax_t size = fs::file_size(it->path(), itemEc);
        if (!itemEc) total += static_cast<size_t>(size);
    }
    return total;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

// 会话 id 直接进文件名，必须挡住路径分隔符（旧实现把 "audio/input"
// 拼进文件名，父目录不存在，ofstream 静默失败）。
std::string SanitizeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) out = "unnamed";
    return out;
}

void PutLE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void PutLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// 44 字节 PCM WAV 头。流式写入时先占位，writer 每秒回填 size。
void WriteWavHeader(std::ofstream& ofs, uint32_t sampleRate, uint16_t channels,
                    uint16_t bitsPerSample, uint32_t dataBytes) {
    const uint16_t blockAlign =
        static_cast<uint16_t>(channels * (bitsPerSample / 8));
    const uint32_t byteRate = sampleRate * static_cast<uint32_t>(blockAlign);

    uint8_t h[44];
    std::memcpy(h + 0, "RIFF", 4);
    PutLE32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    PutLE32(h + 16, 16);
    PutLE16(h + 20, 1);                    // PCM
    PutLE16(h + 22, channels);
    PutLE32(h + 24, sampleRate);
    PutLE32(h + 28, byteRate);
    PutLE16(h + 32, blockAlign);
    PutLE16(h + 34, bitsPerSample);
    std::memcpy(h + 36, "data", 4);
    PutLE32(h + 40, dataBytes);
    ofs.write(reinterpret_cast<const char*>(h), 44);
}

std::string ToLower(const std::string& in) {
    std::string out = in;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

} // namespace

// ============================================================
// MediaDebug implementation
// ============================================================

class MediaDebug : public IMediaDebug {
public:
    MediaDebug() {
        // 不在这里建目录：默认关闭时不应在 CWD 留下 ./debug。
    }

    ~MediaDebug() override {
        StopWriter();
    }

    // ========== Configuration ==========

    MediaDebugConfig GetConfig() const override {
        std::lock_guard<std::mutex> lock(configMu_);
        return config_;
    }

    void SetConfig(const MediaDebugConfig& config) override {
        {
            std::lock_guard<std::mutex> lock(configMu_);
            config_ = config;
        }
        AfterConfigChanged();
    }

    bool IsEnabled() const override {
        return enabled_.load(std::memory_order_relaxed);
    }

    void SetEnabled(bool enabled) override {
        {
            std::lock_guard<std::mutex> lock(configMu_);
            config_.enabled = enabled;
        }
        AfterConfigChanged();
    }

    void LoadFromFile(const std::string& configPath) override {
        if (configPath.empty()) return;

        std::ifstream ifs(configPath.c_str());
        if (!ifs.is_open()) {
            return;   // 没有调试配置文件是正常情况，不报错
        }

        std::string currentSection;
        std::map<std::string, std::string> sectionConfig;

        std::string line;
        while (std::getline(ifs, line)) {
            line.erase(0, line.find_first_not_of(" \t\r"));
            line.erase(line.find_last_not_of(" \t\r") + 1);
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            if (line[0] == '[' && line[line.length() - 1] == ']') {
                try {
                    ApplyConfigSection(currentSection, sectionConfig);
                } catch (const std::exception& e) {
                    VCS_MEDIA_LOG_WARNING("[MediaDebug] section [%s] parse failed: %s",
                                          currentSection.c_str(), e.what());
                }
                currentSection = ToLower(line.substr(1, line.length() - 2));
                sectionConfig.clear();
                continue;
            }

            const size_t eqPos = line.find('=');
            if (eqPos != std::string::npos) {
                std::string key = ToLower(line.substr(0, eqPos));
                std::string value = line.substr(eqPos + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                // 行尾注释：# 或 ; 之后一律忽略（便于在 ini 里逐项标注）
                const size_t comment = value.find_first_of(";#");
                if (comment != std::string::npos) {
                    value.erase(comment);
                    value.erase(value.find_last_not_of(" \t") + 1);
                }
                sectionConfig[key] = value;
            }
        }
        try {
            ApplyConfigSection(currentSection, sectionConfig);
        } catch (const std::exception& e) {
            // 坏值只影响它所在的那一行；前面解析成功的段仍然生效。
            VCS_MEDIA_LOG_WARNING("[MediaDebug] config parse failed: %s", e.what());
        }

        AfterConfigChanged();

        const MediaDebugConfig cfg = GetConfig();
        VCS_MEDIA_LOG_INFO(
            "[MediaDebug] loaded: %s (enabled=%d, audio=%d/%d/%d/%d, dir=%s)",
            configPath.c_str(), (int)cfg.enabled, (int)cfg.audio.enabled,
            (int)cfg.audio.saveInput, (int)cfg.audio.saveEncoded,
            (int)cfg.audio.saveOutput, cfg.outputDir.c_str());
    }

    // ========== Audio taps (TX only) ==========

    void SaveAudioInput(const std::string& memberId,
                        const uint8_t* data,
                        size_t size) override {
        if (!captureOn_.load(std::memory_order_relaxed)) return;
        Enqueue(TapCapture, memberId, data, size);
    }

    void SaveAudioOutput(const std::string& memberId,
                         const uint8_t* data,
                         size_t size) override {
        if (!encodedOn_.load(std::memory_order_relaxed)) return;
        Enqueue(TapEncoded, memberId, data, size);
    }

    void SaveAudioMixed(const uint8_t* data, size_t size) override {
        (void)data;
        (void)size;
    }

    void SaveAudioDecoded(const std::string& memberId,
                          const uint8_t* data,
                          size_t size) override {
        (void)memberId;
        (void)data;
        (void)size;
    }

    void SaveAudioEncoded(const std::string& memberId,
                          const uint8_t* data,
                          size_t size) override {
        if (!preEncodeOn_.load(std::memory_order_relaxed)) return;
        Enqueue(TapPreEncode, memberId, data, size);
    }

    // ========== Video / RTP taps (not wired) ==========

    void SaveVideoInput(const std::string&, const uint8_t*, size_t, uint32_t) override {}
    void SaveVideoOutput(const std::string&, const uint8_t*, size_t, uint32_t) override {}
    void SaveVideoComposed(const uint8_t*, size_t, uint32_t) override {}
    void SaveVideoDecoded(const std::string&, const uint8_t*, size_t) override {}
    void SaveVideoEncoded(const std::string&, const uint8_t*, size_t, bool) override {}
    void SaveRtpIncoming(const uint8_t*, size_t, uint32_t, uint16_t) override {}
    void SaveRtpOutgoing(const uint8_t*, size_t, uint32_t, uint16_t) override {}

    // ========== Cleanup ==========

    void Cleanup() override {
        std::vector<std::string> paths;
        {
            std::lock_guard<std::mutex> lock(sinkMu_);
            for (std::map<std::string, std::unique_ptr<Sink> >::iterator it = sinks_.begin();
                 it != sinks_.end(); ++it) {
                CloseSink(it->second.get());
                paths.push_back(it->second->path);
            }
            sinks_.clear();
            failedKeys_.clear();
            totalStreams_ = 0;
        }
        for (size_t i = 0; i < paths.size(); ++i) {
            std::remove(paths[i].c_str());
        }
        VCS_MEDIA_LOG_INFO("[MediaDebug] cleanup: removed %zu file(s)", paths.size());
    }

    size_t GetDebugDirSize() const override {
        std::string dir;
        {
            std::lock_guard<std::mutex> lock(configMu_);
            dir = config_.outputDir;
        }
        return GetDirSize(dir);
    }

private:
    enum TapKind {
        TapCapture = 0,
        TapPreEncode = 1,
        TapEncoded = 2
    };

    struct Record {
        int kind;
        std::string memberId;
        std::vector<uint8_t> bytes;
    };

    struct Sink {
        Sink() : wav(false), dataBytes(0), lastPatched(0), limitLogged(false) {}
        std::string path;
        std::ofstream ofs;
        bool wav;
        uint64_t dataBytes;
        uint64_t lastPatched;
        bool limitLogged;
    };

    // ---- hot path ----

    void Enqueue(int kind, const std::string& memberId,
                 const uint8_t* data, size_t size) {
        if (!data || size == 0) return;

        Record r;
        r.kind = kind;
        r.memberId = memberId;
        r.bytes.assign(data, data + size);

        {
            std::lock_guard<std::mutex> lock(queueMu_);
            if (queue_.size() >= kQueueCapacity) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue_.push_back(std::move(r));
            queued_.fetch_add(1, std::memory_order_relaxed);
        }
        queueCv_.notify_one();
    }

    // ---- lifecycle ----

    void AfterConfigChanged() {
        MediaDebugConfig cfg;
        {
            std::lock_guard<std::mutex> lock(configMu_);
            cfg = config_;
        }
        // 目录只在真正开始记录时由写线程创建：关闭状态不应在磁盘上留下
        // 任何东西（包括 ./debug）。

        const bool en = cfg.enabled;
        const bool audio = en && cfg.audio.enabled;
        enabled_.store(en, std::memory_order_relaxed);
        captureOn_.store(audio && cfg.audio.saveInput, std::memory_order_relaxed);
        preEncodeOn_.store(audio && cfg.audio.saveEncoded, std::memory_order_relaxed);
        encodedOn_.store(audio && cfg.audio.saveOutput, std::memory_order_relaxed);

        if (en && (cfg.video.saveInput || cfg.video.saveOutput ||
                   cfg.video.saveComposed || cfg.video.saveDecoded ||
                   cfg.video.saveEncoded || cfg.rtp.saveIncoming ||
                   cfg.rtp.saveOutgoing)) {
            VCS_MEDIA_LOG_WARNING(
                "[MediaDebug] video/rtp taps are not wired; only audio TX "
                "(capture / pre-encode / encoded) is recorded");
        }

        if (captureOn_.load(std::memory_order_relaxed) ||
            preEncodeOn_.load(std::memory_order_relaxed) ||
            encodedOn_.load(std::memory_order_relaxed)) {
            StartWriter();
        }
    }

    void StartWriter() {
        bool expected = false;
        if (!writerStarted_.compare_exchange_strong(expected, true)) return;

        {
            std::lock_guard<std::mutex> lock(lifeMu_);
            writerRunning_.store(true, std::memory_order_release);
            writerThread_ = std::thread(&MediaDebug::WriterLoop, this);
        }
    }

    void StopWriter() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lock(lifeMu_);
            if (!writerThread_.joinable()) return;
            writerRunning_.store(false, std::memory_order_release);
            t = std::move(writerThread_);
        }
        queueCv_.notify_all();
        t.join();
    }

    // ---- writer thread (single owner of sinks) ----

    void WriterLoop() {
        // 目录与配额在启动时取一次：writer 运行中不改配置，避免文件头/
        // 配额与已写数据不一致。
        MediaDebugConfig cfg = GetConfig();
        writerDir_ = cfg.outputDir.empty() ? std::string("./debug") : cfg.outputDir;
        quotaBytes_ = static_cast<uint64_t>(cfg.maxTotalSizeMB) * 1024ull * 1024ull;
        fileLimitBytes_ = static_cast<uint64_t>(cfg.maxFileSizeMB) * 1024ull * 1024ull;

        // 音频抽点恒为 8k/1ch/int16：WAV 头必须与数据一致，否则 Audacity
        // 会按错误速率播放（变快变尖），把诊断引向错误结论。配置里的
        // samplerate/channels 只做提示，不参与写头。
        wavRate_ = 8000u;
        wavChannels_ = 1u;
        wavBits_ = 16u;
        if (cfg.audio.sampleRate != 8000 || cfg.audio.channels != 1 ||
            cfg.audio.bitsPerSample != 16) {
            VCS_MEDIA_LOG_WARNING(
                "[MediaDebug] ignoring audio samplerate/channels/bits (%d/%d/%d): "
                "audio taps are fixed 8000Hz/1ch/16bit",
                cfg.audio.sampleRate, cfg.audio.channels, cfg.audio.bitsPerSample);
        }

        if (!EnsureDirectory(writerDir_)) {
            VCS_MEDIA_LOG_ERROR("[MediaDebug] cannot create output dir: %s",
                                writerDir_.c_str());
        }

        VCS_MEDIA_LOG_INFO(
            "[MediaDebug] writer started: dir=%s, wav=%uHz/%uch/%ubit, quota=%lluMB",
            writerDir_.c_str(), wavRate_, (unsigned)wavChannels_, (unsigned)wavBits_,
            static_cast<unsigned long long>(quotaBytes_ >> 20));

        uint64_t lastRecords = 0;
        uint64_t lastDropped = 0;
        uint64_t lastErrors = 0;
        uint64_t lastStatMs = 0;

        while (true) {
            std::deque<Record> batch;
            bool stopping = false;
            {
                std::unique_lock<std::mutex> lock(queueMu_);
                queueCv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                    return !queue_.empty() || !writerRunning_.load(std::memory_order_acquire);
                });
                if (!queue_.empty()) batch.swap(queue_);
                stopping = !writerRunning_.load(std::memory_order_acquire);
            }

            if (!batch.empty()) {
                std::lock_guard<std::mutex> lock(sinkMu_);
                for (size_t i = 0; i < batch.size(); ++i) {
                    WriteRecord(batch[i]);
                }
                PatchHeaders();
                totalStreams_ = sinks_.size();
            }

            const int64_t nowMs = NowMs();
            if (lastStatMs == 0) lastStatMs = nowMs;
            if (nowMs - lastStatMs >= 1000) {
                lastStatMs = nowMs;

                const uint64_t records = recordsWritten_.load(std::memory_order_relaxed);
                const uint64_t dropped = dropped_.load(std::memory_order_relaxed);
                const uint64_t errors = writeErrors_.load(std::memory_order_relaxed);
                if (records != lastRecords || dropped != lastDropped || errors != lastErrors) {
                    VCS_MEDIA_LOG_INFO(
                        "[MediaDebug] audio taps: written=%llu, bytes=%llu, "
                        "dropped=%llu, writeErrors=%llu, streams=%zu",
                        static_cast<unsigned long long>(records),
                        static_cast<unsigned long long>(bytesWritten_.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(dropped),
                        static_cast<unsigned long long>(errors),
                        totalStreams_);
                    lastRecords = records;
                    lastDropped = dropped;
                    lastErrors = errors;
                }
            }

            if (stopping && batch.empty()) {
                std::lock_guard<std::mutex> lock(queueMu_);
                if (queue_.empty()) break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(sinkMu_);
            for (std::map<std::string, std::unique_ptr<Sink> >::iterator it = sinks_.begin();
                 it != sinks_.end(); ++it) {
                CloseSink(it->second.get());
            }
        }

        VCS_MEDIA_LOG_INFO(
            "[MediaDebug] writer stopped: written=%llu, bytes=%llu, dropped=%llu, writeErrors=%llu",
            static_cast<unsigned long long>(recordsWritten_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(bytesWritten_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(dropped_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(writeErrors_.load(std::memory_order_relaxed)));
    }

    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    Sink* GetOrOpenSink(int kind, const std::string& memberId) {
        const std::string key = std::to_string(kind) + "|" + memberId;
        if (failedKeys_.find(key) != failedKeys_.end()) return nullptr;

        std::map<std::string, std::unique_ptr<Sink> >::iterator it = sinks_.find(key);
        if (it != sinks_.end()) return it->second.get();

        std::string name;
        bool wav = false;
        switch (kind) {
        case TapCapture:   name = "tx_capture_" + SanitizeName(memberId) + ".wav";   wav = true; break;
        case TapPreEncode: name = "tx_preencode_" + SanitizeName(memberId) + ".wav"; wav = true; break;
        default:           name = "tx_encoded_" + SanitizeName(memberId) + ".g711";  wav = false; break;
        }

        std::unique_ptr<Sink> sink(new Sink());
        sink->wav = wav;
        sink->path = JoinPath(writerDir_, name);
        sink->ofs.open(sink->path.c_str(), std::ios::binary | std::ios::trunc);
        if (!sink->ofs.is_open()) {
            // 只报一次：否则 50 帧/秒会把日志刷爆。
            failedKeys_.insert(key);
            VCS_MEDIA_LOG_ERROR("[MediaDebug] cannot open: %s", sink->path.c_str());
            writeErrors_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (wav) {
            WriteWavHeader(sink->ofs, wavRate_, wavChannels_, wavBits_, 0);
        }

        VCS_MEDIA_LOG_INFO("[MediaDebug] stream opened: %s (%s)",
                           sink->path.c_str(),
                           wav ? "wav 16-bit PCM" : "raw G.711");

        Sink* raw = sink.get();
        sinks_[key] = std::move(sink);
        return raw;
    }

    void WriteRecord(const Record& r) {
        if (quotaBytes_ > 0) {
            const uint64_t total = bytesWritten_.load(std::memory_order_relaxed);
            if (total + r.bytes.size() > quotaBytes_) {
                if (!quotaHit_) {
                    quotaHit_ = true;
                    VCS_MEDIA_LOG_WARNING(
                        "[MediaDebug] total quota reached (%lluMB); further records are dropped",
                        static_cast<unsigned long long>(quotaBytes_ >> 20));
                }
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        Sink* sink = GetOrOpenSink(r.kind, r.memberId);
        if (!sink) return;

        // 单文件上限复用 maxFileSizeMB：超过后停止该流，不截断已有数据。
        if (fileLimitBytes_ > 0 && sink->dataBytes + r.bytes.size() > fileLimitBytes_) {
            if (!sink->limitLogged) {
                sink->limitLogged = true;
                VCS_MEDIA_LOG_WARNING("[MediaDebug] file limit reached: %s",
                                      sink->path.c_str());
            }
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        sink->ofs.write(reinterpret_cast<const char*>(r.bytes.data()),
                        static_cast<std::streamsize>(r.bytes.size()));
        if (!sink->ofs.good()) {
            writeErrors_.fetch_add(1, std::memory_order_relaxed);
            sink->ofs.clear();
            return;
        }
        sink->dataBytes += r.bytes.size();
        bytesWritten_.fetch_add(r.bytes.size(), std::memory_order_relaxed);
        recordsWritten_.fetch_add(1, std::memory_order_relaxed);
    }

    void PatchHeaders() {
        for (std::map<std::string, std::unique_ptr<Sink> >::iterator it = sinks_.begin();
             it != sinks_.end(); ++it) {
            Sink* s = it->second.get();
            if (!s) continue;
            if (s->wav && s->dataBytes != s->lastPatched) {
                const uint32_t dataBytes =
                    s->dataBytes > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                 : static_cast<uint32_t>(s->dataBytes);
                s->ofs.seekp(0, std::ios::beg);
                WriteWavHeader(s->ofs, wavRate_, wavChannels_, wavBits_, dataBytes);
                s->ofs.seekp(0, std::ios::end);
                s->lastPatched = s->dataBytes;
            }
            s->ofs.flush();
        }
    }

    void CloseSink(Sink* s) {
        if (!s || !s->ofs.is_open()) return;
        if (s->wav) {
            const uint32_t dataBytes =
                s->dataBytes > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                             : static_cast<uint32_t>(s->dataBytes);
            s->ofs.seekp(0, std::ios::beg);
            WriteWavHeader(s->ofs, wavRate_, wavChannels_, wavBits_, dataBytes);
            s->ofs.seekp(0, std::ios::end);
        }
        s->ofs.flush();
        s->ofs.close();
    }

    // ---- config parsing ----

    void ApplyConfigSection(const std::string& section,
                            const std::map<std::string, std::string>& values) {
        std::lock_guard<std::mutex> lock(configMu_);

        if (section == "debug") {
            for (std::map<std::string, std::string>::const_iterator kv = values.begin();
                 kv != values.end(); ++kv) {
                const std::string& key = kv->first;
                const std::string& value = kv->second;
                if (key == "enabled") {
                    config_.enabled = IsTrue(value);
                } else if (key == "outputdir") {
                    config_.outputDir = value;
                } else if (key == "maxfilesize") {
                    config_.maxFileSizeMB = static_cast<size_t>(std::stoul(value));
                } else if (key == "maxtotalsize") {
                    config_.maxTotalSizeMB = static_cast<size_t>(std::stoul(value));
                } else if (key == "autocleanup") {
                    config_.autoCleanup = IsTrue(value);
                }
            }
        } else if (section == "debug.audio") {
            for (std::map<std::string, std::string>::const_iterator kv = values.begin();
                 kv != values.end(); ++kv) {
                const std::string& key = kv->first;
                const std::string& value = kv->second;
                if (key == "enabled") {
                    config_.audio.enabled = IsTrue(value);
                } else if (key == "saveinput") {
                    config_.audio.saveInput = IsTrue(value);
                } else if (key == "saveoutput") {
                    config_.audio.saveOutput = IsTrue(value);
                } else if (key == "savemixed") {
                    config_.audio.saveMixed = IsTrue(value);
                } else if (key == "savedecoded") {
                    config_.audio.saveDecoded = IsTrue(value);
                } else if (key == "saveencoded") {
                    config_.audio.saveEncoded = IsTrue(value);
                } else if (key == "format") {
                    config_.audio.format = value;
                } else if (key == "samplerate") {
                    config_.audio.sampleRate = std::stoi(value);
                } else if (key == "channels") {
                    config_.audio.channels = std::stoi(value);
                } else if (key == "bitspersample") {
                    config_.audio.bitsPerSample = std::stoi(value);
                }
            }
        } else if (section == "debug.video") {
            for (std::map<std::string, std::string>::const_iterator kv = values.begin();
                 kv != values.end(); ++kv) {
                const std::string& key = kv->first;
                const std::string& value = kv->second;
                if (key == "enabled") {
                    config_.video.enabled = IsTrue(value);
                } else if (key == "saveinput") {
                    config_.video.saveInput = IsTrue(value);
                } else if (key == "saveoutput") {
                    config_.video.saveOutput = IsTrue(value);
                } else if (key == "savecomposed") {
                    config_.video.saveComposed = IsTrue(value);
                } else if (key == "savedecoded") {
                    config_.video.saveDecoded = IsTrue(value);
                } else if (key == "saveencoded") {
                    config_.video.saveEncoded = IsTrue(value);
                } else if (key == "format") {
                    config_.video.format = value;
                }
            }
        } else if (section == "debug.rtp") {
            for (std::map<std::string, std::string>::const_iterator kv = values.begin();
                 kv != values.end(); ++kv) {
                const std::string& key = kv->first;
                const std::string& value = kv->second;
                if (key == "enabled") {
                    config_.rtp.enabled = IsTrue(value);
                } else if (key == "saveincoming") {
                    config_.rtp.saveIncoming = IsTrue(value);
                } else if (key == "saveoutgoing") {
                    config_.rtp.saveOutgoing = IsTrue(value);
                } else if (key == "hexdump") {
                    config_.rtp.hexDump = IsTrue(value);
                }
            }
        }
    }

    static bool IsTrue(const std::string& value) {
        const std::string v = ToLower(value);
        return v == "true" || v == "1" || v == "yes";
    }

    // ---- state ----

    mutable std::mutex configMu_;
    MediaDebugConfig config_;

    // 热路径只读这三个 atomic；配置变更时重算。
    std::atomic<bool> enabled_{false};
    std::atomic<bool> captureOn_{false};
    std::atomic<bool> preEncodeOn_{false};
    std::atomic<bool> encodedOn_{false};

    std::mutex queueMu_;
    std::condition_variable queueCv_;
    std::deque<Record> queue_;
    std::atomic<uint64_t> queued_{0};
    std::atomic<uint64_t> dropped_{0};

    std::mutex lifeMu_;
    std::atomic<bool> writerStarted_{false};
    std::atomic<bool> writerRunning_{false};
    std::thread writerThread_;

    // writer-thread private (only touched by WriterLoop / sink paths)
    std::mutex sinkMu_;
    std::map<std::string, std::unique_ptr<Sink> > sinks_;
    std::set<std::string> failedKeys_;
    size_t totalStreams_ = 0;
    std::string writerDir_;
    uint32_t wavRate_ = 8000;
    uint16_t wavChannels_ = 1;
    uint16_t wavBits_ = 16;
    uint64_t quotaBytes_ = 0;
    uint64_t fileLimitBytes_ = 0;
    bool quotaHit_ = false;

    std::atomic<uint64_t> recordsWritten_{0};
    std::atomic<uint64_t> bytesWritten_{0};
    std::atomic<uint64_t> writeErrors_{0};
};

// ============================================================
// Global singleton
// ============================================================

static MediaDebug g_mediaDebug;

IMediaDebug* GetMediaDebug() {
    return &g_mediaDebug;
}

}} // namespace vcs::media
