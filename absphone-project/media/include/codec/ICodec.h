#ifndef ICODEC_H
#define ICODEC_H

// =============================================================================
// Cross-platform includes - use standard C++ library, not platform-specific headers
// =============================================================================
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "codec_export.h"

namespace vcs { namespace media { namespace codec {

// ============================================================
// Codec Types
// ============================================================

enum class AudioCodecType {
    G711U,     // G.711 u-law
    G711A,     // G.711 A-law
    G722,      // G.722
    G728,      // G.728
    G729,      // G.729A
    Opus,      // Opus
    AAC,       // AAC
};

enum class VideoCodecType {
    H264,
    H265,
    VP8,
    VP9,
    AV1,
};

// ============================================================
// Codec Backend Types
// ============================================================
enum class CodecBackend {
    Software,   // FFmpeg/libopus software codec
    Hardware,   // GPU/hardware acceleration
    Remote,     // Remote gRPC call
};

// ============================================================
// Codec Capability
// ============================================================
struct CodecCapability {
    std::string codecName;
    CodecBackend backend;

    bool supportHardware = false;      // Hardware codec support
    bool supportSimd = false;         // SIMD optimization support
    uint32_t maxResolution = 1920 * 1080;
    uint32_t maxFramerate = 60;
    uint32_t maxBitrate = 10000000;   // bps
    uint32_t recommendedBitrate = 0;
    int priority = 0;                 // Selection priority
};

// ============================================================
// SRTP Configuration
// ============================================================
enum class SrtpMode {
    Disabled = 0,    // Completely disable SRTP
    Optional = 1,    // Optional - crypto in SDP but not required
    Mandatory = 2,   // Mandatory - must use SRTP
};

struct SrtpConfig {
    SrtpMode mode = SrtpMode::Disabled;  // Default disabled
    bool requireSecureSignaling = false;   // Require TLS signaling
};

// ============================================================
// Audio Codec Configuration
// ============================================================
struct AudioCodecConfig {
    AudioCodecType codecType = AudioCodecType::G711U;
    uint32_t sampleRate = 8000;
    uint8_t channels = 1;
    uint32_t bitrate = 64000;  // bps
    CodecBackend backend = CodecBackend::Software;
    std::string remoteAddress;  // Remote codec address
};

// ============================================================
// Video Codec Configuration
// ============================================================
struct VideoCodecConfig {
    VideoCodecType codecType = VideoCodecType::H264;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t framerate = 30;
    uint32_t bitrate = 2000000;  // bps
    uint32_t keyFrameInterval = 60;
    CodecBackend backend = CodecBackend::Software;
    std::string remoteAddress;  // Remote codec address

    // Encoding parameters
    uint32_t gopSize = 60;
    int8_t quality = -1;  // -1 = use bitrate control

    // [V5.4 New] Preferred encoder name (optional)
    // Supported encoders:
    //   Hardware: h264_nvenc (NVIDIA), h264_vaapi (Intel/AMD), h264_v4l2m2m (Linux)
    //   Software: libx264 (high quality), h264 (FFmpeg built-in, fast)
    // If empty or unavailable, auto-select best available encoder based on backend
    std::string preferredEncoder;
};

// ============================================================
// PCM Audio Frame
//
// Memory ownership contract:
//   - The caller is responsible for freeing `data`.
//   - `data` MUST be allocated with `new int16_t[]` (NOT malloc) so that
//     it can be released with `delete[]` (or by transferring ownership
//     into a `std::unique_ptr<int16_t[]>`).
//   - Using `free()` on this pointer is undefined behavior.
//   - The decoder owns the buffer during the call and transfers
//     ownership to the caller when `Decode` returns true.
// ============================================================
struct AudioPcmFrame {
    int16_t* data = nullptr;
    size_t samples = 0;
    uint32_t sampleRate = 8000;
    uint8_t channels = 1;
    uint64_t timestamp = 0;
};

// ============================================================
// Audio Encoder Interface
// ============================================================
class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;

    // Initialize
    virtual bool Initialize(const AudioCodecConfig& config) = 0;

    // Encode PCM->encoded data
    virtual bool Encode(const AudioPcmFrame& pcmFrame,
                       std::vector<uint8_t>& outData,
                       uint64_t& timestamp) = 0;

    // Get encoder capability
    virtual CodecCapability GetCapability() const = 0;

    // Get backend type
    virtual CodecBackend GetBackend() const = 0;

    // Release
    virtual void Release() = 0;
};

// ============================================================
// Audio Decoder Interface
// ============================================================
class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    // Initialize
    virtual bool Initialize(const AudioCodecConfig& config) = 0;

    // Decode encoded data->PCM
    virtual bool Decode(const uint8_t* encodedData,
                       size_t encodedSize,
                       AudioPcmFrame& outFrame) = 0;

    // Get decoder capability
    virtual CodecCapability GetCapability() const = 0;

    // Get backend type
    virtual CodecBackend GetBackend() const = 0;

    // Packet loss concealment (PLC)
    virtual void PacketLossConceal(size_t samples) = 0;

