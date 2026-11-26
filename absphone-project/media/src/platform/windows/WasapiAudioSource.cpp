#include "WasapiAudioSource.h"
#include "AudioFrame.h"
#include "PlatformTime.h"
#include "ILogger.h"
#include "IMediaDebug.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>   // PKEY_Device_FriendlyName
#include <ksmedia.h>      // WAVEFORMATEXTENSIBLE / KSDATAFORMAT_SUBTYPE_*

#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")   // PKEY_Device_FriendlyName

namespace vcs { namespace media { namespace platform { namespace windows {

namespace {

// 极简 COM 智能指针
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** Put() { Reset(); return &p_; }
    T*  Get() const { return p_; }
    T*  operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }

private:
    T* p_ = nullptr;
};

// 判断 WAVEFORMATEX 是否为 float 格式
bool IsFloatFormat(const WAVEFORMATEX* fmt) {
    if (!fmt) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
    out.resize(static_cast<size_t>(len - 1));
    return out;
}

// 读取端点的友好名（"麦克风阵列 (Realtek Audio)" 这类），失败返回占位串.
// 排查"是否选错设备"时，名字比索引更能说明问题。
std::string EndpointFriendlyName(IMMDevice* dev) {
    std::string name = "<unknown>";
    if (!dev) return name;
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.Put()))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR) {
            name = WideToUtf8(pv.pwszVal);
        }
        PropVariantClear(&pv);
    }
    return name;
}

} // anonymous namespace

// ============================================================
// Construction / Destruction
// ============================================================
WasapiAudioSource::WasapiAudioSource(int deviceIndex)
    : deviceIndex_(deviceIndex) {
    id_ = deviceIndex < 0 ? std::string("mic:default")
                          : "mic:" + std::to_string(deviceIndex);
    accumBuf_.reserve(targetFrameSamples_ * 4);
    VCS_MEDIA_LOG_INFO("[WasapiAudioSource] created: id=%s", id_.c_str());
}

WasapiAudioSource::~WasapiAudioSource() {
    running_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    VCS_MEDIA_LOG_INFO("[WasapiAudioSource] destroyed: id=%s", id_.c_str());
}

// ============================================================
// Subscribe / Unsubscribe
// ============================================================
void WasapiAudioSource::Subscribe(const std::string& consumerId, FrameCallback cb) {
    if (!cb) return;

    std::lock_guard<std::mutex> lock(startStopMtx_);
    {
        std::lock_guard<std::mutex> subLock(subsMtx_);
        subscribers_[consumerId] = std::move(cb);
    }

    if (!running_.load()) {
        // 上一轮线程可能因设备打开失败自行退出：先回收，再赋新线程，
        // 否则给 joinable 的 thread 赋值会 std::terminate。
        if (captureThread_.joinable()) {
            captureThread_.join();
        }
        running_ = true;
        captureThread_ = std::thread(&WasapiAudioSource::CaptureThreadFunc, this);
        VCS_MEDIA_LOG_INFO("[WasapiAudioSource] capture thread started: id=%s, consumer=%s",
                           id_.c_str(), consumerId.c_str());
    }
}

void WasapiAudioSource::Unsubscribe(const std::string& consumerId) {
    bool empty = false;
    {
        std::lock_guard<std::mutex> subLock(subsMtx_);
        subscribers_.erase(consumerId);
        empty = subscribers_.empty();
    }

    if (empty) {
        std::lock_guard<std::mutex> lock(startStopMtx_);
        running_ = false;
        // 以 joinable 而不是 running_ 为准：打开失败的线程自己会把 running_
        // 置 false 并退出，但仍然需要 join 回收。
        if (captureThread_.joinable()) {
            captureThread_.join();
            VCS_MEDIA_LOG_INFO("[WasapiAudioSource] capture thread stopped: id=%s",
                               id_.c_str());
        }
    }
}

size_t WasapiAudioSource::GetSubscriberCount() const {
    std::lock_guard<std::mutex> lock(subsMtx_);
    return subscribers_.size();
}

