#pragma once

#include "IVideoSource.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// 前向声明（避免在头文件里拉 D3D11 / DXGI 头）
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIOutputDuplication;
struct ID3D11Texture2D;
struct SwsContext;

namespace vcs { namespace media { namespace platform { namespace windows {

/**
 * @brief DXGI Desktop Duplication 屏幕捕获
 *
 * 用 IDXGIOutputDuplication 拿桌面帧（BGRA），用 libswscale 转成 YUV420P。
 *
 * 线程模型：
 *   - 第一个 Subscribe 启动采集线程
 *   - 最后一个 Unsubscribe 停止采集线程
 *   - 回调在采集线程执行，订阅者必须快速返回
 *
 * COM 生命周期：
 *   - 采集线程内部 CoInitializeEx(MTA) / CoUninitialize
 *   - D3D11 设备、DXGI duplication、sws context 只在采集线程上创建和销毁
 */
class ScreenCaptureSource : public IVideoSource {
public:
    explicit ScreenCaptureSource(int monitorIndex);
    ~ScreenCaptureSource() override;

    const std::string& GetId() const override { return id_; }

    void Subscribe(const std::string& consumerId, FrameCallback cb) override;
    void Unsubscribe(const std::string& consumerId) override;

    size_t GetSubscriberCount() const override;

private:
    void CaptureThreadFunc();

    // 采集线程内调用
    bool InitDxgi();
    void CleanupDxgi();
    bool ConvertBgraToYuv(const uint8_t* bgra, uint32_t width, uint32_t height,
                          uint32_t bgraStride,
                          std::vector<uint8_t>& yuv);

    int monitorIndex_;
    std::string id_;

    // DXGI / D3D11 对象（只在采集线程访问）
    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D*     stagingTexture_ = nullptr;
    SwsContext*          swsCtx_ = nullptr;
    uint32_t             frameWidth_ = 0;
    uint32_t             frameHeight_ = 0;

    // 订阅者
    mutable std::mutex subsMtx_;
    std::map<std::string, FrameCallback> subscribers_;

    // 采集线程
    std::thread captureThread_;
    std::atomic<bool> running_{false};

    // 启动/停止互斥
    std::mutex startStopMtx_;
};

}}}}