    // Release
    virtual void Release() = 0;
};

// ============================================================
// Video Encoder Interface
// ============================================================
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // Initialize
    virtual bool Initialize(const VideoCodecConfig& config) = 0;

    // Encode YUV->encoded data
    virtual bool Encode(const uint8_t* yuvData,
                       size_t yuvSize,
                       std::vector<uint8_t>& outData,
                       bool& isKeyFrame,
                       uint64_t timestamp) = 0;

    // Dynamic bitrate adjustment
    virtual void UpdateBitrate(uint32_t bitrate) = 0;

    // Request key frame
    virtual void RequestKeyFrame() = 0;

    // Get encoder capability
    virtual CodecCapability GetCapability() const = 0;

    // Get backend type
    virtual CodecBackend GetBackend() const = 0;

    // Release
    virtual void Release() = 0;
};

// ============================================================
// Video Decoder Interface
// ============================================================
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // Initialize
    virtual bool Initialize(const VideoCodecConfig& config) = 0;

    // Decode encoded data->YUV
    virtual bool Decode(const uint8_t* encodedData,
                       size_t encodedSize,
                       bool isKeyFrame,
                       uint8_t* yuvData,
                       size_t& yuvSize,
                       uint32_t& width,
                       uint32_t& height) = 0;

    // Packet loss concealment
    virtual void PacketLossConceal() = 0;

    // Get decoder capability
    virtual CodecCapability GetCapability() const = 0;

    // Get backend type
    virtual CodecBackend GetBackend() const = 0;

    // Release
    virtual void Release() = 0;
};

// ============================================================
// Codec Factory Interface
// ============================================================
class ICodecFactory {
public:
    virtual ~ICodecFactory() = default;

    // Create audio encoder
    virtual std::unique_ptr<IAudioEncoder> CreateAudioEncoder(
        AudioCodecType codec,
        CodecBackend backend = CodecBackend::Software) = 0;

    // Create audio decoder
    virtual std::unique_ptr<IAudioDecoder> CreateAudioDecoder(
        AudioCodecType codec,
        CodecBackend backend = CodecBackend::Software) = 0;

    // Create video encoder
    virtual std::unique_ptr<IVideoEncoder> CreateVideoEncoder(
        VideoCodecType codec,
        CodecBackend backend = CodecBackend::Software) = 0;

    // Create video decoder
    virtual std::unique_ptr<IVideoDecoder> CreateVideoDecoder(
        VideoCodecType codec,
        CodecBackend backend = CodecBackend::Software) = 0;

    // Get supported capability list
    virtual std::vector<CodecCapability> GetAudioCapabilities(
        AudioCodecType codec, CodecBackend backend) = 0;
    virtual std::vector<CodecCapability> GetVideoCapabilities(
        VideoCodecType codec, CodecBackend backend) = 0;

    // [P5 Improvement] Dynamic codec registration
    // Returns true if registration succeeds, false if already exists or invalid params
    virtual bool RegisterAudioEncoder(AudioCodecType codec,
        std::function<std::unique_ptr<IAudioEncoder>()> creator) = 0;
    virtual bool RegisterAudioDecoder(AudioCodecType codec,
        std::function<std::unique_ptr<IAudioDecoder>()> creator) = 0;
    virtual bool RegisterVideoEncoder(VideoCodecType codec,
        std::function<std::unique_ptr<IVideoEncoder>()> creator) = 0;
    virtual bool RegisterVideoDecoder(VideoCodecType codec,
        std::function<std::unique_ptr<IVideoDecoder>()> creator) = 0;

    // Unregister (for plugin unload)
    virtual bool UnregisterAudioEncoder(AudioCodecType codec) = 0;
    virtual bool UnregisterAudioDecoder(AudioCodecType codec) = 0;
    virtual bool UnregisterVideoEncoder(VideoCodecType codec) = 0;
    virtual bool UnregisterVideoDecoder(VideoCodecType codec) = 0;
};

// ============================================================
// Factory Creation Functions (for CodecManager internal use)
// ============================================================

// Create default factory (local software implementation)
std::unique_ptr<ICodecFactory> CreateDefaultCodecFactory();

// Create hardware factory
std::unique_ptr<ICodecFactory> CreateHardwareCodecFactory();

// Create remote factory (gRPC call to remote media service)
std::unique_ptr<ICodecFactory> CreateRemoteCodecFactory(
    const std::string& serverAddress);

// ============================================================
// Type Name Conversion
// ============================================================
inline const char* AudioCodecTypeToString(AudioCodecType type) {
    switch (type) {
        case AudioCodecType::G711U: return "PCMU";
        case AudioCodecType::G711A: return "PCMA";
        case AudioCodecType::G722: return "G722";
        case AudioCodecType::G728: return "G728";
        case AudioCodecType::G729: return "G729A";
        case AudioCodecType::Opus: return "opus";
        case AudioCodecType::AAC: return "aac";
        default: return "unknown";
    }
}

inline const char* VideoCodecTypeToString(VideoCodecType type) {
    switch (type) {
        case VideoCodecType::H264: return "H264";
        case VideoCodecType::H265: return "H265";
        case VideoCodecType::VP8: return "VP8";
        case VideoCodecType::VP9: return "VP9";
        case VideoCodecType::AV1: return "AV1";
        default: return "unknown";
    }
}