// ============================================================
// InitWasapi
// ============================================================
bool WasapiAudioSource::InitWasapi() {
    // 1. 创建设备枚举器
    ComPtr<IMMDeviceEnumerator> enumTmp;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   (void**)enumTmp.Put());
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] CoCreateInstance(MMDeviceEnumerator) failed: 0x%08X",
                            static_cast<unsigned>(hr));
        return false;
    }
    enumerator_ = enumTmp.Get();
    enumerator_->AddRef();

    // 2. 枚举麦克风设备
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                          collection.Put());
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] EnumAudioEndpoints failed");
        CleanupWasapi();
        return false;
    }

    UINT count = 0;
    collection->GetCount(&count);

    // 把当前活动采集端点全部列出来：静音排查的第一步是确认 mic:0
    // 到底指向哪个设备（虚拟声卡/立体声混音会一直输出数字静音）。
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> devTmp;
        if (FAILED(collection->Item(i, devTmp.Put()))) continue;
        VCS_MEDIA_LOG_INFO("[WasapiAudioSource] capture device[%u]%s: %s",
                           i,
                           (static_cast<int>(i) == deviceIndex_) ? " (selected)" : "",
                           EndpointFriendlyName(devTmp.Get()).c_str());
    }

    // mic:N 走的是"枚举位置"，而用户在系统里设的默认麦克风可能是另一个
    // 端点。把两个默认角色也打出来，一次运行就能判断 mic:0 是不是用户
    // 以为的那个设备；一致时再看端点静音/音量。mic:default 的选中结果
    // 下面会打印，这里不重复。
    if (deviceIndex_ >= 0) {
        const ERole roles[] = { eCommunications, eConsole };
        const char* roleName[] = { "communications", "console" };
        for (int r = 0; r < 2; ++r) {
            ComPtr<IMMDevice> defDev;
            if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eCapture, roles[r],
                                                               defDev.Put()))) {
                VCS_MEDIA_LOG_INFO("[WasapiAudioSource] default capture (%s): %s",
                                   roleName[r],
                                   EndpointFriendlyName(defDev.Get()).c_str());
            } else {
                VCS_MEDIA_LOG_INFO("[WasapiAudioSource] default capture (%s): <none>",
                                   roleName[r]);
            }
        }
    }

    ComPtr<IMMDevice> devTmp;
    if (deviceIndex_ < 0) {
        // mic:default：按系统设置的默认麦克风打开。先"通信"角色，再退
        // "控制台"角色；两者都失败就报错返回，绝不退回枚举 0 位 —— 0 位
        // 可能是立体声混音这类环回端点，会一直输出数字静音。
        const ERole roles[] = { eCommunications, eConsole };
        const char* roleName[] = { "communications", "console" };
        for (int r = 0; r < 2 && !devTmp; ++r) {
            if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eCapture, roles[r],
                                                               devTmp.Put()))) {
                VCS_MEDIA_LOG_INFO(
                    "[WasapiAudioSource] %s -> default capture (%s): %s",
                    id_.c_str(), roleName[r],
                    EndpointFriendlyName(devTmp.Get()).c_str());
            }
        }
        if (!devTmp) {
            VCS_MEDIA_LOG_ERROR(
                "[WasapiAudioSource] %s: no default capture endpoint", id_.c_str());
            CleanupWasapi();
            return false;
        }
    } else {
        if (static_cast<UINT>(deviceIndex_) >= count) {
            VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] mic index=%d not found (count=%u)",
                                deviceIndex_, count);
            CleanupWasapi();
            return false;
        }
        hr = collection->Item(static_cast<UINT>(deviceIndex_), devTmp.Put());
        if (FAILED(hr)) {
            VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] collection->Item(%d) failed",
                                deviceIndex_);
            CleanupWasapi();
            return false;
        }
    }
    device_ = devTmp.Get();
    device_->AddRef();

    // 3. 激活 IAudioClient
    ComPtr<IAudioClient> clientTmp;
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           (void**)clientTmp.Put());
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] Activate(IAudioClient) failed: 0x%08X",
                            static_cast<unsigned>(hr));
        CleanupWasapi();
        return false;
    }
    audioClient_ = clientTmp.Get();
    audioClient_->AddRef();

    // 4. 拿设备原生格式
    WAVEFORMATEX* mixFmt = nullptr;
    hr = audioClient_->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] GetMixFormat failed");
        CleanupWasapi();
        return false;
    }
    mixFormat_ = mixFmt;

    nativeSampleRate_    = mixFormat_->nSamplesPerSec;
    nativeChannels_      = static_cast<uint8_t>(mixFormat_->nChannels);
    nativeBitsPerSample_ = mixFormat_->wBitsPerSample;
    nativeIsFloat_       = IsFloatFormat(mixFormat_);

    VCS_MEDIA_LOG_INFO("[WasapiAudioSource] native format: %uHz, %uch, %ubit, %s",
                       nativeSampleRate_, nativeChannels_, nativeBitsPerSample_,
                       nativeIsFloat_ ? "float" : "int");

    // 5. Initialize：shared mode，buffer 时长 100ms（WASAPI 要求 >= 10ms，
    //    100ms 给采集线程足够调度余地）
    const REFERENCE_TIME bufferDuration = 100 * 10000;   // 100ms in 100ns units
    hr = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        bufferDuration,
        0,
        mixFormat_,
        nullptr);
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] IAudioClient::Initialize failed: 0x%08X",
                            static_cast<unsigned>(hr));
        CleanupWasapi();
        return false;
    }

    // 6. 拿 IAudioCaptureClient
    ComPtr<IAudioCaptureClient> capTmp;
    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                    (void**)capTmp.Put());
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] GetService(IAudioCaptureClient) failed");
        CleanupWasapi();
        return false;
    }
    captureClient_ = capTmp.Get();
    captureClient_->AddRef();

    // 7. 端点音量/静音（只读诊断）：共享模式下端点被静音或音量为 0 时，
    //    采集照样成功，但拿到的全是 0 —— 必须在开会前就能看见。
    ComPtr<IAudioEndpointVolume> volTmp;
    hr = device_->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                           nullptr, (void**)volTmp.Put());
    if (SUCCEEDED(hr)) {
        endpointVolume_ = volTmp.Get();
        endpointVolume_->AddRef();
        LogEndpointState("open");
    } else {
        VCS_MEDIA_LOG_WARNING("[WasapiAudioSource] Activate(IAudioEndpointVolume) failed: 0x%08X",
                              static_cast<unsigned>(hr));
    }

    // 8. 启动
    hr = audioClient_->Start();
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] IAudioClient::Start failed");
        CleanupWasapi();
        return false;
    }

    VCS_MEDIA_LOG_INFO("[WasapiAudioSource] opened: id=%s, target=%uHz/%uch/%ums",
                       id_.c_str(), targetSampleRate_, targetChannels_, targetFrameMs_);
    return true;
}

