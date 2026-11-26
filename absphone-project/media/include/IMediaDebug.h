#pragma once

#include "media_export.h"
#include <string>
#include <cstdint>
#include <memory>

namespace vcs { namespace media {

// ============================================================
// Media debug configuration
// ============================================================

// Audio debug configuration
struct AudioDebugConfig {
    bool enabled = false;              // Enable
    bool saveInput = false;            // Save input audio
    bool saveOutput = false;           // Save output audio
    bool saveMixed = false;            // Save mixed audio
    bool saveDecoded = false;         // Save decoded PCM
    bool saveEncoded = false;          // Save pre-encoded PCM

    std::string format = "pcm";        // Save format: pcm, wav
    int sampleRate = 48000;           // Sample rate
    int channels = 2;                 // Number of channels
    int bitsPerSample = 16;           // Bit depth
};

// Video debug configuration
struct VideoDebugConfig {
    bool enabled = false;              // Enable
    bool saveInput = false;            // Save input video
    bool saveOutput = false;           // Save output video
    bool saveComposed = false;        // Save composed video
    bool saveDecoded = false;         // Save decoded data
    bool saveEncoded = false;          // Save encoded data

    std::string format = "h264";       // Save format: h264, ivf, raw
};

// RTP debug configuration
struct RtpDebugConfig {
    bool enabled = false;              // Enable
    bool saveIncoming = false;         // Save incoming RTP packets
    bool saveOutgoing = false;         // Save outgoing RTP packets
    bool hexDump = false;              // Save in hexadecimal format
};

// Overall debug configuration
struct MediaDebugConfig {
    bool enabled = false;              // Master switch
    std::string outputDir = "./debug"; // Output directory

    // File size limits
    size_t maxFileSizeMB = 100;        // Max file size in MB
    size_t maxTotalSizeMB = 1000;     // Max total directory size in MB
    bool autoCleanup = true;           // Auto cleanup when exceeding limit

    AudioDebugConfig audio;
    VideoDebugConfig video;
    RtpDebugConfig rtp;
};

// ============================================================
// Media debug interface
// ============================================================

class VCS_MEDIA_API IMediaDebug {
public:
    virtual ~IMediaDebug() = default;

    // Get configuration
    virtual MediaDebugConfig GetConfig() const = 0;
    virtual void SetConfig(const MediaDebugConfig& config) = 0;

    // Master switch
    virtual bool IsEnabled() const = 0;
    virtual void SetEnabled(bool enabled) = 0;

    // Load from configuration file
    virtual void LoadFromFile(const std::string& configPath) = 0;

    // ========== Audio debug ==========

    // Save audio PCM data (input)
    virtual void SaveAudioInput(const std::string& memberId,
                               const uint8_t* data,
                               size_t size) = 0;

    // Save audio PCM data (output)
    virtual void SaveAudioOutput(const std::string& memberId,
                                const uint8_t* data,
                                size_t size) = 0;

    // Save mixed audio
    virtual void SaveAudioMixed(const uint8_t* data, size_t size) = 0;

    // Save decoded PCM
    virtual void SaveAudioDecoded(const std::string& memberId,
                                 const uint8_t* data,
                                 size_t size) = 0;

    // Save pre-encoded PCM
    virtual void SaveAudioEncoded(const std::string& memberId,
                                  const uint8_t* data,
                                  size_t size) = 0;

    // ========== Video debug ==========

    // Save video data (input)
    virtual void SaveVideoInput(const std::string& memberId,
                               const uint8_t* data,
                               size_t size,
                               uint32_t timestamp) = 0;

    // Save video data (output)
    virtual void SaveVideoOutput(const std::string& memberId,
                                const uint8_t* data,
                                size_t size,
                                uint32_t timestamp) = 0;

    // Save composed video
    virtual void SaveVideoComposed(const uint8_t* data,
                                  size_t size,
                                  uint32_t timestamp) = 0;

    // Save decoded YUV
    virtual void SaveVideoDecoded(const std::string& memberId,
                                 const uint8_t* data,
                                 size_t size) = 0;

    // Save encoded H.264
    virtual void SaveVideoEncoded(const std::string& memberId,
                                 const uint8_t* data,
                                 size_t size,
                                 bool isKeyFrame) = 0;

    // ========== RTP debug ==========

    // Save incoming RTP packets
    virtual void SaveRtpIncoming(const uint8_t* data,
                                size_t size,
                                uint32_t ssrc,
                                uint16_t seq) = 0;

    // Save outgoing RTP packets
    virtual void SaveRtpOutgoing(const uint8_t* data,
                                 size_t size,
                                 uint32_t ssrc,
                                 uint16_t seq) = 0;

    // ========== Cleanup ==========

    // Cleanup all debug files
    virtual void Cleanup() = 0;

    // Get current debug directory size
    virtual size_t GetDebugDirSize() const = 0;
};

// Get media debug singleton
VCS_MEDIA_API IMediaDebug* GetMediaDebug();

}} // namespace vcs::media
