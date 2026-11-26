#include "GdiVideoSink.h"
#include "ILogger.h"

#include <windows.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <cstring>
#include <vector>

namespace vcs { namespace media { namespace platform { namespace windows {

GdiVideoSink::GdiVideoSink(const vcs::media_config::VideoSinkConfig& config)
    : nativeWindow_(config.nativeWindow),
      targetWidth_(config.width  > 0 ? config.width  : 1280),
      targetHeight_(config.height > 0 ? config.height : 720) {
    id_ = "gdi_sink";
    VCS_MEDIA_LOG_INFO("[GdiVideoSink] created: hwnd=%p, target=%ux%u",
                       nativeWindow_, targetWidth_, targetHeight_);
}

GdiVideoSink::~GdiVideoSink() {
    ReleaseBackBuffer();
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    VCS_MEDIA_LOG_INFO("[GdiVideoSink] destroyed");
}

// ============================================================
// BindStream / UnbindStream / GetBoundStreamId
// ============================================================
bool GdiVideoSink::BindStream(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!boundStreamId_.empty() && boundStreamId_ != streamId) {
        VCS_MEDIA_LOG_WARNING("[GdiVideoSink] BindStream: already bound to '%s', "
                              "rejecting '%s'",
                              boundStreamId_.c_str(), streamId.c_str());
        return false;
    }
    boundStreamId_ = streamId;
    VCS_MEDIA_LOG_INFO("[GdiVideoSink] BindStream: streamId=%s", streamId.c_str());
    return true;
}

void GdiVideoSink::UnbindStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!boundStreamId_.empty()) {
        VCS_MEDIA_LOG_INFO("[GdiVideoSink] UnbindStream: was '%s'",
                           boundStreamId_.c_str());
        boundStreamId_.clear();
    }
}

const std::string& GdiVideoSink::GetBoundStreamId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return boundStreamId_;
}

// ============================================================
// EnsureBackBuffer
//
// 按 width x height 创建 32-bit BGRA DIB section + memory DC
// 尺寸不变则复用
// ============================================================
bool GdiVideoSink::EnsureBackBuffer(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;

    if (dibWidth_ == width && dibHeight_ == height &&
        dibSection_ && dibMemDC_ && dibBits_) {
        return true;
    }

    ReleaseBackBuffer();

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return false;

    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(height);   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

    ReleaseDC(nullptr, screenDC);

    if (!dib || !bits || !memDC) {
        if (dib) DeleteObject(dib);
        if (memDC) DeleteDC(memDC);
        VCS_MEDIA_LOG_ERROR("[GdiVideoSink] EnsureBackBuffer: CreateDIBSection failed "
                            "(%ux%u)", width, height);
        return false;
    }

    SelectObject(memDC, dib);

    dibSection_ = dib;
    dibMemDC_   = memDC;
    dibBits_    = bits;
    dibWidth_   = width;
    dibHeight_  = height;

    VCS_MEDIA_LOG_INFO("[GdiVideoSink] back buffer created: %ux%u", width, height);
    return true;
}

void GdiVideoSink::ReleaseBackBuffer() {
    if (dibMemDC_) {
        DeleteDC(static_cast<HDC>(dibMemDC_));
        dibMemDC_ = nullptr;
    }
    if (dibSection_) {
        DeleteObject(static_cast<HBITMAP>(dibSection_));
        dibSection_ = nullptr;
    }
    dibBits_ = nullptr;
    dibWidth_ = 0;
    dibHeight_ = 0;
}

// ============================================================
// ConvertYuvToBgra
//
// 用 sws_scale 把 YUV420P 转成 BGRA（32-bit）。
// swsCtx_ 按源分辨率缓存，分辨率变化时重建。
// ============================================================
bool GdiVideoSink::ConvertYuvToBgra(const uint8_t* yuv, size_t yuvSize,
	uint32_t width, uint32_t height,
	std::vector<uint8_t>& bgra) {
	if (!yuv || width == 0 || height == 0) return false;

	// 只在分辨率变化时创建，否则一直复用！
	if (!swsCtx_ || swsSrcWidth_ != width || swsSrcHeight_ != height) {
		if (swsCtx_) sws_freeContext(swsCtx_);
		swsCtx_ = sws_getContext(
			width, height, AV_PIX_FMT_YUV420P,
			width, height, AV_PIX_FMT_BGRA,
			SWS_FAST_BILINEAR, nullptr, nullptr, nullptr); // 用 FAST 模式降低 CPU 占用
		swsSrcWidth_ = width;
		swsSrcHeight_ = height;
	}

	// 拷贝逻辑 (这里必须按 YUV420P 来拷，确保 stride 对齐)
	const size_t ySize = width * height;
	const size_t uvSize = ySize / 4;
	const uint8_t* srcData[4] = { yuv, yuv + ySize, yuv + ySize + uvSize, nullptr };
	const int srcLinesize[4] = { (int)width, (int)(width / 2), (int)(width / 2), 0 };

	const size_t bgraStride = static_cast<size_t>(width) * 4;
	bgra.resize(bgraStride * height);
	uint8_t* dstData[4] = { bgra.data(), nullptr, nullptr, nullptr };
	int dstLinesize[4] = { (int)bgraStride, 0, 0, 0 };

	sws_scale(swsCtx_, srcData, srcLinesize, 0, height, dstData, dstLinesize);
	return true;
}

