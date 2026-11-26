#pragma once

#include "ICodec.h"
#include <map>
#include <vector>
#include <functional>

namespace vcs { namespace media { namespace codec {

// ============================================================
// Creator Function Types
// ============================================================
using AudioEncoderCreator = std::unique_ptr<IAudioEncoder>(*)();
using AudioDecoderCreator = std::unique_ptr<IAudioDecoder>(*)();
using VideoEncoderCreator = std::unique_ptr<IVideoEncoder>(*)();
using VideoDecoderCreator = std::unique_ptr<IVideoDecoder>(*)();

// ============================================================
// Registration Entry
// ============================================================
struct AudioCodecRegistration {
    AudioCodecType type;
    CodecBackend backend;
    CodecCapability capability;
    AudioEncoderCreator createEncoder = nullptr;
    AudioDecoderCreator createDecoder = nullptr;
    int priority = 0;
};

struct VideoCodecRegistration {
    VideoCodecType type;
    CodecBackend backend;
    CodecCapability capability;
    VideoEncoderCreator createEncoder = nullptr;
    VideoDecoderCreator createDecoder = nullptr;
    int priority = 0;
};

// ============================================================
// Codec Registry Interface
// ============================================================
class ICodecRegistry {
public:
    virtual ~ICodecRegistry() = default;

    // ==================== Registration ====================

    // Register audio encoder
    virtual void RegisterAudioEncoder(
        AudioCodecType type,
        AudioEncoderCreator creator,
        const CodecCapability& cap,
        int priority = 0) = 0;

    virtual void RegisterAudioDecoder(
        AudioCodecType type,
        AudioDecoderCreator creator,
        const CodecCapability& cap,
        int priority = 0) = 0;

    // Register video encoder
    virtual void RegisterVideoEncoder(
        VideoCodecType type,
        VideoEncoderCreator creator,
        const CodecCapability& cap,
        int priority = 0) = 0;

    virtual void RegisterVideoDecoder(
        VideoCodecType type,
        VideoDecoderCreator creator,
        const CodecCapability& cap,
        int priority = 0) = 0;

    // ==================== Query ====================

    // Get specific codec capability
    virtual CodecCapability GetAudioCapability(
        AudioCodecType type, CodecBackend backend) const = 0;

    virtual CodecCapability GetVideoCapability(
        VideoCodecType type, CodecBackend backend) const = 0;

    // Get supported implementation list
    virtual std::vector<AudioCodecRegistration> GetAudioImplementations(
        AudioCodecType type) const = 0;

    virtual std::vector<VideoCodecRegistration> GetVideoImplementations(
        VideoCodecType type) const = 0;

    // Check if supported
    virtual bool IsAudioSupported(AudioCodecType type, CodecBackend backend) const = 0;
    virtual bool IsVideoSupported(VideoCodecType type, CodecBackend backend) const = 0;

    // ==================== Singleton ====================
    static ICodecRegistry& Instance();
};

// ============================================================
// Registration Helper Macros
// ============================================================
#define REGISTER_AUDIO_ENCODER(registry, type, creator, cap, priority) \
    registry.RegisterAudioEncoder(type, creator, cap, priority)

#define REGISTER_AUDIO_DECODER(registry, type, creator, cap, priority) \
    registry.RegisterAudioDecoder(type, creator, cap, priority)

#define REGISTER_VIDEO_ENCODER(registry, type, creator, cap, priority) \
    registry.RegisterVideoEncoder(type, creator, cap, priority)

#define REGISTER_VIDEO_DECODER(registry, type, creator, cap, priority) \
    registry.RegisterVideoDecoder(type, creator, cap, priority)

}}} // namespace vcs::media::codec
