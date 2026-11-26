#include "ScreenCaptureSource.h"
#include "PlatformTime.h"
#include "ILogger.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>   // Microsoft::WRL::ComPtr（D3D11 需要，比手写 ComPtr 更可靠）

extern "C" {
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace vcs { namespace media { namespace platform { namespace windows {

ScreenCaptureSource::ScreenCaptureSource(int monitorIndex)
    : monitorIndex_(monitorIndex) {
    id_ = "screen:" + std::to_string(monitorIndex);
    VCS_MEDIA_LOG_INFO("[ScreenCaptureSource] created: id=%s", id_.c_str());
}

ScreenCaptureSource::~ScreenCaptureSource() {
    running_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    VCS_MEDIA_LOG_INFO("[ScreenCaptureSource] destroyed: id=%s", id_.c_str());
}

// ============================================================
// Subscribe / Unsubscribe
// ============================================================
void ScreenCaptureSource::Subscribe(const std::string& consumerId, FrameCallback cb) {
    if (!cb) return;

    std::lock_guard<std::mutex> lock(startStopMtx_);
    {
        std::lock_guard<std::mutex> subLock(subsMtx_);
        subscribers_[consumerId] = std::move(cb);
    }

    if (!running_.load()) {
        // 上一轮线程可能因初始化失败自行退出：先回收，再赋新线程，
        // 否则给 joinable 的 thread 赋值会 std::terminate。
        if (captureThread_.joinable()) {
            captureThread_.join();
        }
        running_ = true;
        captureThread_ = std::thread(&ScreenCaptureSource::CaptureThreadFunc, this);
        VCS_MEDIA_LOG_INFO("[ScreenCaptureSource] capture thread started: id=%s, consumer=%s",
                           id_.c_str(), consumerId.c_str());
    }
}

void ScreenCaptureSource::Unsubscribe(const std::string& consumerId) {
    bool empty = false;
    {
        std::lock_guard<std::mutex> subLock(subsMtx_);
        subscribers_.erase(consumerId);
        empty = subscribers_.empty();
    }

    if (empty) {
        std::lock_guard<std::mutex> lock(startStopMtx_);
        running_ = false;
        // 以 joinable 而不是 running_ 为准：初始化失败的线程自己会把 running_
        // 置 false 并退出，但仍然需要 join 回收。
        if (captureThread_.joinable()) {
            captureThread_.join();
            VCS_MEDIA_LOG_INFO("[ScreenCaptureSource] capture thread stopped: id=%s",
                               id_.c_str());
        }
    }
}

size_t ScreenCaptureSource::GetSubscriberCount() const {
    std::lock_guard<std::mutex> lock(subsMtx_);
    return subscribers_.size();
}

// ============================================================
// InitDxgi
//
// 1. D3D11CreateDevice
// 2. 从 device 拿到 IDXGIDevice → IDXGIAdapter → EnumOutputs(monitorIndex)
// 3. IDXGIOutput1::DuplicateOutput 拿 duplication
// ============================================================
bool ScreenCaptureSource::InitDxgi() {
    // 1. 创建 D3D11 设备
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, &featureLevel, 1, D3D11_SDK_VERSION,
        &device_, nullptr, &context_);

    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] D3D11CreateDevice failed: 0x%08X",
                            static_cast<unsigned>(hr));
        return false;
    }

    // 2. 拿 DXGI adapter 和 output
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_->QueryInterface(__uuidof(IDXGIDevice),
                                 reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] QueryInterface(IDXGIDevice) failed");
        CleanupDxgi();
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] GetAdapter failed");
        CleanupDxgi();
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(static_cast<UINT>(monitorIndex_),
                              output.GetAddressOf());
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] EnumOutputs(%d) failed",
                            monitorIndex_);
        CleanupDxgi();
        return false;
    }

    // 3. DuplicateOutput
    ComPtr<IDXGIOutput1> output1;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1),
                                reinterpret_cast<void**>(output1.GetAddressOf()));
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] QueryInterface(IDXGIOutput1) failed");
        CleanupDxgi();
        return false;
    }

    hr = output1->DuplicateOutput(device_, &duplication_);
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] DuplicateOutput failed: 0x%08X",
                            static_cast<unsigned>(hr));
        CleanupDxgi();
        return false;
    }

    // 4. 记录分辨率（从 output desc 拿）
    DXGI_OUTPUT_DESC desc = {};
    if (SUCCEEDED(output->GetDesc(&desc))) {
        frameWidth_  = static_cast<uint32_t>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
        frameHeight_ = static_cast<uint32_t>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
    }

    if (frameWidth_ == 0 || frameHeight_ == 0) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] invalid frame size");
        CleanupDxgi();
        return false;
    }

    VCS_MEDIA_LOG_INFO("[ScreenCaptureSource] opened: id=%s, %ux%u",
                       id_.c_str(), frameWidth_, frameHeight_);
    return true;
}

void ScreenCaptureSource::CleanupDxgi() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (stagingTexture_) { stagingTexture_->Release(); stagingTexture_ = nullptr; }
    if (duplication_)    { duplication_->Release();    duplication_ = nullptr; }
    if (context_)        { context_->Release();        context_ = nullptr; }
    if (device_)         { device_->Release();         device_ = nullptr; }
    frameWidth_ = 0;
    frameHeight_ = 0;
}

