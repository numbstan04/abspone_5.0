#pragma once

#include "IVideoSink.h"
#include "MediaConfig.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 前向声明
struct SwsContext;

namespace vcs { namespace media { namespace platform { namespace windows {

/**
 * @brief GDI 视频渲染目标
 *
 * 用 GDI StretchBlt 把 YUV420P 渲染到 HWND。
 *
 * 渲染流程：
 *   YUV420P → sws_scale → BGRA → GDI DIB section → StretchBlt 到 HWND
 *
 * 双缓冲：
 *   - 一个离屏 DIB section 作为绘制缓冲
 *   - 每帧绘制到离屏，再 BitBlt 到窗口
 *   - 避免闪烁
 *
 * 线程：
 *   - Render 在流的 decodeThread 里调用（VideoProcessor::DecodeLoop）
 *   - BindStream / UnbindStream 由 VideoProcessor::Start / Stop 调用
 *   - 内部 mutex 保护 boundStreamId_ 和 DIB 状态
 */
class GdiVideoSink : public IVideoSink {
public:
    explicit GdiVideoSink(const vcs::media_config::VideoSinkConfig& config);
    ~GdiVideoSink() override;

    const std::string& GetId() const override { return id_; }

    bool BindStream(const std::string& streamId) override;
    void UnbindStream() override;
    bool Render(std::shared_ptr<const VideoFrame> frame) override;
    const std::string& GetBoundStreamId() const override;

private:
    bool EnsureBackBuffer(uint32_t width, uint32_t height);
    void ReleaseBackBuffer();
    bool ConvertYuvToBgra(const uint8_t* yuv, size_t yuvSize,
                          uint32_t width, uint32_t height,
                          std::vector<uint8_t>& bgra);

    std::string id_;

    // 渲染目标（HWND）
    void* nativeWindow_ = nullptr;   // 存 HWND，不用 windows.h 类型避免头文件污染
    uint32_t targetWidth_  = 1280;
    uint32_t targetHeight_ = 720;

    // 绑定的流
    mutable std::mutex mutex_;
    std::string boundStreamId_;

    // 离屏 BGRA DIB
    void*  dibSection_ = nullptr;    // HBITMAP
    void*  dibMemDC_   = nullptr;    // HDC
    void*  dibBits_    = nullptr;    // BGRA 像素内存
    uint32_t dibWidth_  = 0;
    uint32_t dibHeight_ = 0;

    // sws 上下文
    SwsContext* swsCtx_ = nullptr;
    uint32_t    swsSrcWidth_  = 0;
    uint32_t    swsSrcHeight_ = 0;

    bool firstFrameLogged_ = false;  // Render 只在首帧打 Info
};

}}}}