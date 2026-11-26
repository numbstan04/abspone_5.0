#pragma once

#include "media_export.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace vcs { namespace media {

/**
 * @brief Video frame structure
 *
 * Unified video frame format, supports both raw pixel data and encoded data modes.
 * Uses shared_ptr to manage data buffer, avoiding unnecessary data copies.
 */
struct VideoFrame {
    // ============================================================
    // Frame data
    // ============================================================
    std::shared_ptr<std::vector<uint8_t>> data;  // Frame data buffer
    size_t dataOffset = 0;                        // Data start offset
    size_t dataSize = 0;                        // Valid data size

    // ============================================================
    // Frame properties
    // ============================================================
    uint32_t width = 0;           // Width (pixels)
    uint32_t height = 0;         // Height (pixels)
    uint64_t timestamp = 0;       // Timestamp (microseconds)
    uint32_t duration = 0;        // Frame duration (microseconds)
    bool isKeyFrame = false;       // Is keyframe
    bool isFlipped = false;        // Needs flip

    // ============================================================
    // Encoding info (for encoded frames)
    // ============================================================
    uint8_t payloadType = 0;      // RTP payload type
    std::string codecName;        // Codec name

    // ============================================================
    // Raw pixel format info
    // ============================================================
    enum class PixelFormat {
        UNKNOWN,
        YUV420P,      // Planar YUV 4:2:0
        YUV422P,      // Planar YUV 4:2:2
        YUV444P,      // Planar YUV 4:4:4
        NV12,         // Semi-planar YUV 4:2:0
        NV21,         // Semi-planar YUV 4:2:0 (UV order reversed)
        RGB24,        // Packed RGB
        BGR24,        // Packed BGR
        RGBA,         // RGB with Alpha channel
        BGRA,         // BGR with Alpha channel
    };
    PixelFormat pixelFormat = PixelFormat::UNKNOWN;

    // ============================================================
    // Metadata
    // ============================================================
    std::string memberId;         // Source member ID
    uint32_t ssrc = 0;           // SSRC identifier
    std::map<std::string, std::string> metadata;  // Custom metadata

    // ============================================================
    // Helper methods
    // ============================================================

    /**
     * @brief Check if frame is valid
     */
    bool IsValid() const {
        return data != nullptr &&
               dataSize > 0 &&
               width > 0 &&
               height > 0;
    }

    /**
     * @brief Get data pointer
     */
    const uint8_t* GetData() const {
        return data ? (data->data() + dataOffset) : nullptr;
    }

    uint8_t* GetData() {
        return data ? (data->data() + dataOffset) : nullptr;
    }

    /**
     * @brief Create empty frame
     */
    static VideoFrame CreateEmpty() {
        return VideoFrame{};
    }

    /**
     * @brief Create encoded frame
     */
    static VideoFrame CreateEncoded(
        std::shared_ptr<std::vector<uint8_t>> encodedData,
        uint8_t pt,
        uint64_t ts,
        bool keyFrame) {
        VideoFrame frame;
        frame.data = std::move(encodedData);
        frame.dataSize = frame.data ? frame.data->size() : 0;
        frame.payloadType = pt;
        frame.timestamp = ts;
        frame.isKeyFrame = keyFrame;
        return frame;
    }

    /**
     * @brief Create raw pixel frame
     */
    static VideoFrame CreateRaw(
        std::shared_ptr<std::vector<uint8_t>> rawData,
        uint32_t w,
        uint32_t h,
        PixelFormat fmt,
        uint64_t ts = 0) {
        VideoFrame frame;
        frame.data = std::move(rawData);
        frame.width = w;
        frame.height = h;
        frame.pixelFormat = fmt;
        frame.timestamp = ts;
        frame.dataSize = frame.data ? frame.data->size() : 0;
        return frame;
    }

    /**
     * @brief Deep copy (for scenarios requiring independent copy)
     */
    VideoFrame Clone() const {
        VideoFrame copy;
        if (data) {
            copy.data = std::make_shared<std::vector<uint8_t>>(*data);
            copy.dataOffset = dataOffset;
        }
        copy.dataSize = dataSize;
        copy.width = width;
        copy.height = height;
        copy.timestamp = timestamp;
        copy.duration = duration;
        copy.isKeyFrame = isKeyFrame;
        copy.isFlipped = isFlipped;
        copy.payloadType = payloadType;
        copy.codecName = codecName;
        copy.pixelFormat = pixelFormat;
        copy.memberId = memberId;
        copy.ssrc = ssrc;
        copy.metadata = metadata;
        return copy;
    }

    /**
     * @brief Move semantics (for pipeline transfer)
     */
    VideoFrame(VideoFrame&&) = default;
    VideoFrame& operator=(VideoFrame&&) = default;
    VideoFrame(const VideoFrame&) = default;
    VideoFrame& operator=(const VideoFrame&) = default;

    /**
     * @brief Default constructor
     */
    VideoFrame() = default;
};

} // namespace media
} // namespace vcs
