#pragma once

#include "common_export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vcs {

/**
 * @brief 媒体配置公共接口
 *
 * 定义跨模块共享的媒体配置类型。
 * 这些类型在 common 层定义，供 gateway、conference、media 等模块使用。
 *
 * 设计原则：
 * 1. 模块间通过接口交互，不直接调用其他模块内部类型
 * 2. 共享的配置类型定义在公共层（common）
 * 3. Media 模块内部实现使用自己的类型，但接口层使用公共类型
 */
namespace media_config {

	/**
	 * @brief Layout selection mode
	 */
	enum class LayoutSelectMode {
		Manual = 0,           // Manual mode - user manually sets layout
		VoiceActivation = 1,  // Voice activation mode - selects based on VAD
		Presenter = 2,        // Presenter mode - presenter occupies main region
		Gallery = 3,          // Gallery mode - equally displays
		Adaptive = 4,         // Adaptive mode - auto selects based on participant count
	};


/**
 * @brief 编解码后端类型
 */
enum class CodecBackend {
    Software, // 软件编解码
    Hardware,    // 硬件编解码
    Remote       // 远程编解码
};

/**
 * @brief 视频编解码类型
 */
enum class VideoCodecType {
    H264,
    VP8,
    VP9,
    H265
};

/**
 * @brief 音频编解码类型
 */
enum class AudioCodecType {
    G711ulaw,
    G711alaw,
    G722,
    G728,
    G729,
    OPUS,
    AAC
};

/**
 * @brief SRTP 加密模式
 */
enum class SrtpMode {
    Disabled = 0,   // 禁用 SRTP（默认，兼容性考虑）
    Optional = 1,   // 可选，协商决定
    Mandatory = 2   // 强制，必须使用 SRTP
};

/**
 * @brief 视频编解码配置
 */
struct VideoCodecConfig {
    VideoCodecType codecType = VideoCodecType::H264;
    uint16_t width = 1280;           // 720p
    uint16_t height = 720;
    uint16_t framerate = 30;
    uint32_t bitrate = 1000000;      // 1 Mbps
    uint16_t keyFrameInterval = 60;  // GOP
    CodecBackend backend = CodecBackend::Software;
    std::string profileLevelId;      // H.264 Profile/Level
    std::string spropParameterSets;   // SPS/PPS
};

/**
 * @brief 音频编解码配置
 */
struct AudioCodecConfig {
    AudioCodecType codecType = AudioCodecType::G722;
    uint32_t sampleRate = 16000;
    uint8_t channels = 1;
    uint32_t bitrate = 64000;
    CodecBackend backend = CodecBackend::Software;
};

/**
 * @brief SRTP encryption configuration
 */
struct SrtpConfig {
    SrtpMode mode = SrtpMode::Disabled;  // Default disabled
    bool requireSecureSignaling = false; // Whether to require encrypted signaling
};

// ============================================================
// Q2 + Q5 additions: media-resource configuration
// ============================================================
//
// These fields are consumed by IMediaControl::SetupMediaResources
// (Q2 Section 17) and IRtpBackend (Q5 Section 20). The legacy VideoCodecConfig /
// AudioCodecConfig above remain unchanged for backward compatibility.

struct VideoSinkConfig {
	uint32_t width = 1280;
	uint32_t height = 720;
	void* nativeWindow = nullptr;   // HWND / NSView* / XID
};

struct AudioSinkConfig {
	uint32_t sampleRate = 8000;
	uint8_t channels = 1;
	std::string deviceId;
};

/**
 * @brief Top-level per-conference media-resource configuration.
 *
 * Passed to IMediaControl::SetupMediaResources. The conference module is
 * the SOLE producer; media is a pure consumer.
 */
struct MediaResourceConfig {
    // ---- audio / video enable flags ----
    bool enableAudio    = true;
    bool enableVideo    = true;
    bool enableRtcp     = true;

    // ---- bandwidth ----
    uint32_t maxBandwidth = 4000000;   // bps

    // ---- RTP payload types (filled by gateway after SDP negotiation) ----
    uint8_t audioPayloadType = 0;        // 0 = PCMU default
    uint8_t videoPayloadType = 97;       // 97 = dynamic H.264 default

    // ---- bind address (empty = INADDR_ANY) ----
    std::string bindAddress;

    // ---- handler-chain enable list (Q3 Section 18.3) ----
    // Names of handlers that should be active for this conference.
    // Empty means "all default handlers"; otherwise only the listed ones run.
    // Examples: "Decode", "Layout", "Compose", "Encode", "Egress"
    std::vector<std::string> enabledHandlers;

    // ---- Q1 Section 16.5: per-conference async worker sizing ----
    // MediaWorker keeps one SPSC<MediaPacket> lane per MediaStreamKind.
    // Only the VIDEO lane is tunable here: it dominates memory and burstiness.
    // The audio lane (16/8) and the aux lane (128/16) are fixed by their
    // latency budgets - audio must never wait behind a video IDR burst, so
    // letting it be enlarged would only add delay.
    // queueDepth is rounded up to a power of two inside SpscQueue.
    // 0 means "use the MediaWorker default" (256 / 32).
    uint32_t workerVideoQueueDepth = 256;
    uint32_t workerVideoBatchSize  = 32;



	// ---- 新增：主流设备 ----
	std::string mainVideoSourceId;
	std::string mainVideoSinkId;
	VideoSinkConfig mainVideoSinkConfig;

	// ---- 新增：辅流设备 ----
	std::string secondaryVideoSourceId;
	std::string secondaryVideoSinkId;
	VideoSinkConfig secondaryVideoSinkConfig;

	// ---- 新增：音频设备 ----
	std::string audioSourceId;
	std::string audioSinkId;
	AudioSinkConfig audioSinkConfig;
};

} // namespace media_config
} // namespace vcs