void WasapiAudioSource::CleanupWasapi() {
    if (audioClient_) {
        audioClient_->Stop();
    }
    if (endpointVolume_) { endpointVolume_->Release(); endpointVolume_ = nullptr; }
    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (audioClient_)   { audioClient_->Release();   audioClient_   = nullptr; }
    if (device_)        { device_->Release();        device_        = nullptr; }
    if (enumerator_)    { enumerator_->Release();    enumerator_    = nullptr; }
    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
    accumBuf_.clear();
    int16MonoBuf_.clear();
    resampledBuf_.clear();
}

// ============================================================
// 端点状态 / 采集幅度诊断（每个采集源 1 Hz 一行）
// ============================================================
void WasapiAudioSource::LogEndpointState(const char* where) {
    if (!endpointVolume_) return;

    BOOL muted = FALSE;
    if (FAILED(endpointVolume_->GetMute(&muted))) return;
    float scalar = 0.0f;
    if (FAILED(endpointVolume_->GetMasterVolumeLevelScalar(&scalar))) return;

    endpointMuted_ = (muted != FALSE);
    endpointVolumeScalar_ = scalar;

    if (endpointMuted_ || scalar <= 0.0f) {
        VCS_MEDIA_LOG_WARNING(
            "[WasapiAudioSource] endpoint is SILENT (%s): id=%s, muted=%d, volume=%.2f",
            where, id_.c_str(), (int)endpointMuted_, scalar);
    } else {
        VCS_MEDIA_LOG_INFO(
            "[WasapiAudioSource] endpoint level (%s): id=%s, muted=%d, volume=%.2f",
            where, id_.c_str(), (int)endpointMuted_, scalar);
    }
}