// ============================================================
// Render
//
// 1. 校验绑定的流
// 2. YUV → BGRA
// 3. 拷贝到离屏 DIB（按目标尺寸缩放）
// 4. StretchBlt 到 HWND
// ============================================================
bool GdiVideoSink::Render(std::shared_ptr<const VideoFrame> frame) {
    if (!frame || !frame->IsValid()) return false;

    // Per-frame logging would flood Info; report the first frame only.
    if (!firstFrameLogged_) {
        firstFrameLogged_ = true;
        VCS_MEDIA_LOG_INFO("[GdiVideoSink] Render: first frame w=%u, h=%u, format=%d, dataSize=%zu",
            frame->width, frame->height, (int)frame->pixelFormat, frame->dataSize);
    } else {
        VCS_MEDIA_LOG_TRACE("[GdiVideoSink] Render: w=%u, h=%u, format=%d, dataSize=%zu",
            frame->width, frame->height, (int)frame->pixelFormat, frame->dataSize);
    }

    if (!nativeWindow_) {
        // 没有 HWND，静默丢弃（Headless 场景）
        return false;
    }

    HWND hwnd = static_cast<HWND>(nativeWindow_);
    if (!IsWindow(hwnd)) {
        VCS_MEDIA_LOG_WARNING("[GdiVideoSink] Render: invalid HWND %p", hwnd);
        return false;
    }

    // 校验绑定的流
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!boundStreamId_.empty() && frame->memberId != boundStreamId_) {
            // 不是当前绑定的流，丢弃（避免多流争抢）
            return false;
        }
    }

    const uint32_t srcW = frame->width;
    const uint32_t srcH = frame->height;
    if (srcW == 0 || srcH == 0) return false;

    // 目标尺寸：从窗口客户区拿
    RECT clientRect = {};
    if (!GetClientRect(hwnd, &clientRect)) {
        return false;
    }
	const LONG w = clientRect.right - clientRect.left;
	const LONG h = clientRect.bottom - clientRect.top;
	const uint32_t dstW = static_cast<uint32_t>(w > 1 ? w : 1);
	const uint32_t dstH = static_cast<uint32_t>(h > 1 ? h : 1);

    // 1. YUV → BGRA（源分辨率）
    std::vector<uint8_t> bgra;
    if (!ConvertYuvToBgra(frame->GetData(), frame->dataSize, srcW, srcH, bgra)) {
        return false;
    }

    // 2. 按目标尺寸创建离屏 DIB
    if (!EnsureBackBuffer(dstW, dstH)) {
        return false;
    }

    // 3. BGRA → 离屏 DIB（带缩放）
    //    这里用 StretchDIBits 一步到位：源 BGRA → 目标 DIB
    HDC memDC = static_cast<HDC>(dibMemDC_);
    BITMAPINFO srcBmi = {};
    srcBmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    srcBmi.bmiHeader.biWidth       = static_cast<LONG>(srcW);
    srcBmi.bmiHeader.biHeight      = -static_cast<LONG>(srcH);
    srcBmi.bmiHeader.biPlanes      = 1;
    srcBmi.bmiHeader.biBitCount    = 32;
    srcBmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(memDC,
                  0, 0, static_cast<int>(dstW), static_cast<int>(dstH),
                  0, 0, static_cast<int>(srcW), static_cast<int>(srcH),
                  bgra.data(), &srcBmi, DIB_RGB_COLORS, SRCCOPY);

    // 4. 离屏 DIB → 窗口
    HDC windowDC = GetDC(hwnd);
    if (!windowDC) return false;

    BitBlt(windowDC,
           0, 0, static_cast<int>(dstW), static_cast<int>(dstH),
           memDC,
           0, 0,
           SRCCOPY);

    ReleaseDC(hwnd, windowDC);

    return true;
}

}}}}