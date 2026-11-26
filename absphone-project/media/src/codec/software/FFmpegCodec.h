#pragma once

#include "codec/ICodec.h"

namespace vcs { namespace media { namespace codec { namespace software {

// ============================================================
// FFmpeg video codec (H.264 software encoder/decoder)
// ============================================================

class FFmpegEncoder : public IVideoEncoder {
public:
    FFmpegEncoder();
    ~FFmpegEncoder() override;

    bool Initialize(const VideoCodecConfig& config) override;
    bool Encode(const uint8_t* yuvData,
                size_t yuvSize,
                std::vector<uint8_t>& outData,
                bool& isKeyFrame,
                uint64_t timestamp) override;
    void UpdateBitrate(uint32_t bitrate) override;
    void RequestKeyFrame() override;
    CodecCapability GetCapability() const override;
    CodecBackend GetBackend() const override;
    void Release() override;

private:
    struct Impl;
    Impl* impl_;
};

class FFmpegDecoder : public IVideoDecoder {
public:
    FFmpegDecoder();
    ~FFmpegDecoder() override;

    bool Initialize(const VideoCodecConfig& config) override;
    bool Decode(const uint8_t* encodedData,
                size_t encodedSize,
                bool isKeyFrame,
                uint8_t* yuvData,
                size_t& yuvSize,
                uint32_t& width,
                uint32_t& height) override;
    void PacketLossConceal() override;
    CodecCapability GetCapability() const override;
    CodecBackend GetBackend() const override;
    void Release() override;

private:
    struct Impl;
    Impl* impl_;
};

// ============================================================
// Creator functions
// ============================================================
std::unique_ptr<IVideoEncoder> CreateFFmpegEncoder();
std::unique_ptr<IVideoDecoder> CreateFFmpegDecoder();

}}}} // namespace vcs::media::codec::software