void WasapiAudioSource::MaybeLogDiagnostics() {
    const int64_t nowMs = static_cast<int64_t>(vcs::base::platform::UtcNowEpochMs());
    if (diagLastLogMs_ == 0) {
        diagLastLogMs_ = nowMs;
        return;
    }
    if (nowMs - diagLastLogMs_ < 1000) return;
    diagLastLogMs_ = nowMs;

    // 刷新端点状态（静音/音量可能在通话中被用户改掉），只更新数值，
    // 不额外刷日志 —— 下面这行已经带上 muted/vol。
    if (endpointVolume_) {
        BOOL muted = FALSE;
        if (SUCCEEDED(endpointVolume_->GetMute(&muted))) {
            endpointMuted_ = (muted != FALSE);
        }
        float scalar = 0.0f;
        if (SUCCEEDED(endpointVolume_->GetMasterVolumeLevelScalar(&scalar))) {
            endpointVolumeScalar_ = scalar;
        }
    }

    // 每秒窗口统计：正常应为 packets≈50（10ms/包）、flushedFrames≈50、
    // 编码侧对应 frames≈50。peak8k=0 且 zeroFrames==flushedFrames
    // 表示"采集成功但拿到的是数字静音"。
    VCS_MEDIA_LOG_INFO(
        "[WasapiAudioSource] capture diag: id=%s, packets=%llu, silentFlagPkts=%llu, "
        "nativeFrames=%llu, flushedFrames=%llu, zeroFrames=%llu, peak8k=%d, muted=%d, vol=%.2f",
        id_.c_str(),
        static_cast<unsigned long long>(diagPackets_),
        static_cast<unsigned long long>(diagSilentFlagPackets_),
        static_cast<unsigned long long>(diagNativeFrames_),
        static_cast<unsigned long long>(diagFlushedFrames_),
        static_cast<unsigned long long>(diagZeroFrames_),
        diagPeak8k_, (int)endpointMuted_, endpointVolumeScalar_);

    // 只有真的收到过采集数据还全零时才告警；刚启动或设备暂时无数据
    // （packets=0）不算。
    if (diagPeak8k_ == 0 && (diagPackets_ > 0 || diagFlushedFrames_ > 0)) {
        VCS_MEDIA_LOG_WARNING(
            "[WasapiAudioSource] captured audio is ALL ZERO: id=%s, silentFlagPkts=%llu, "
            "muted=%d, vol=%.2f (check endpoint mute/volume, Windows microphone privacy, "
            "or wrong device index)",
            id_.c_str(), static_cast<unsigned long long>(diagSilentFlagPackets_),
            (int)endpointMuted_, endpointVolumeScalar_);
    }

    diagPackets_ = 0;
    diagSilentFlagPackets_ = 0;
    diagNativeFrames_ = 0;
    diagFlushedFrames_ = 0;
    diagZeroFrames_ = 0;
    diagPeak8k_ = 0;
}

// ============================================================
// ConvertToInt16Mono
//
// 把原生格式（int16 / float32，1ch 或 Nch）转成 int16 mono
// ============================================================
void WasapiAudioSource::ConvertToInt16Mono(const uint8_t* data, uint32_t  numFrames,
                                            std::vector<int16_t>& out) {
    out.resize(numFrames);
    const uint8_t ch = (nativeChannels_ > 0) ? nativeChannels_ : 1;

    if (nativeIsFloat_) {
        const float* src = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < numFrames; ++i) {
            float sum = 0.0f;
            for (uint8_t c = 0; c < ch; ++c) {
                sum += src[i * ch + c];
            }
            float mono = sum / ch;
            // clamp [-1, 1] 然后转 int16
            if (mono > 1.0f) mono = 1.0f;
            else if (mono < -1.0f) mono = -1.0f;
            out[i] = static_cast<int16_t>(mono * 32767.0f);
        }
    } else {
        // 假设 int16
        const int16_t* src = reinterpret_cast<const int16_t*>(data);
        for (UINT32 i = 0; i < numFrames; ++i) {
            int32_t sum = 0;
            for (uint8_t c = 0; c < ch; ++c) {
                sum += src[i * ch + c];
            }
            out[i] = static_cast<int16_t>(sum / ch);
        }
    }
}