// ============================================================
// ConvertBgraToYuv
//
// 用 sws_scale 把 BGRA 转成 YUV420P。
// swsCtx_ 按分辨率缓存，分辨率变化时重建。
// ============================================================
bool ScreenCaptureSource::ConvertBgraToYuv(const uint8_t* bgra,
                                            uint32_t width, uint32_t height,
                                            uint32_t bgraStride,
                                            std::vector<uint8_t>& yuv) {
    if (!bgra || width == 0 || height == 0) return false;

    // swsCtx_ 按分辨率缓存
    if (!swsCtx_) {
        swsCtx_ = sws_getContext(
            static_cast<int>(width), static_cast<int>(height),
            AV_PIX_FMT_BGRA,
            static_cast<int>(width), static_cast<int>(height),
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx_) {
            VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] sws_getContext failed");
            return false;
        }
    }

    // 准备源图像
    const uint8_t* srcData[4] = { bgra, nullptr, nullptr, nullptr };
    int srcLinesize[4] = { static_cast<int>(bgraStride), 0, 0, 0 };

    // 准备目标图像（YUV420P，紧凑排列）
    const size_t ySize  = static_cast<size_t>(width) * height;
    const size_t uvSize = ySize / 4;

    if (yuv.size() != ySize + uvSize * 2) {
        yuv.resize(ySize + uvSize * 2);
    }

    uint8_t* dstData[4] = {
        yuv.data(),
        yuv.data() + ySize,
        yuv.data() + ySize + uvSize,
        nullptr
    };
    int dstLinesize[4] = {
        static_cast<int>(width),
        static_cast<int>(width / 2),
        static_cast<int>(width / 2),
        0
    };

    int scaled = sws_scale(swsCtx_, srcData, srcLinesize, 0,
                           static_cast<int>(height),
                           dstData, dstLinesize);
    if (scaled != static_cast<int>(height)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] sws_scale returned %d, expected %u",
                            scaled, height);
        return false;
    }

    return true;
}

// ============================================================
// CaptureThreadFunc
// ============================================================
void ScreenCaptureSource::CaptureThreadFunc() {
    // COM 初始化（MTA）
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOk = SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE;

    VCS_MEDIA_LOG_INFO("[ScreenCaptureSource] CaptureThreadFunc enter: id=%s",
                       id_.c_str());

    if (!InitDxgi()) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] InitDxgi failed: id=%s",
                            id_.c_str());
        running_ = false;
        if (comOk && SUCCEEDED(comHr)) CoUninitialize();
        return;
    }

    const uint32_t width  = frameWidth_;
    const uint32_t height = frameHeight_;

    // 创建 staging texture（CPU 可读）
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width  = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = device_->CreateTexture2D(&texDesc, nullptr, &stagingTexture_);
    if (FAILED(hr)) {
        VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] CreateTexture2D(staging) failed: 0x%08X",
                            static_cast<unsigned>(hr));
        CleanupDxgi();
        running_ = false;
        if (comOk && SUCCEEDED(comHr)) CoUninitialize();
        return;
    }

    const uint32_t bgraStride = width * 4;

    while (running_.load(std::memory_order_acquire)) {
        if (!duplication_) break;

        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        ComPtr<IDXGIResource> desktopResource;

        hr = duplication_->AcquireNextFrame(16,   // 16ms 超时
                                             &frameInfo,
                                             desktopResource.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // 屏幕无变化，继续等
            continue;
        }
        if (FAILED(hr)) {
            if (running_.load()) {
                VCS_MEDIA_LOG_WARNING("[ScreenCaptureSource] AcquireNextFrame failed: 0x%08X",
                                      static_cast<unsigned>(hr));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        // 从 IDXGIResource 拿 ID3D11Texture2D
        ComPtr<ID3D11Texture2D> desktopTexture;
        hr = desktopResource.As(&desktopTexture);
        if (SUCCEEDED(hr)) {
            // 拷贝到 staging texture
            context_->CopyResource(stagingTexture_, desktopTexture.Get());

            // Map staging texture
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            hr = context_->Map(stagingTexture_, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                // 构造 VideoFrame
                auto yuv = std::make_shared<std::vector<uint8_t>>();
                if (ConvertBgraToYuv(static_cast<const uint8_t*>(mapped.pData),
                                     width, height, mapped.RowPitch, *yuv)) {
                    auto frame = std::make_shared<VideoFrame>();
                    frame->data = yuv;
                    frame->dataSize = yuv->size();
                    frame->width = width;
                    frame->height = height;
                    frame->timestamp = vcs::base::platform::UtcNowEpochMs() * 1000;  // us
                    frame->pixelFormat = VideoFrame::PixelFormat::YUV420P;
                    frame->memberId = id_;

                    // 广播
                    std::shared_ptr<const VideoFrame> constFrame = frame;
                    std::vector<FrameCallback> cbs;
                    {
                        std::lock_guard<std::mutex> lock(subsMtx_);
                        cbs.reserve(subscribers_.size());
                        for (auto& kv : subscribers_) cbs.push_back(kv.second);
                    }
                    for (auto& cb : cbs) {
                        try { cb(constFrame); }
                        catch (const std::exception& e) {
                            VCS_MEDIA_LOG_ERROR("[ScreenCaptureSource] subscriber threw: %s",
                                                e.what());
                        }
                    }
                }
                context_->Unmap(stagingTexture_, 0);
            }
        }

        duplication_->ReleaseFrame();
    }

    CleanupDxgi();

    if (comOk && SUCCEEDED(comHr)) {
        CoUninitialize();
    }

    VCS_MEDIA_LOG_INFO("[ScreenCaptureSource] CaptureThreadFunc exit: id=%s",
                       id_.c_str());
    running_ = false;
}

}}}}