#pragma once

#include "codec/ICodec.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ILogger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
}

namespace vcs { namespace media { namespace codec { namespace software {

// 声道布局的 FFmpeg API 在 5.1 换成了 AVChannelLayout（libavutil 57.28），
// 旧的 channels/channel_layout 字段在 7.0 已删除。版本差异只允许出现在这里：
// 调用点一律走这两个函数，不再直接碰字段（docs/06 §6 的隔离点规则）。
namespace ffmpeg_version_compat {

inline void SetCodecChannels(AVCodecContext* ctx, int channels) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    av_channel_layout_default(&ctx->ch_layout, channels);
#else
    ctx->channels = channels;
    ctx->channel_layout = (channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
}

inline void SetFrameChannels(AVFrame* frame, int channels) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    av_channel_layout_default(&frame->ch_layout, channels);
#else
    frame->channels = channels;
    frame->channel_layout = (channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
}

} // namespace ffmpeg_version_compat

// ============================================================
// G.711 mu-law / A-law codec (FFmpeg-backed, ITU-T reference)
// ============================================================

class G711Encoder : public IAudioEncoder {
public:
    G711Encoder() : initialized_(false), channels_(1), ctx_(nullptr) {}

    ~G711Encoder() override {
        Release();
    }

    bool Initialize(const AudioCodecConfig& config) override {
        codecName_ = AudioCodecTypeToString(config.codecType);
        channels_ = config.channels;

        const char* encoderName = (config.codecType == AudioCodecType::G711A) ? "pcm_alaw" : "pcm_mulaw";
        const AVCodec* codec = avcodec_find_encoder_by_name(encoderName);
        if (!codec) {
            VCS_MEDIA_LOG_ERROR("[G711Encoder] FFmpeg encoder not found: %s", encoderName);
            return false;
        }

        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) {
            VCS_MEDIA_LOG_ERROR("[G711Encoder] failed to allocate codec context");
            return false;
        }

        ctx_->sample_fmt = AV_SAMPLE_FMT_S16;
        ctx_->sample_rate = config.sampleRate > 0 ? config.sampleRate : 8000;
        ffmpeg_version_compat::SetCodecChannels(ctx_, channels_);
        ctx_->bit_rate = 64000 * channels_;

        int ret = avcodec_open2(ctx_, codec, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            VCS_MEDIA_LOG_ERROR("[G711Encoder] avcodec_open2 failed: %s", errbuf);
            avcodec_free_context(&ctx_);
            return false;
        }

        initialized_ = true;
        VCS_MEDIA_LOG_INFO("[G711Encoder] initialized: codec=%s, rate=%d, channels=%d",
                           encoderName, ctx_->sample_rate, channels_);
        return true;
    }

    bool Encode(const AudioPcmFrame& pcmFrame,
                std::vector<uint8_t>& outData,
                uint64_t& timestamp) override {
        if (!initialized_ || !pcmFrame.data || pcmFrame.samples == 0 || !ctx_) {
            return false;
        }

        timestamp = pcmFrame.timestamp;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;

        frame->nb_samples = pcmFrame.samples;
        frame->format = AV_SAMPLE_FMT_S16;
        ffmpeg_version_compat::SetFrameChannels(frame, channels_);
        frame->sample_rate = ctx_->sample_rate;

        int ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            av_frame_free(&frame);
            return false;
        }

        // Copy PCM data into AVFrame
        const int data_size = pcmFrame.samples * channels_ * sizeof(int16_t);
        std::memcpy(frame->data[0], pcmFrame.data, data_size);

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            av_frame_free(&frame);
            return false;
        }

        ret = avcodec_send_frame(ctx_, frame);
        if (ret < 0) {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            return false;
        }

        ret = avcodec_receive_packet(ctx_, pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            return false;
        }

        if (ret == 0 && pkt->size > 0) {
            outData.assign(pkt->data, pkt->data + pkt->size);
        } else {
            outData.clear();
        }

        av_packet_free(&pkt);
        av_frame_free(&frame);
        return true;
    }

    CodecCapability GetCapability() const override {
        CodecCapability cap;
        cap.codecName = codecName_;
        cap.backend = CodecBackend::Software;
        cap.supportSimd = true;
        cap.maxResolution = 48000;
        cap.maxBitrate = 64000;
        cap.priority = 100;
        return cap;
    }

    CodecBackend GetBackend() const override {
        return CodecBackend::Software;
    }