// ============================================================
// Resample（box filter）
//
// 输入：int16 单声道 @ inRate
// 输出：int16 单声道 @ targetSampleRate_
//
// box filter：每个输出样本取对应输入区间的平均。任意比例都能处理，
// 对 G.711 语音质量足够。
// ============================================================
void WasapiAudioSource::Resample(const std::vector<int16_t>& in, uint32_t inRate,
                                  std::vector<int16_t>& out) {
    if (in.empty() || inRate == 0) {
        out.clear();
        return;
    }
    if (inRate == targetSampleRate_) {
        out = in;
        return;
    }

    const double ratio = static_cast<double>(inRate) / targetSampleRate_;
    const size_t outCount = static_cast<size_t>(
        std::ceil(static_cast<double>(in.size()) / ratio));
    out.resize(outCount);

    const size_t inCount = in.size();
    for (size_t o = 0; o < outCount; ++o) {
        const double start = o * ratio;
        const double end   = (o + 1) * ratio;
        const size_t iStart = static_cast<size_t>(start);
        size_t iEnd = static_cast<size_t>(std::ceil(end));
        if (iEnd > inCount) iEnd = inCount;
        if (iStart >= inCount) {
            out[o] = 0;
            continue;
        }
        if (iEnd <= iStart) {
            out[o] = in[iStart];
            continue;
        }
        int64_t sum = 0;
        for (size_t i = iStart; i < iEnd; ++i) {
            sum += in[i];
        }
        out[o] = static_cast<int16_t>(sum / static_cast<int64_t>(iEnd - iStart));
    }
}

// ============================================================
// ProcessNativeFrames
//
// 1. 原生格式 -> int16 mono
// 2. 重采样到 8000Hz
// 3. 累积到 accumBuf_
// 4. 累积够一帧（targetFrameSamples_）就 FlushAccumulation
// ============================================================
void WasapiAudioSource::ProcessNativeFrames(const uint8_t* data, uint32_t numFrames, uint32_t flags) {
    if (numFrames == 0) return;

    ++diagPackets_;
    diagNativeFrames_ += numFrames;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ++diagSilentFlagPackets_;

    // 静音帧：填 0，但还是要累积（保持帧节奏）
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        int16MonoBuf_.assign(numFrames, 0);
    } else {
        ConvertToInt16Mono(data, numFrames, int16MonoBuf_);
    }

    // 重采样
    Resample(int16MonoBuf_, nativeSampleRate_, resampledBuf_);

    // 目标采样率下的峰值（0 表示这一秒采到的就是数字静音）
    for (size_t i = 0; i < resampledBuf_.size(); ++i) {
        const int32_t v = resampledBuf_[i] < 0
            ? -static_cast<int32_t>(resampledBuf_[i])
            : static_cast<int32_t>(resampledBuf_[i]);
        if (v > diagPeak8k_) diagPeak8k_ = v;
    }

    // 累积
    accumBuf_.insert(accumBuf_.end(), resampledBuf_.begin(), resampledBuf_.end());

    // 凑够一帧就回调
    while (accumBuf_.size() >= targetFrameSamples_) {
        FlushAccumulation();
    }
}