inline const char* CodecBackendToString(CodecBackend backend) {
    switch (backend) {
        case CodecBackend::Software: return "software";
        case CodecBackend::Hardware: return "hardware";
        case CodecBackend::Remote: return "remote";
        default: return "unknown";
    }
}

// ============================================================
// String-to-Enum parsing helpers
//
// Used by processors (Video/Audio) when SetCodec is called with a
// negotiated codec name coming from SDP. The SDP/SDP-fallback name
// may be in different casings (e.g. "PCMU", "pcmu", "Opus", "opus"),
// so we perform a case-insensitive comparison and also handle common
// SDP aliases (PCMU<->G711U, PCMA<->G711A, opus<->OPUS, etc.).
// ============================================================
namespace detail {
    // Case-insensitive ASCII equality
    inline bool IEqualAscii(const std::string& a, const char* b) {
        size_t i = 0;
        for (; i < a.size() && b[i] != '\0'; ++i) {
            unsigned char ca = static_cast<unsigned char>(a[i]);
            unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return i == a.size() && b[i] == '\0';
    }

    // Strip leading/trailing ASCII whitespace
    inline std::string TrimAscii(const std::string& s) {
        size_t begin = 0;
        while (begin < s.size() &&
               std::isspace(static_cast<unsigned char>(s[begin]))) {
            ++begin;
        }
        size_t end = s.size();
        while (end > begin &&
               std::isspace(static_cast<unsigned char>(s[end - 1]))) {
            --end;
        }
        return s.substr(begin, end - begin);
    }
} // namespace detail

// Try to parse a video codec name (case-insensitive, SDP aliases supported).
// On success returns true and sets `out`; on failure returns false.
inline bool TryParseVideoCodecType(const std::string& name, VideoCodecType& out) {
    std::string n = detail::TrimAscii(name);
    if (n.empty()) return false;
    if (detail::IEqualAscii(n, "H264")) { out = VideoCodecType::H264; return true; }
    if (detail::IEqualAscii(n, "H265") ||
        detail::IEqualAscii(n, "HEVC")) { out = VideoCodecType::H265; return true; }
    if (detail::IEqualAscii(n, "VP8"))  { out = VideoCodecType::VP8;  return true; }
    if (detail::IEqualAscii(n, "VP9"))  { out = VideoCodecType::VP9;  return true; }
    if (detail::IEqualAscii(n, "AV1"))  { out = VideoCodecType::AV1;  return true; }
    return false;
}

// Try to parse an audio codec name (case-insensitive, SDP aliases supported).
// On success returns true and sets `out`; on failure returns false.
inline bool TryParseAudioCodecType(const std::string& name, AudioCodecType& out) {
    std::string n = detail::TrimAscii(name);
    if (n.empty()) return false;
    // G.711 u-law: SDP "PCMU" / "G711U" / "G.711U"
    if (detail::IEqualAscii(n, "PCMU") ||
        detail::IEqualAscii(n, "G711U") ||
        detail::IEqualAscii(n, "G.711U")) {
        out = AudioCodecType::G711U; return true;
    }
    // G.711 A-law: SDP "PCMA" / "G711A" / "G.711A"
    if (detail::IEqualAscii(n, "PCMA") ||
        detail::IEqualAscii(n, "G711A") ||
        detail::IEqualAscii(n, "G.711A")) {
        out = AudioCodecType::G711A; return true;
    }
    if (detail::IEqualAscii(n, "G722") ||
        detail::IEqualAscii(n, "G.722")) { out = AudioCodecType::G722; return true; }
    if (detail::IEqualAscii(n, "G728") ||
        detail::IEqualAscii(n, "G.728")) { out = AudioCodecType::G728; return true; }
    if (detail::IEqualAscii(n, "G729") ||
        detail::IEqualAscii(n, "G729A") ||
        detail::IEqualAscii(n, "G.729A") ||
        detail::IEqualAscii(n, "G.729")) {
        out = AudioCodecType::G729; return true;
    }
    if (detail::IEqualAscii(n, "opus") ||
        detail::IEqualAscii(n, "OPUS")) { out = AudioCodecType::Opus; return true; }
    if (detail::IEqualAscii(n, "AAC") ||
        detail::IEqualAscii(n, "AAC_LC") ||
        detail::IEqualAscii(n, "AAC-LC")) {
        out = AudioCodecType::AAC; return true;
    }
    return false;
}

// Convenience wrappers: returns the parsed value, or `defaultValue` if parsing fails.
inline VideoCodecType ParseVideoCodecTypeOrDefault(const std::string& name,
                                                   VideoCodecType defaultValue) {
    VideoCodecType v = defaultValue;
    TryParseVideoCodecType(name, v);
    return v;
}

inline AudioCodecType ParseAudioCodecTypeOrDefault(const std::string& name,
                                                   AudioCodecType defaultValue) {
    AudioCodecType v = defaultValue;
    TryParseAudioCodecType(name, v);
    return v;
}

}}} // namespace vcs::media::codec

#endif // ICODEC_H
