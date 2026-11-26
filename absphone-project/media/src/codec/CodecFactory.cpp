/**
 * @file CodecFactory.cpp
 * @brief Codec factory implementation
 *
 *  Supports multiple video encoding formats:
 * - H.264 (H264)
 * - H.265/HEVC (H265)
 * - VP8
 * - VP9
 * - AV1
 */


// Cross-platform includes
#include "ICodec.h"
#include "ICodecRegistry.h"
#include "CodecFactory.h"
#include "software/G711Codec.h"
#include "software/FFmpegCodec.h"
#include <iostream>
#include <cassert>
#include <memory>



extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vcs { namespace media { namespace codec {

// ============================================================
// Software Codec Factory Implementation
// ============================================================
class SoftwareCodecFactory : public ICodecFactory {
public:
    SoftwareCodecFactory() {
        // Register audio codecs
        audioEncoders_[AudioCodecType::G711U] = AudioEncoderCreator(software::CreateG711Encoder);
        audioDecoders_[AudioCodecType::G711U] = AudioDecoderCreator(software::CreateG711Decoder);
        audioEncoders_[AudioCodecType::G711A] = AudioEncoderCreator(software::CreateG711Encoder);
        audioDecoders_[AudioCodecType::G711A] = AudioDecoderCreator(software::CreateG711Decoder);

        // Register all supported video codecs
        // Video codecs use unified FFmpeg implementation, distinguished by codecType parameter
        videoEncoders_[VideoCodecType::H264] = VideoEncoderCreator(software::CreateFFmpegEncoder);
        videoDecoders_[VideoCodecType::H264] = VideoDecoderCreator(software::CreateFFmpegDecoder);
        videoEncoders_[VideoCodecType::H265] = VideoEncoderCreator(software::CreateFFmpegEncoder);
        videoDecoders_[VideoCodecType::H265] = VideoDecoderCreator(software::CreateFFmpegDecoder);
        videoEncoders_[VideoCodecType::VP8] = VideoEncoderCreator(software::CreateFFmpegEncoder);
        videoDecoders_[VideoCodecType::VP8] = VideoDecoderCreator(software::CreateFFmpegDecoder);
        videoEncoders_[VideoCodecType::VP9] = VideoEncoderCreator(software::CreateFFmpegEncoder);
        videoDecoders_[VideoCodecType::VP9] = VideoDecoderCreator(software::CreateFFmpegDecoder);
        videoEncoders_[VideoCodecType::AV1] = VideoEncoderCreator(software::CreateFFmpegEncoder);
        videoDecoders_[VideoCodecType::AV1] = VideoDecoderCreator(software::CreateFFmpegDecoder);
    }

    std::unique_ptr<IAudioEncoder> CreateAudioEncoder(
        AudioCodecType codec, CodecBackend backend) override {
        (void)backend;
        auto it = audioEncoders_.find(codec);
        if (it != audioEncoders_.end() && it->second) {
            return it->second();
        }
        return software::CreateG711Encoder();
    }

    std::unique_ptr<IAudioDecoder> CreateAudioDecoder(
        AudioCodecType codec, CodecBackend backend) override {
        (void)backend;
        auto it = audioDecoders_.find(codec);
        if (it != audioDecoders_.end() && it->second) {
            return it->second();
        }
        return software::CreateG711Decoder();
    }

    std::unique_ptr<IVideoEncoder> CreateVideoEncoder(
        VideoCodecType codec, CodecBackend backend) override {
        (void)backend;
        auto it = videoEncoders_.find(codec);
        if (it != videoEncoders_.end() && it->second) {
            return it->second();
        }
        // Default to FFmpeg, supports all formats
        return software::CreateFFmpegEncoder();
    }

    std::unique_ptr<IVideoDecoder> CreateVideoDecoder(
        VideoCodecType codec, CodecBackend backend) override {
        (void)backend;
        auto it = videoDecoders_.find(codec);
        if (it != videoDecoders_.end() && it->second) {
            return it->second();
        }
        // Default to FFmpeg, supports all formats
        return software::CreateFFmpegDecoder();
    }

    std::vector<CodecCapability> GetAudioCapabilities(
        AudioCodecType codec, CodecBackend backend) override {
        (void)backend;
        std::vector<CodecCapability> caps;
        CodecCapability cap;
        cap.codecName = AudioCodecTypeToString(codec);
        cap.backend = CodecBackend::Software;
        cap.supportSimd = true;
        cap.maxResolution = 48000;
        cap.maxBitrate = (codec == AudioCodecType::Opus) ? 128000 : 64000;
        caps.push_back(cap);
        return caps;
    }

    // Get hardware encoder name based on codec type
    const char* GetHardwareEncoderName(VideoCodecType codecType) {
        switch (codecType) {
            case VideoCodecType::H264: return "h264_nvenc";
            case VideoCodecType::H265: return "hevc_nvenc";
            case VideoCodecType::VP8: return nullptr;  // VP8 has no NVIDIA hardware support
            case VideoCodecType::VP9: return nullptr;
            case VideoCodecType::AV1: return nullptr;
            default: return nullptr;
        }
    }

    std::vector<CodecCapability> GetVideoCapabilities(
        VideoCodecType codec, CodecBackend backend) override {
        (void)backend;
        std::vector<CodecCapability> caps;
        CodecCapability cap;
        cap.codecName = VideoCodecTypeToString(codec);
        cap.backend = CodecBackend::Software;
        cap.maxResolution = 1920 * 1080;
        cap.maxFramerate = 60;
        cap.maxBitrate = 10000000;

        // Detect hardware encoders for each format
        // H.264 hardware encoders
        if (codec == VideoCodecType::H264) {
            if (avcodec_find_encoder_by_name("h264_nvenc")) {
                cap.supportHardware = true;
                cap.backend = CodecBackend::Hardware;
                cap.codecName = "h264_nvenc";
                cap.priority = 1;
                caps.push_back(cap);
            }
            if (avcodec_find_encoder_by_name("h264_vaapi")) {
                cap.supportHardware = true;
                cap.backend = CodecBackend::Hardware;
                cap.codecName = "h264_vaapi";
                cap.priority = 2;
                caps.push_back(cap);
            }
            if (avcodec_find_encoder_by_name("h264_v4l2m2m")) {
                cap.supportHardware = true;
                cap.backend = CodecBackend::Hardware;
                cap.codecName = "h264_v4l2m2m";
                cap.priority = 3;
                caps.push_back(cap);
            }
        }

        // H.265/HEVC hardware encoders
        if (codec == VideoCodecType::H265) {
            if (avcodec_find_encoder_by_name("hevc_nvenc")) {
                cap.supportHardware = true;
                cap.backend = CodecBackend::Hardware;
                cap.codecName = "hevc_nvenc";
                cap.priority = 1;
                cap.maxResolution = 3840 * 2160;  // 4K support
                caps.push_back(cap);
            }
            if (avcodec_find_encoder_by_name("hevc_vaapi")) {
                cap.supportHardware = true;
                cap.backend = CodecBackend::Hardware;
                cap.codecName = "hevc_vaapi";
                cap.priority = 2;
                cap.maxResolution = 3840 * 2160;
                caps.push_back(cap);
            }
        }

        // Software encoders
        if (codec == VideoCodecType::H264 && avcodec_find_encoder_by_name("libx264")) {
            cap.supportHardware = false;
            cap.backend = CodecBackend::Software;
            cap.codecName = "libx264";
            cap.priority = 4;
            caps.push_back(cap);
        }
        if (codec == VideoCodecType::H265 && avcodec_find_encoder_by_name("libx265")) {
            cap.supportHardware = false;
            cap.backend = CodecBackend::Software;
            cap.codecName = "libx265";
            cap.priority = 4;
            caps.push_back(cap);
        }

        // FFmpeg built-in encoder (fallback)
        cap.supportHardware = false;
        cap.backend = CodecBackend::Software;
        cap.codecName = VideoCodecTypeToString(codec);
        cap.priority = 5;
        cap.maxResolution = 1920 * 1080;
        caps.push_back(cap);

        return caps;
    }

    // Dynamic registration implementation
    bool RegisterAudioEncoder(AudioCodecType codec,
        std::function<std::unique_ptr<IAudioEncoder>()> creator) override {
        if (!creator) return false;
        audioEncoders_[codec] = creator;
        return true;
    }

    bool RegisterAudioDecoder(AudioCodecType codec,
        std::function<std::unique_ptr<IAudioDecoder>()> creator) override {
        if (!creator) return false;
        audioDecoders_[codec] = creator;
        return true;
    }

    bool RegisterVideoEncoder(VideoCodecType codec,
        std::function<std::unique_ptr<IVideoEncoder>()> creator) override {
        if (!creator) return false;
        videoEncoders_[codec] = creator;
        return true;
    }

    bool RegisterVideoDecoder(VideoCodecType codec,
        std::function<std::unique_ptr<IVideoDecoder>()> creator) override {
        if (!creator) return false;
        videoDecoders_[codec] = creator;
        return true;
    }

    // Unregister implementation
    bool UnregisterAudioEncoder(AudioCodecType codec) override {
        return audioEncoders_.erase(codec) > 0;
    }

    bool UnregisterAudioDecoder(AudioCodecType codec) override {
        return audioDecoders_.erase(codec) > 0;
    }

    bool UnregisterVideoEncoder(VideoCodecType codec) override {
        return videoEncoders_.erase(codec) > 0;
    }

    bool UnregisterVideoDecoder(VideoCodecType codec) override {
        return videoDecoders_.erase(codec) > 0;
    }

private:
    std::map<AudioCodecType, std::function<std::unique_ptr<IAudioEncoder>()>> audioEncoders_;
    std::map<AudioCodecType, std::function<std::unique_ptr<IAudioDecoder>()>> audioDecoders_;
    std::map<VideoCodecType, std::function<std::unique_ptr<IVideoEncoder>()>> videoEncoders_;
    std::map<VideoCodecType, std::function<std::unique_ptr<IVideoDecoder>()>> videoDecoders_;
};

// ============================================================
// Factory Creation Functions (for CodecManager)
// ============================================================
std::unique_ptr<ICodecFactory> CreateDefaultCodecFactory() {
    return std::unique_ptr<ICodecFactory>(new SoftwareCodecFactory());
}

std::unique_ptr<ICodecFactory> CreateHardwareCodecFactory() {
    // TODO: Implement hardware factory
    return CreateDefaultCodecFactory();
}

std::unique_ptr<ICodecFactory> CreateRemoteCodecFactory(const std::string& serverAddress) {
    // TODO: Implement remote factory
    (void)serverAddress;
    return CreateDefaultCodecFactory();
}

}}} // namespace vcs::media::codec