// ============================================================
// FlushAccumulation
//
// 从 accumBuf_ 里取 targetFrameSamples_ 个样本，构造 AudioFrame，广播
// ============================================================
void WasapiAudioSource::FlushAccumulation() {
    if (accumBuf_.size() < targetFrameSamples_) return;

    auto frameData = std::make_shared<std::vector<int16_t>>(
        accumBuf_.begin(),
        accumBuf_.begin() + static_cast<std::ptrdiff_t>(targetFrameSamples_));

    // 从 accumBuf_ 删除已消费的部分
    accumBuf_.erase(accumBuf_.begin(),
                    accumBuf_.begin() + static_cast<std::ptrdiff_t>(targetFrameSamples_));

    auto frame = std::make_shared<AudioFrame>();
    frame->data       = frameData;
    frame->samples    = targetFrameSamples_;
    frame->sampleRate = targetSampleRate_;
    frame->channels   = targetChannels_;
    frame->timestamp  = vcs::base::platform::UtcNowEpochMs() * 1000;  // us

    ++diagFlushedFrames_;
    bool allZero = true;
    for (size_t i = 0; i < frameData->size(); ++i) {
        if ((*frameData)[i] != 0) { allZero = false; break; }
    }
    if (allZero) ++diagZeroFrames_;

    // 调试旁路：落"重采样后进入编码队列的 8k/1ch PCM"，与 TX 侧
    // tx_preencode 对照即可把静音夹在采集还是增益/编码之间。
    if (IMediaDebug* debug = GetMediaDebug()) {
        debug->SaveAudioInput(
            id_,
            reinterpret_cast<const uint8_t*>(frameData->data()),
            frameData->size() * sizeof(int16_t));
    }

    // 广播（锁外）
    AudioFramePtr constFrame = frame;
    std::vector<FrameCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(subsMtx_);
        cbs.reserve(subscribers_.size());
        for (auto& kv : subscribers_) cbs.push_back(kv.second);
    }
    for (auto& cb : cbs) {
        try {
            cb(constFrame);
        } catch (const std::exception& e) {
            VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] subscriber threw: %s", e.what());
        }
    }
}

// ============================================================
// CaptureThreadFunc
// ============================================================
void WasapiAudioSource::CaptureThreadFunc() {
    // COM 初始化（MTA）
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOk = SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE;

    VCS_MEDIA_LOG_INFO("[WasapiAudioSource] CaptureThreadFunc enter: id=%s",
                       id_.c_str());

    if (!InitWasapi()) {
        VCS_MEDIA_LOG_ERROR("[WasapiAudioSource] InitWasapi failed: id=%s",
                            id_.c_str());
        running_ = false;
        if (comOk && SUCCEEDED(comHr)) CoUninitialize();
        return;
    }

    while (running_.load(std::memory_order_acquire)) {
        if (!captureClient_) break;

        // 1 Hz 采集诊断（放在循环顶部：即使 GetBuffer 一直返回 0 包，
        // 也能看到"没有采集数据"这个事实）。
        MaybeLogDiagnostics();

        // 查询当前有多少帧可读
        uint32_t packetSize = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetSize);
        if (FAILED(hr)) {
            if (running_.load()) {
                VCS_MEDIA_LOG_WARNING("[WasapiAudioSource] GetNextPacketSize failed: 0x%08X",
                                      static_cast<unsigned>(hr));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        if (packetSize == 0) {
            // 没有数据，等下一批
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 把当前所有 packet 全部读完
        while (packetSize > 0 && running_.load()) {
            uint8_t* data = nullptr;
            uint32_t numFrames = 0;
			DWORD flags = 0;

            hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                VCS_MEDIA_LOG_WARNING("[WasapiAudioSource] GetBuffer failed: 0x%08X",
                                      static_cast<unsigned>(hr));
                break;
            }

            if (numFrames > 0) {
                ProcessNativeFrames(data, numFrames, flags);
            }

            captureClient_->ReleaseBuffer(numFrames);

            hr = captureClient_->GetNextPacketSize(&packetSize);
            if (FAILED(hr)) break;
        }
    }

    CleanupWasapi();

    if (comOk && SUCCEEDED(comHr)) {
        CoUninitialize();
    }

    VCS_MEDIA_LOG_INFO("[WasapiAudioSource] CaptureThreadFunc exit: id=%s",
                       id_.c_str());
    running_ = false;
}

}}}}