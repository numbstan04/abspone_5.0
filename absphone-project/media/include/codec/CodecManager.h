#pragma once

/**
 * @file CodecManager.h
 * @brief Unified codec manager
 *
 * Provides unified factory access entry point.
 * Compilation method is determined by CMake (static .a or dynamic .so), code doesn't need to care.
 *
 * Usage:
 *   auto factory = CodecManager::Instance().CreateDefaultFactory();
 */

#include "ICodec.h"
#include <string>
#include <memory>

namespace vcs { namespace media { namespace codec {

/**
 * @brief Codec manager (singleton)
 *
 * Unified entry point at code level, compilation method determined by CMake.
 *
 * VCS_CODEC_API is required: media (static lib) calls into codec_soft.dll,
 * so these symbols must be exported and an import library produced.
 */
class VCS_CODEC_API CodecManager {
public:
    static CodecManager& Instance();

    // Create factory
    std::unique_ptr<ICodecFactory> CreateDefaultFactory();
    std::unique_ptr<ICodecFactory> CreateHardwareFactory();
    std::unique_ptr<ICodecFactory> CreateRemoteFactory(const std::string& serverAddress);

    // Convenience methods
    std::unique_ptr<IAudioEncoder> CreateAudioEncoder(
        AudioCodecType codec,
        CodecBackend backend = CodecBackend::Software) {
        return CreateDefaultFactory()->CreateAudioEncoder(codec, backend);
    }

    std::unique_ptr<IAudioDecoder> CreateAudioDecoder(
        AudioCodecType codec,
        CodecBackend backend = CodecBackend::Software) {
        return CreateDefaultFactory()->CreateAudioDecoder(codec, backend);
    }

    std::unique_ptr<IVideoEncoder> CreateVideoEncoder(
        VideoCodecType codec,
        CodecBackend backend = CodecBackend::Software) {
        return CreateDefaultFactory()->CreateVideoEncoder(codec, backend);
    }

    std::unique_ptr<IVideoDecoder> CreateVideoDecoder(
        VideoCodecType codec,
        CodecBackend backend = CodecBackend::Software) {
        return CreateDefaultFactory()->CreateVideoDecoder(codec, backend);
    }

private:
    CodecManager() = default;
    ~CodecManager() = default;
    CodecManager(const CodecManager&) = delete;
    CodecManager& operator=(const CodecManager&) = delete;
};

}}} // namespace vcs::media::codec