    void Release() override {
        if (ctx_) {
            avcodec_free_context(&ctx_);
            ctx_ = nullptr;
        }
        initialized_ = false;
    }

private:
    std::string codecName_;
    bool initialized_;
    uint8_t channels_;
    AVCodecContext* ctx_;
};

class G711Decoder : public IAudioDecoder {
public:
    G711Decoder() : initialized_(false), channels_(1), sampleRate_(8000), ctx_(nullptr) {}

    ~G711Decoder() override {
        Release();
    }

    bool Initialize(const AudioCodecConfig& config) override {
        codecName_ = AudioCodecTypeToString(config.codecType);
        channels_ = config.channels;
        sampleRate_ = config.sampleRate > 0 ? config.sampleRate : 8000;

        const char* decoderName = (config.codecType == AudioCodecType::G711A) ? "pcm_alaw" : "pcm_mulaw";
        const AVCodec* codec = avcodec_find_decoder_by_name(decoderName);
        if (!codec) {
            VCS_MEDIA_LOG_ERROR("[G711Decoder] FFmpeg decoder not found: %s", decoderName);
            return false;
        }

        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) {
            VCS_MEDIA_LOG_ERROR("[G711Decoder] failed to allocate codec context");
            return false;
        }

        ctx_->sample_fmt = AV_SAMPLE_FMT_S16;
        ctx_->sample_rate = sampleRate_;
        ffmpeg_version_compat::SetCodecChannels(ctx_, channels_);

        int ret = avcodec_open2(ctx_, codec, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            VCS_MEDIA_LOG_ERROR("[G711Decoder] avcodec_open2 failed: %s", errbuf);
            avcodec_free_context(&ctx_);
            return false;
        }

        initialized_ = true;
        VCS_MEDIA_LOG_INFO("[G711Decoder] initialized: codec=%s, rate=%d, channels=%d",
                           decoderName, ctx_->sample_rate, channels_);
        return true;
    }

    bool Decode(const uint8_t* encodedData,
                size_t encodedSize,
                AudioPcmFrame& outFrame) override {
        if (!initialized_ || !encodedData || encodedSize == 0 || !ctx_) {
            return false;
        }

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return false;

        pkt->data = const_cast<uint8_t*>(encodedData);
        pkt->size = encodedSize;

        int ret = avcodec_send_packet(ctx_, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            return false;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            av_packet_free(&pkt);
            return false;
        }

        ret = avcodec_receive_frame(ctx_, frame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            av_frame_free(&frame);
            av_packet_free(&pkt);
            return false;
        }

        if (ret == 0 && frame->nb_samples > 0) {
            const int sample_count = frame->nb_samples * channels_;
            try {
                outFrame.data = new int16_t[sample_count];
                std::memcpy(outFrame.data, frame->data[0], sample_count * sizeof(int16_t));
            } catch (const std::bad_alloc&) {
                VCS_MEDIA_LOG_ERROR("[G711Decoder] failed to allocate %d samples", sample_count);
                av_frame_free(&frame);
                av_packet_free(&pkt);
                return false;
            }
            outFrame.samples = sample_count;
            outFrame.sampleRate = sampleRate_;
            outFrame.channels = channels_;
            outFrame.timestamp = 0;
        } else {
            outFrame.data = nullptr;
            outFrame.samples = 0;
        }

        av_frame_free(&frame);
        av_packet_free(&pkt);
        return true;
    }

    CodecCapability GetCapability() const override {
        CodecCapability cap;
        cap.codecName = codecName_;
        cap.backend = CodecBackend::Software;
        cap.supportSimd = true;
        cap.maxResolution = 48000;
        cap.priority = 100;
        return cap;
    }

    CodecBackend GetBackend() const override {
        return CodecBackend::Software;
    }

    void PacketLossConceal(size_t samples) override {
        (void)samples;
    }

    void Release() override {
        if (ctx_) {
            avcodec_free_context(&ctx_);
            ctx_ = nullptr;
        }
        initialized_ = false;
    }

private:
    std::string codecName_;
    bool initialized_;
    uint8_t channels_;
    uint32_t sampleRate_;
    AVCodecContext* ctx_;
};

// ============================================================
// Creator functions
// ============================================================
std::unique_ptr<IAudioEncoder> CreateG711Encoder() {
    return std::unique_ptr<IAudioEncoder>(new G711Encoder());
}

std::unique_ptr<IAudioDecoder> CreateG711Decoder() {
    return std::unique_ptr<IAudioDecoder>(new G711Decoder());
}

}}}} // namespace vcs::media::codec::software
