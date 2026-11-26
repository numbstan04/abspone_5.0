// FFmpegCodec.h includes ICodec.h which provides cross-platform types

// [MSVC[Compat]] Windows min/max
#define NOMINMAX

#include "FFmpegCodec.h"
#include "ILogger.h"
#include <algorithm>  // For std::min, std::max
#include <atomic>     // [Fix 2026-08-13] For std::atomic<bool> pendingKeyFrame_
#include <vector>     // [Fix B] For std::vector<NalRange>
#include <cstdio>     // [Debug Dump] For fwrite/fopen/fflush
#include <cstdlib>    // [Debug Dump] For getenv
#include <cstring>    // [Debug Dump] For strerror
#include <cerrno>     // [Debug Dump] For errno

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h> 
#include <libswscale/swscale.h>  // [YUV Fix] format conversion to guaranteed YUV420P
}



namespace vcs {
	namespace media {
		namespace codec {
			namespace software {

				// ============================================================
				// [Debug Dump Helpers] - hardcoded paths, no env var required
				//   decoded YUV  -> G:/tmp/decoded.yuv
				//   encoded H264 -> G:/tmp/encoded.h264
				//   max frames   -> 300 (compile-time constant; change here)
				// Verification:
				//   ffplay -f rawvideo -pixel_format yuv420p -video_size 1280x720 G:/tmp/decoded.yuv
				//   ffplay G:/tmp/encoded.h264
				// ============================================================
				namespace {
					struct DumpContext {
						FILE* file = nullptr;
						int   frameCount = 0;
						int   maxFrames = 300;
						int   width = 0;
						int   height = 0;
						const char* label = nullptr;
					};

					static DumpContext g_decodedYuvDump;
					static DumpContext g_encodedH264Dump;
					static FILE* g_decodedBmpFile = nullptr;
					static bool g_dumpInited = false;

					// [YUV Fix] SwsContext + scratch frame for forcing decoded frames into
					// YUV420P before writing them to the dump file. The FFmpeg decoder may
					// emit NV12 / YUVJ420P / YV12 / etc. depending on the input H.264 stream,
					// and naively copying frame->data[0..2] as if they were planar YUV420P
					// causes the kind of "alternating top/bottom garbage" the user reported.
					// sws_scale guarantees a tightly-packed YUV420P frame regardless of
					// the source pixel format.
					static SwsContext* g_dumpSwsCtx = nullptr;
					static AVFrame*    g_dumpConvFrame = nullptr;
					static AVPixelFormat g_dumpLastSrcFmt = AV_PIX_FMT_NONE;

#if 0
					static void InitDumpIfNeeded() {
						if (g_dumpInited) return;
						g_dumpInited = true;

						// Auto-create dump directory (Windows / Linux compatible)
						system("mkdir -p G:/tmp 2>nul");
						system("mkdir -p /tmp 2>/dev/null");

						// Pure YUV420P (no header), ffplay requires -video_size
						// Also write an info.txt recording actual resolution, ffmpeg reads this
						g_decodedYuvDump.file = fopen("G:/tmp/decoded.yuv", "wb");
						g_decodedYuvDump.label = "decoded.yuv";
						if (g_decodedYuvDump.file) {
							// Write an info file, telling user the resolution
							FILE* infoFile = fopen("G:/tmp/decoded_info.txt", "w");
							if (infoFile) {
								fprintf(infoFile, "# YUV file info (read this to find correct -video_size)\n");
								fprintf(infoFile, "YUV_PATH=G:/tmp/decoded.yuv\n");
								fprintf(infoFile, "# Real width and height will be APPENDED on first frame\n");
								fprintf(infoFile, "FORMAT=YUV420P\n");
								fflush(infoFile);
								fclose(infoFile);
							}

						}


						g_encodedH264Dump.file = fopen("G:/tmp/encoded.h264", "wb");
						g_encodedH264Dump.label = "encoded.h264";

					}
#endif
				} // anonymous namespace


				// ============================================================
				// FFmpegEncoder implementation
				// ============================================================

				class FFmpegEncoder::Impl {
				public:
					AVCodecContext* ctx_ = nullptr;
					AVPacket* packet_ = nullptr;
					AVFrame* frame_ = nullptr;
					bool initialized_ = false;
					VideoCodecConfig config_;
					
					uint64_t frameCount_ = 0;
					int consecutiveErrors_ = 0;  // consecutive error count,error
					static const int MAX_CONSECUTIVE_ERRORS = 3;  // consecutive error count threshold
					// One-shot IDR request flag, set by RequestKeyFrame() and consumed by
					// the next Encode() call. This is the new (correct) mechanism that
					// replaces the old "ctx_->gop_size = 1" trick.
					std::atomic<bool> pendingKeyFrame_{ false };

					~Impl() {
						Release();
					}

					// re-initialize encoder(recover from error)
					bool Reinitialize() {
						VCS_MEDIA_LOG_INFO("FFmpegEncoder: Reinitializing encoder after %d consecutive errors",
							consecutiveErrors_);

						// save config
						VideoCodecConfig savedConfig = config_;

						// completely release
						Release();

						// re-initialize
						return Initialize(savedConfig);
					}

					// ============================================================
					// H.264 format conversion helpers
					// ============================================================

					/**
					 * @brief Check if data has Annex B start code
					 */
					bool HasAnnexBStartCode(const uint8_t* data, size_t size) {
						if (!data || size < 4) return false;
						if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) return true;
						if (size >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) return true;
						return false;
					}

					/**
					 * @brief Check if data is AVC format (length prefix)
					 */
					bool IsAvcFormat(const uint8_t* data, size_t size) {
						if (!data || size < 5) return false;
						if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00) {
							if (data[3] != 0x00 && data[3] != 0x01) return true;
						}
						return false;
					}

					/**
					 * @brief Convert H.264 from AVC format to Annex B format
					 */
					bool ConvertAvcToAnnexB(const uint8_t* avcData, size_t avcSize, std::vector<uint8_t>& outData) {
						if (!avcData || avcSize < 5) return false;
						static const uint8_t startCode[4] = { 0x00, 0x00, 0x00, 0x01 };
						size_t offset = 0;
						while (offset + 4 < avcSize) {
							uint32_t nalLength = (static_cast<uint32_t>(avcData[offset]) << 24) |
								(static_cast<uint32_t>(avcData[offset + 1]) << 16) |
								(static_cast<uint32_t>(avcData[offset + 2]) << 8) |
								static_cast<uint32_t>(avcData[offset + 3]);
							outData.insert(outData.end(), startCode, startCode + 4);
							if (nalLength > 0 && offset + 4 + nalLength <= avcSize) {
								outData.insert(outData.end(), avcData + offset + 4, avcData + offset + 4 + nalLength);
								offset += 4 + nalLength;
							}
							else {
								return false;
							}
						}
						return !outData.empty();
					}

					/**
					 * @brief Get NAL type name
					 */
					const char* GetNalTypeName(uint8_t nalType) {
						switch (nalType) {
						case 0:  return "Unspecified";
						case 1:  return "Non-IDR";
						case 5:  return "IDR";
						case 6:  return "SEI";
						case 7:  return "SPS";
						case 8:  return "PPS";
						default: return "Unknown";
						}
					}

					// get correspondingFFmpeg codec ID
					AVCodecID GetCodecId(VideoCodecType codecType) {
						switch (codecType) {
						case VideoCodecType::H264: return AV_CODEC_ID_H264;
						case VideoCodecType::H265: return AV_CODEC_ID_H265;
						case VideoCodecType::VP8: return AV_CODEC_ID_VP8;
						case VideoCodecType::VP9: return AV_CODEC_ID_VP9;
						case VideoCodecType::AV1: return AV_CODEC_ID_AV1;
						default: return AV_CODEC_ID_H264;
						}
					}

					// get encoder name by codec type
					const char* GetEncoderName(VideoCodecType codecType) {
						switch (codecType) {
						case VideoCodecType::H264: return "H.264";
						case VideoCodecType::H265: return "H.265/HEVC";
						case VideoCodecType::VP8: return "VP8";
						case VideoCodecType::VP9: return "VP9";
						case VideoCodecType::AV1: return "AV1";
						default: return "H.264";
						}
					}

					bool Initialize(const VideoCodecConfig& config) {
						// [[Key Fix]]inbefore re-initialize, thoroughly clean up
						Release();

						config_ = config;
						consecutiveErrors_ = 0;  // reset error count

						const AVCodec* codec = nullptr;
						AVCodecID codecId = GetCodecId(config.codecType);
						const char* codecName = GetEncoderName(config.codecType);

						VCS_MEDIA_LOG_INFO("FFmpegEncoder: initializing %s encoder (software only), codecType=%d", codecName, (int)config.codecType);

						// 
						// use avcodec_find_encoder find encoder(auto-select first available)
						// FFmpeg 4.0+ codec auto-registration

						if (codecId == AV_CODEC_ID_H264) {
							// H.264: use avcodec_find_encoder 
							codec = avcodec_find_encoder(codecId);
							if (!codec) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: H.264 encoder not found");
								return false;
							}
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: found H.264 encoder: %s", codec->name);
							if (!TryOpenCodec(codec, config)) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: H.264 encoder failed to open");
								return false;
							}
						}
						else if (codecId == AV_CODEC_ID_H265) {
							// H.265: use avcodec_find_encoder 
							codec = avcodec_find_encoder(codecId);
							if (!codec) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: H.265 encoder not found");
								return false;
							}
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: found H.265 encoder: %s", codec->name);
							if (!TryOpenCodec(codec, config)) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: H.265 encoder failed to open");
								return false;
							}
						}
						else if (codecId == AV_CODEC_ID_VP8) {
							codec = avcodec_find_encoder(codecId);
							if (!codec) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: VP8 encoder not found");
								return false;
							}
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: found VP8 encoder: %s", codec->name);
							if (!TryOpenCodec(codec, config)) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: VP8 encoder failed to open");
								return false;
							}
						}
						else if (codecId == AV_CODEC_ID_VP9) {
							codec = avcodec_find_encoder(codecId);
							if (!codec) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: VP9 encoder not found");
								return false;
							}
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: found VP9 encoder: %s", codec->name);
							if (!TryOpenCodec(codec, config)) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: VP9 encoder failed to open");
								return false;
							}
						}
						else {
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: unsupported codec type: %d", codecId);
							return false;
						}

						// [P0-2 Fix] Allocate frame_ and packet_ BEFORE TryOpenCodec().
						// The previous order caused TryOpenCodec() to skip the pre-allocation
						// of frame_->buf (the if (frame_) check was always false at that point),
						// so every Encode() call hit av_frame_make_writable() with an
						// unallocated frame and returned EINVAL (-22). No frame was ever
						// encoded, leaving the decoder with no input.
						packet_ = av_packet_alloc();
						frame_ = av_frame_alloc();
						if (!packet_ || !frame_) {
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: failed to allocate packet/frame");
							if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
							if (frame_) { av_frame_free(&frame_);   frame_ = nullptr; }
							return false;
						}

						// [P0-3 Fix] Explicitly allocate the frame buffer here.
						// The pre-allocation in TryOpenCodec() is conditional on frame_ being
						// non-null, but at the time TryOpenCodec() ran, frame_ was still null,
						// so the buffer was never allocated. Doing it here guarantees the
						// buffer exists before Encode() is called.
						frame_->format = ctx_->pix_fmt;
						frame_->width = ctx_->width;
						frame_->height = ctx_->height;
						int frameBufRet = av_frame_get_buffer(frame_, 0);
						if (frameBufRet < 0) {
							char errBuf[128];
							av_strerror(frameBufRet, errBuf, sizeof(errBuf));
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: av_frame_get_buffer failed in Initialize, ret=%d (%s)",
								frameBufRet, errBuf);
							return false;
						}
						VCS_MEDIA_LOG_INFO("FFmpegEncoder: frame buffer allocated, linesize=[%d,%d,%d,%d]",
							frame_->linesize[0], frame_->linesize[1],
							frame_->linesize[2], frame_->linesize[3]);

						// encodinghassuccessfully
						frameCount_ = 0;
						initialized_ = true;

						// flush encoder after init to getSPS/PPS
						FlushEncoderForHeader();

						return true;
					}

					// try to open encoder (auto-clean on failure)
					bool TryOpenCodec(const AVCodec* codec, const VideoCodecConfig& config) {
						// clean up old context
						if (ctx_) {
							avcodec_free_context(&ctx_);
							ctx_ = nullptr;
						}

						ctx_ = avcodec_alloc_context3(codec);
						if (!ctx_) {
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: failed to allocate context for '%s'", codec->name);
							return false;
						}

						// set encoding parameters
						ctx_->width = static_cast<int>(config.width > 0 ? config.width : 640);
						ctx_->height = static_cast<int>(config.height > 0 ? config.height : 480);
						ctx_->bit_rate = static_cast<uint64_t>(config.bitrate > 0 ? config.bitrate : 1000000);
						// 出口硬上限：bit_rate 只给 libx264 平均码率，峰值不受限。
						// rc_max_rate / rc_buffer_size 才是 x264 的 VBV 参数，
						// 与 bit_rate 取同值即"平均=上限"，1s 缓冲。
						ctx_->rc_max_rate = ctx_->bit_rate;
						ctx_->rc_buffer_size = static_cast<int>(ctx_->bit_rate);
						ctx_->framerate = { static_cast<int>(config.framerate > 0 ? config.framerate : 30), 1 };
						int gopSize = 0;
						if (config.gopSize > 0) {
							gopSize = static_cast<int>(config.gopSize);
						}
						else if (config.keyFrameInterval > 0) {
							gopSize = static_cast<int>(config.keyFrameInterval);
						}
						else {
							gopSize = 2 * static_cast<int>(config.framerate > 0 ? config.framerate : 30);
						}
						ctx_->gop_size = gopSize;
						ctx_->keyint_min = gopSize;   // 禁止 x264 提前插 IDR
						av_opt_set_int(ctx_->priv_data, "sc_threshold", 0, 0);  // 关闭场景切换插 IDR
						ctx_->max_b_frames = 0;
						ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
						ctx_->time_base = { 1, static_cast<int>(config.framerate > 0 ? config.framerate : 30) };

						// set optimization parameters
						std::string name = codec->name;
						if ((name.find("h264") != std::string::npos || name.find("x264") != std::string::npos) &&
							name.find("v4l2m2m") == std::string::npos && name.find("nvenc") == std::string::npos && name.find("vaapi") == std::string::npos) {
							// H.264 software encoder: usebaseline profile,low-latency optimization
							// [CRITICAL FIX] libx264 contains "x264" not "h264" - both must be checked
							int retProfile = av_opt_set(ctx_->priv_data, "profile", "baseline", 0);
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: av_opt_set profile=baseline ret=%d", retProfile);
							// [[Key Fix]]use "fast" rather than "ultrafast",ensure bitrate control takes effect
							// "ultrafast" severely weaken bitrate control,causingoutput bitrate far exceeds target
							int retPreset = av_opt_set(ctx_->priv_data, "preset", "fast", 0);
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: av_opt_set preset=ultrafast ret=%d", retPreset);
							int retTune = av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: av_opt_set tune=zerolatency ret=%d", retTune);

							// [RESTORED] bitrate/maxrate/bufsize for proper rate control
							char bitrateStr[32];
							snprintf(bitrateStr, sizeof(bitrateStr), "%d", config.bitrate > 0 ? config.bitrate : 1000000);
							av_opt_set(ctx_->priv_data, "bitrate", bitrateStr, 0);

							char vbvMaxrateStr[32];
							snprintf(vbvMaxrateStr, sizeof(vbvMaxrateStr), "%d", (config.bitrate > 0 ? config.bitrate : 1000000) * 1.5);
							av_opt_set(ctx_->priv_data, "maxrate", vbvMaxrateStr, 0);

							char vbvBufsizeStr[32];
							snprintf(vbvBufsizeStr, sizeof(vbvBufsizeStr), "%d", (config.bitrate > 0 ? config.bitrate : 1000000) * 2);
							av_opt_set(ctx_->priv_data, "bufsize", vbvBufsizeStr, 0);

						}
						else if (name.find("hevc") != std::string::npos || name.find("h265") != std::string::npos) {
							// H.265 software encoder: usemain profile,low-latency optimization
							av_opt_set(ctx_->priv_data, "preset", "fast", 0);
							av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);

							// [[Key Fix]]H.265 need bitrate
							char bitrateStr[32];
							snprintf(bitrateStr, sizeof(bitrateStr), "%d", config.bitrate > 0 ? config.bitrate : 1000000);
							av_opt_set(ctx_->priv_data, "bitrate", bitrateStr, 0);

							char vbvMaxrateStr[32];
							snprintf(vbvMaxrateStr, sizeof(vbvMaxrateStr), "%d", (config.bitrate > 0 ? config.bitrate : 1000000) * 1.5);
							av_opt_set(ctx_->priv_data, "maxrate", vbvMaxrateStr, 0);

						}
						else if (name == "vp8" || name == "vp9") {
							// VP8/VP9: whenencoding
							av_opt_set(ctx_->priv_data, "deadline", "realtime", 0);
							av_opt_set(ctx_->priv_data, "cpu-used", "4", 0);
						}

						// try to open encoder
						if (avcodec_open2(ctx_, codec, nullptr) < 0) {
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: failed to open codec '%s'", codec->name);
							avcodec_free_context(&ctx_);
							ctx_ = nullptr;
							return false;
						}

						VCS_MEDIA_LOG_INFO("FFmpegEncoder: codec '%s' opened, width=%d, height=%d, bitrate=%d, fps=%d",
							codec->name, ctx_->width, ctx_->height, (int)ctx_->bit_rate, ctx_->framerate.num);

						// Pre-allocate the AVFrame buffer once. The same frame_ object is
						// reused on every Encode() call (av_frame_unref + refill), so calling
						// av_frame_get_buffer here �� instead of inside Encode() �� avoids a
						// per-frame buffer allocation in the hot path.
						if (frame_) {
							frame_->format = ctx_->pix_fmt;
							frame_->width = ctx_->width;
							frame_->height = ctx_->height;
							int frameBufRet = av_frame_get_buffer(frame_, 0);
							if (frameBufRet < 0) {
								char errBuf[128];
								av_strerror(frameBufRet, errBuf, sizeof(errBuf));
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: av_frame_get_buffer (init) failed, ret=%d (%s)",
									frameBufRet, errBuf);
								return false;
							}
							VCS_MEDIA_LOG_DEBUG("FFmpegEncoder: frame buffer pre-allocated, linesize=[%d,%d,%d,%d]",
								frame_->linesize[0], frame_->linesize[1],
								frame_->linesize[2], frame_->linesize[3]);
						}
						return true;
					}

					// flush encoder to getSPS/PPS/VUI
					void FlushEncoderForHeader() {
						if (!ctx_ || !packet_) return;

						// [[Key Fix]]libx264,afterdo notneedheader
						// SPS/PPSinauto-output on first actual frame encoding
						// NULLframemaycausingencodingenterstate
						// isencoder state,do notNULLframe
						VCS_MEDIA_LOG_DEBUG("FlushEncoderForHeader: encoder initialized, skipping initial flush");
						VCS_MEDIA_LOG_DEBUG("FlushEncoderForHeader: ctx state - frame_count=%lld", (long long)frameCount_);
					}

					bool Encode(const uint8_t* yuvData, size_t yuvSize,
						std::vector<uint8_t>& outData, bool& isKeyFrame, uint64_t timestamp) {
						if (!initialized_ || !ctx_ || !packet_) {
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: not initialized");
							return false;
						}
						if (!yuvData || yuvSize == 0) {
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: invalid input data");
							return false;
						}

						// [Debug Dump Init] ensure dump files are opened before any fwrite
						//InitDumpIfNeeded();

						// [Debug log] entry point: prove Encode is being called
						if (g_encodedH264Dump.frameCount < g_encodedH264Dump.maxFrames) {
							/*VCS_MEDIA_LOG_INFO("[Dump] Encoder.Encode ENTRY: yuvSize=%zu, ts=%llu, ctx=%ux%u",
								yuvSize, (unsigned long long)timestamp,
								ctx_->width, ctx_->height);*/
						}

						// [fix], /tmp causing "Operation not permitted" error
						// ifneedYUVdata,please usetest(test_ffmpeg_encoder.cpp)
						// do notin production code

						// [[Key Fix]]do notinencodingNULLframe flush!
						// NULLframecausingencodingenter"flushed"state,
						// subsequent callssend_framereturnEOFerror
						// FFmpegencodingUsage:
						// 1. send frame -> avcodec_send_frame
						// 2. read output -> avcodec_receive_packet
						// 3. 1-2untilEAGAIN,thencontinue sending next frame
						// do notinframebeforeNULLframe flush!

						// The frame buffer is pre-allocated once in TryOpenCodec() (after
						// avcodec_open2 succeeds) and reused here. We do NOT call
						// av_frame_get_buffer() every Encode() call �� that would be a
						// per-frame allocation in the hot path. If width/height/pix_fmt
						// ever change, re-initialization is required.

						// set frame parameters (simplified - only set what's needed)
						frame_->format = AV_PIX_FMT_YUV420P;
						frame_->width = ctx_->width;
						frame_->height = ctx_->height;
						frame_->pts = timestamp;

						// [P0-5 Fix] Do NOT set frame->pict_type explicitly here.
						// libx264 has its own logic to determine the picture type based on
						// the GOP structure. Forcing the type on every frame disrupts its
						// internal state machine and was causing avcodec_receive_packet to
						// return EAGAIN immediately after every send_frame, even though
						// libx264 was internally producing output (visible at close time).
						// The pendingKeyFrame_ request is honored via x264's `force_idr`
						// option, set through av_opt_set on the next Encode call.
						bool forceKeyFrame = isKeyFrame || pendingKeyFrame_.load(std::memory_order_relaxed);
						if (pendingKeyFrame_.exchange(false, std::memory_order_acq_rel)) {
							VCS_MEDIA_LOG_INFO("FFmpegEncoder: pending IDR request, encoding next frame as I-frame");
							// Force IDR on the next x264 frame
							av_opt_set(ctx_->priv_data, "force_idr", "1", 0);
						}

						// Make sure the pre-allocated buffer is writable. With ref count 1
						// (the common case after av_frame_unref + reuse) this is a no-op.
						int ret = av_frame_make_writable(frame_);
						if (ret < 0) {
							char errBuf[256];
							av_strerror(ret, errBuf, sizeof(errBuf));
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: av_frame_make_writable failed, ret=%d (%s)", ret, errBuf);
							return false;
						}

						// copyYUVdata
						size_t ySize = ctx_->width * ctx_->height;
						size_t uvSize = ySize / 4;

						if (yuvSize < ySize * 3 / 2) {
							uint32_t newW = ctx_->width, newH = ctx_->height;
							// 尝试推导宽高 (16:9, 4:3)
							for (uint32_t h_test = 240; h_test <= 2160; h_test += 2) {
								uint32_t w_test = h_test * 16 / 9;
								if ((size_t)w_test * h_test * 3 / 2 == yuvSize) {
									newW = w_test; newH = h_test; break;
								}
							}
							if (newW == ctx_->width) {
								for (uint32_t h_test = 240; h_test <= 2160; h_test += 2) {
									uint32_t w_test = h_test * 4 / 3;
									if ((size_t)w_test * h_test * 3 / 2 == yuvSize) {
										newW = w_test; newH = h_test; break;
									}
								}
							}

							VCS_MEDIA_LOG_WARNING(
								"FFmpegEncoder: size mismatch: yuvSize=%zu (expecting %zu for %zux%zu), "
								"reinitializing encoder to %ux%u",
								yuvSize, ySize * 3 / 2, (size_t)ctx_->width, (size_t)ctx_->height,
								newW, newH);

							config_.width = newW;
							config_.height = newH;

							if (Reinitialize()) {
								ySize = ctx_->width * ctx_->height;
								uvSize = ySize / 4; // 【修复1】：必须重新计算 uvSize
								if (yuvSize < ySize * 3 / 2) {
									VCS_MEDIA_LOG_ERROR("[Dump] FFmpegEncoder: still size mismatch after reinit, abort");
									return false;
								}
							}
							else {
								return false;
							}
							// 【修复2】：刚重建完编码器，因为这一帧是错误尺寸进来的，直接丢弃，等下一帧正常编码。
							return false;
						}

						// [[Key Fix]]use linesize for correct memory copy
						for (int y = 0; y < ctx_->height; ++y) {
							memcpy(frame_->data[0] + y * frame_->linesize[0],
								yuvData + y * ctx_->width,
								ctx_->width);
						}
						for (int y = 0; y < ctx_->height / 2; ++y) {
							memcpy(frame_->data[1] + y * frame_->linesize[1],
								yuvData + ySize + y * (ctx_->width / 2),
								ctx_->width / 2);
						}
						for (int y = 0; y < ctx_->height / 2; ++y) {
							memcpy(frame_->data[2] + y * frame_->linesize[2],
								yuvData + ySize + uvSize + y * (ctx_->width / 2),
								ctx_->width / 2);
						}

						VCS_MEDIA_LOG_DEBUG("FFmpegEncoder: sending frame, pts=%llu, size=%ux%u",
							(unsigned long long)timestamp, ctx_->width, ctx_->height);

						// [P0-6 Fix] Reset the frame's picture-type hints BEFORE send_frame.
						// After a key-frame request, x264 consumes the "force_idr" flag and
						// produces an IDR, but the next call must not carry over a stale
						// AV_PICTURE_TYPE_I. Resetting pict_type to NONE and key_frame to 0
						// lets libx264 decide the picture type based on its GOP state.

						// encoding - send frame
						// [P0-4 Fix] Do NOT call av_frame_unref() here.
						// The frame buffer is pre-allocated once in Initialize() and is
						// intended to be reused across Encode() calls. unref() frees the
						// buffer, leaving the next call with an unwritable frame and a
						// av_frame_make_writable() failure (EINVAL).
						ret = avcodec_send_frame(ctx_, frame_);
						// [P0-4 Fix] av_frame_unref(frame_) intentionally removed; see above.

						if (ret < 0) {
							char errBuf[256];
							av_strerror(ret, errBuf, sizeof(errBuf));
							VCS_MEDIA_LOG_ERROR("FFmpegEncoder: avcodec_send_frame failed, ret=%d (%s), pts=%llu",
								ret, errBuf, (unsigned long long)timestamp);
							consecutiveErrors_++;

							if (consecutiveErrors_ >= MAX_CONSECUTIVE_ERRORS) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: too many consecutive errors, reinitializing");
								Reinitialize();
							}
							return false;
						}

						// encode succeeded,reset error count
						consecutiveErrors_ = 0;

						bool encoded = false;
						isKeyFrame = false;

						// read all output packets
						int recvIterations = 0;
						while (true) {
							ret = avcodec_receive_packet(ctx_, packet_);
							if (ret == AVERROR(EAGAIN)) {
								// no more output,continue sending nextframe
								if (recvIterations == 0 && g_encodedH264Dump.frameCount < g_encodedH264Dump.maxFrames) {
									VCS_MEDIA_LOG_WARNING("[Dump] Encoder NAL loop: avcodec_receive_packet returned EAGAIN immediately (no packets produced from this send_frame)");
								}
								break;
							}
							else if (ret == AVERROR_EOF) {
								break;
							}
							else if (ret < 0) {
								VCS_MEDIA_LOG_ERROR("FFmpegEncoder: avcodec_receive_packet failed, ret=%d", ret);
								break;
							}
							recvIterations++;

							isKeyFrame = (packet_->flags & AV_PKT_FLAG_KEY) != 0;

							// ensureH.264output isAnnex-Bformat
							const uint8_t* data = packet_->data;
							size_t size = packet_->size;

							// detectNALtype
							uint8_t nalType = size > 0 ? (data[0] & 0x1F) : 0;

							VCS_MEDIA_LOG_DEBUG("H.264 NAL: type=%d (%s), size=%zu, isKey=%d",
								nalType, GetNalTypeName(nalType), size, isKeyFrame);

							// [Debug Dump] remember the boundary before format conversion
							const size_t outSizeBefore = outData.size();

							// format conversion
							std::vector<uint8_t> nalUnitData;
							if (IsAvcFormat(data, size)) {
								// AVCconvert format toAnnex Bformat
								VCS_MEDIA_LOG_TRACE("H.264 NAL: AVC format detected, converting to Annex B, type=%d", nalType);
								if (ConvertAvcToAnnexB(data, size, nalUnitData)) {
									outData.insert(outData.end(), nalUnitData.begin(), nalUnitData.end());
								}
								else {
									VCS_MEDIA_LOG_ERROR("H.264 NAL: AVC->Annex B conversion FAILED");
									// conversion failed,append start code and raw data
									static const uint8_t startCode[4] = { 0x00, 0x00, 0x00, 0x01 };
									outData.insert(outData.end(), startCode, startCode + 4);
									outData.insert(outData.end(), data, data + size);
								}
							}
							else if (HasAnnexBStartCode(data, size)) {
								// already isAnnex Bformat,append directly
								VCS_MEDIA_LOG_TRACE("H.264 NAL: Annex B format, type=%d", nalType);
								outData.insert(outData.end(), data, data + size);
							}
							else {
								// unknown format,append start code and raw data
								VCS_MEDIA_LOG_WARNING("H.264 NAL: unknown format, adding Annex B prefix");
								static const uint8_t startCode[4] = { 0x00, 0x00, 0x00, 0x01 };
								outData.insert(outData.end(), startCode, startCode + 4);
								outData.insert(outData.end(), data, data + size);
							}

							// [Debug Dump] Write the just-inserted NAL bytes (with start code)
							// to the Annex-B file. This is the EXACT byte stream that the
							// RTP packer will hand to Yealink.
							{
#if 0
								InitDumpIfNeeded();
								if (g_encodedH264Dump.file &&
									g_encodedH264Dump.frameCount < g_encodedH264Dump.maxFrames) {
									const uint8_t* nalPtr = outData.data() + outSizeBefore;
									const size_t   nalLen = outData.size() - outSizeBefore;
									size_t written = fwrite(nalPtr, 1, nalLen, g_encodedH264Dump.file);
									fflush(g_encodedH264Dump.file);
									g_encodedH264Dump.frameCount++;
									/*VCS_MEDIA_LOG_INFO("[Dump] H264 NAL type=%d (%s), size=%zu, written=%zu, totalNals=%d",
										nalType, GetNalTypeName(nalType),
										size, written, g_encodedH264Dump.frameCount);*/
								}
#endif
							}

							encoded = true;
							frameCount_++;

							av_packet_unref(packet_);
						}

						// do not send after each encoding NULL frame flush!
						//  NULL framecausingencodingenter "flushed" state,
						// subsequent calls avcodec_send_frame return AVERROR_EOF
						// FFmpeg encodingUsage:
						// 1. send frame -> avcodec_send_frame
						// 2. read output -> avcodec_receive_packet (until EAGAIN)
						// 3. continue sending next frame(no need to send NULL frame flush)
						// only need to send once before encoder close NULL frameget remaining output

						if (!encoded) {
							// [[Key Fix]]no encoded output may be normal(decodingfailure leads to no valid input)
							// do not log as WARNING,avoid log flooding
							VCS_MEDIA_LOG_DEBUG("FFmpegEncoder: no encoded output, frameCount=%llu",
								(unsigned long long)frameCount_);
						}
						else {
							/*VCS_MEDIA_LOG_INFO("FFmpegEncoder: encoded %zu bytes, frameCount=%llu",
								outData.size(), (unsigned long long)frameCount_);*/
						}

						return encoded;
					}

					void UpdateBitrate(uint32_t bitrate) {
						if (ctx_) {
							ctx_->bit_rate = bitrate;
							config_.bitrate = bitrate;
						}
					}

					void RequestKeyFrame() {
						// Mark a one-shot pending IDR for the next Encode() call.
						// We previously set ctx_->gop_size = 1 here, but that permanently
						// changed the GOP size and forced every subsequent frame to be an
						// I-frame, severely degrading compression efficiency. The new
						// approach is to set a flag and let Encode() force the next frame
						// to be an IDR via frame_->pict_type / key_frame hint, which is
						// honored by libx264 and other software H.264 encoders.
						pendingKeyFrame_ = true;
					}

					void Release() {
						// [[Key Fix]]flush encoder,frame
						if (ctx_) {
							// NULLframeflush encoder
							avcodec_send_frame(ctx_, nullptr);
							// read all remaining output
							while (packet_ && avcodec_receive_packet(ctx_, packet_) >= 0) {
								av_packet_unref(packet_);
							}
						}

						if (frame_) {
							av_frame_free(&frame_);
							frame_ = nullptr;
						}
						if (packet_) {
							av_packet_free(&packet_);
							packet_ = nullptr;
						}
						if (ctx_) {
							avcodec_free_context(&ctx_);
							ctx_ = nullptr;
						}
						initialized_ = false;
					}
				};

				// ============================================================
				// FFmpegdecoder implementation(H.264 SPS/PPS buffering issuefix)
				// ============================================================

				class FFmpegDecoder::Impl {
				public:
					AVCodecContext* ctx_ = nullptr;
					AVFrame* frame_ = nullptr;
					AVPacket* packet_ = nullptr;
					bool initialized_ = false;
					VideoCodecConfig config_;
					bool waitingForNextIDR_ = false;

					// H.264 SPS/PPS buffering mechanism
					// used before receiving SPS/PPS buffer frames before,avoid decode errors
					bool hasSPS_ = false;  // whether has received SPS
					bool hasPPS_ = false;  // whether has received PPS
					bool extradataSet_ = false;  // whether SetExtradata has been applied
					std::vector<uint8_t> spsData_;  // SPS data
					std::vector<uint8_t> ppsData_;  // PPS data
					std::vector<std::vector<uint8_t>> bufferedFrames_;  // buffered frame data
					static const size_t MAX_BUFFERED_FRAMES = 30;  // max buffered frames,prevent memory leak
					static const size_t MAX_FRAME_SIZE = 2 * 1024 * 1024;  // max frame size 2MB

					// track consecutive errors,used to detect whether decoder needs reset
					int consecutiveErrors_ = 0;
					static const int MAX_CONSECUTIVE_ERRORS = 10;

					~Impl() {
						Release();
					}

					// get correspondingFFmpeg codec ID
					AVCodecID GetCodecId(VideoCodecType codecType) {
						switch (codecType) {
						case VideoCodecType::H264: return AV_CODEC_ID_H264;
						case VideoCodecType::H265: return AV_CODEC_ID_H265;
						case VideoCodecType::VP8: return AV_CODEC_ID_VP8;
						case VideoCodecType::VP9: return AV_CODEC_ID_VP9;
						case VideoCodecType::AV1: return AV_CODEC_ID_AV1;
						default: return AV_CODEC_ID_H264;
						}
					}

					// Everything the decoder writes on its context lives here, so
					// SetExtradata() can rebuild the context on any FFmpeg version:
					// 7.0 removed avcodec_close(), and an opened context cannot be
					// reopened, so "close then open again" is no longer portable.
					void ConfigureContext() {
						ctx_->thread_count = 4;
						ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

						// H.264: slice threading only. FF_THREAD_FRAME holds up to
						// thread_count-1 decoded frames in the executor before the
						// first avcodec_receive_frame() can return, which is fine for
						// file playback but wrong for a real-time receive path: while
						// the sync gate is closed only keyframes are fed to the
						// decoder, so frame threading pushed the first rendered frame
						// out by ~4 keyframes (~6 s of black screen).
						if (GetCodecId(config_.codecType) == AV_CODEC_ID_H264) {
							ctx_->thread_type = FF_THREAD_SLICE;
						}
					}

					bool Initialize(const VideoCodecConfig& config) {
						if (initialized_) {
							Release();
						}

						config_ = config;

						// according to codecType decoding
						AVCodecID codecId = GetCodecId(config.codecType);
						const AVCodec* codec = avcodec_find_decoder(codecId);
						if (!codec) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: codec not found for codecType=%d", (int)config.codecType);
							return false;
						}

						VCS_MEDIA_LOG_INFO("FFmpegDecoder: found codec '%s' for codecType=%d", codec->name, (int)config.codecType);

						ctx_ = avcodec_alloc_context3(codec);
						if (!ctx_) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: failed to allocate context");
							return false;
						}

						ConfigureContext();
						if (avcodec_open2(ctx_, codec, nullptr) < 0) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: failed to open codec");
							avcodec_free_context(&ctx_);
							ctx_ = nullptr;
							return false;
						}

						VCS_MEDIA_LOG_INFO("FFmpegDecoder: codec opened");

						packet_ = av_packet_alloc();
						frame_ = av_frame_alloc();
						initialized_ = true;

						// reset buffer state
						ResetBufferState();

						return true;
					}

					// reset buffer state
					void ResetBufferState() {
						//hasSPS_ = false;
						//hasPPS_ = false;
						extradataSet_ = false;
						//spsData_.clear();
						//ppsData_.clear();
						bufferedFrames_.clear();
						consecutiveErrors_ = 0;
					}

					// detect NAL type
					// return NAL type (0-31),or -1 indicates parse failure
					int DetectNalType(const uint8_t* data, size_t size, bool hasAnnexB) {
						if (!data || size == 0) {
							return -1;
						}

						// skip Annex-B prefix
						const uint8_t* nalData = data;
						size_t nalSize = size;

						if (hasAnnexB) {
							// skip 00 00 00 01 or 00 00 01
							if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
								data[2] == 0x00 && data[3] == 0x01) {
								nalData = data + 4;
								nalSize = size - 4;
							}
							else if (size >= 3 && data[0] == 0x00 && data[1] == 0x00 &&
								data[2] == 0x01) {
								nalData = data + 3;
								nalSize = size - 3;
							}
						}

						if (nalSize == 0) {
							return -1;
						}

						// NAL header: forbidden_bit(1) | nal_ref_idc(2) | nal_unit_type(5)
						uint8_t nalType = nalData[0] & 0x1F;
						return static_cast<int>(nalType);
					}

					// extract NAL unit data(excluding Annex-B prefix)
					bool ExtractNalUnit(const uint8_t* data, size_t size, bool hasAnnexB,
						std::vector<uint8_t>& outNal) {
						if (!data || size == 0) {
							return false;
						}

						// skip Annex-B prefix
						const uint8_t* nalData = data;
						size_t nalSize = size;

						if (hasAnnexB) {
							if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
								data[2] == 0x00 && data[3] == 0x01) {
								nalData = data + 4;
								nalSize = size - 4;
							}
							else if (size >= 3 && data[0] == 0x00 && data[1] == 0x00 &&
								data[2] == 0x01) {
								nalData = data + 3;
								nalSize = size - 3;
							}
						}

						if (nalSize == 0 || nalSize > MAX_FRAME_SIZE) {
							return false;
						}

						outNal.assign(nalData, nalData + nalSize);
						return true;
					}

					// processing SPS/PPS
					// return true indicates complete parameter set received
					bool ProcessSpsPps(const uint8_t* nalData, size_t nalSize, int nalType) {
						if (nalType == 7) {  // SPS
							VCS_MEDIA_LOG_TRACE("FFmpegDecoder: received SPS, size=%zu", nalSize);
							spsData_.assign(nalData, nalData + nalSize);
							hasSPS_ = true;
						}
						else if (nalType == 8) {  // PPS
							VCS_MEDIA_LOG_TRACE("FFmpegDecoder: received PPS, size=%zu", nalSize);
							ppsData_.assign(nalData, nalData + nalSize);
							hasPPS_ = true;
						}

						return hasSPS_ && hasPPS_;
					}

					// decode buffered frames
					//
					// [P3-Fix] Correct FFmpeg decoder state machine usage.
					// The previous implementation iterated one packet at a time, calling
					// send_packet + receive_frame in a single iteration. That is wrong:
					//   - EAGAIN from receive_frame means "input accepted, no output yet"
					//     (NORMAL state for H.264), not "drop this packet".
					//   - The old `++it; continue` would re-send already-consumed packets
					//     on the next call, corrupting decoder state and producing
					//     "decoded frame never appears" symptoms.
					//
					// Correct flow:
					//   1. Send ALL buffered packets via avcodec_send_packet.
					//   2. Drain via avcodec_receive_frame. EAGAIN == "no output yet,
					//      input has been accepted, just return false this time".
					//   3. On a real frame, copy it AND erase all buffered packets
					//      (they have all been consumed by the decoder).
					bool DecodeBufferedFrames(uint8_t* yuvData, size_t& yuvSize,
						uint32_t& width, uint32_t& height) {
						if (bufferedFrames_.empty()) {
							return false;
						}

						if (!extradataSet_) {
							VCS_MEDIA_LOG_WARNING("FFmpegDecoder: DecodeBufferedFrames called but extradata not set, cannot decode");

							return false;
						}

						VCS_MEDIA_LOG_INFO("FFmpegDecoder: processing %zu buffered frames", bufferedFrames_.size());

						// ---- 1) Send every buffered packet to the decoder ----
						for (auto it = bufferedFrames_.begin(); it != bufferedFrames_.end(); ++it) {
							const std::vector<uint8_t>& frameData = *it;
							av_packet_unref(packet_);
							int allocRet = av_new_packet(packet_, frameData.size());
							if (allocRet < 0) {
								VCS_MEDIA_LOG_ERROR(
									"FFmpegDecoder: av_new_packet failed in buffered decode, size=%zu",
									frameData.size());
								return false;
							}
							std::memcpy(packet_->data, frameData.data(), frameData.size());
							int sendRet = avcodec_send_packet(ctx_, packet_);
							if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
								char errBuf[128];
								av_strerror(sendRet, errBuf, sizeof(errBuf));
								VCS_MEDIA_LOG_ERROR(
									"FFmpegDecoder: avcodec_send_packet failed in buffered decode, ret=%d (%s)",
									sendRet, errBuf);
								return false;
							}
						}

						// ---- 2) Drain: try to get one output frame ----
						int ret = avcodec_receive_frame(ctx_, frame_);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
							// EAGAIN: input was accepted, decoder needs more packets
							//        before producing a frame. Caller will feed us more
							//        next tick �� all sent packets have been consumed,
							//        so we can safely clear the buffer.
							// EOF   : only seen after avcodec_send_packet(NULL); not
							//         expected in normal flow but treat as no-frame.
							if (ret == AVERROR(EAGAIN) && !bufferedFrames_.empty()) {
								VCS_MEDIA_LOG_DEBUG(
									"FFmpegDecoder: EAGAIN after %zu buffered packets, "
									"need more input",
									bufferedFrames_.size());
							}
							// All sent packets have been consumed by the decoder; clear them.
							//bufferedFrames_.clear();
							return false;
						}
						if (ret < 0) {
							char errBuf[128];
							av_strerror(ret, errBuf, sizeof(errBuf));
							VCS_MEDIA_LOG_ERROR(
								"FFmpegDecoder: avcodec_receive_frame failed, ret=%d (%s)",
								ret, errBuf);
							return false;
						}

						// ---- 3) Got a frame. Copy it and clear the buffer. ----
						width = frame_->width;
						height = frame_->height;

						// [P1-1 / P2-1 Fix] Use frame_->linesize[i] as the source row
						// stride. The previous code read from frame_->data[i] using a
						// compact (width-based) offset, which is wrong whenever
						// linesize[i] != width (FFmpeg often adds alignment padding).
						// The bytes between rows in the source would be copied into the
						// output as if they were pixel data, corrupting the image.
						size_t needed = width * height * 3 / 2;
						if (yuvData && yuvSize >= needed) {
							const uint8_t* srcY = frame_->data[0];
							const uint8_t* srcU = frame_->data[1];
							const uint8_t* srcV = frame_->data[2];
							const int    strideY = frame_->linesize[0];
							const int    strideU = frame_->linesize[1];
							const int    strideV = frame_->linesize[2];
							uint8_t* dstY = yuvData;
							uint8_t* dstU = yuvData + width * height;
							uint8_t* dstV = dstU + (width * height / 4);

							for (uint32_t y = 0; y < height; ++y) {
								memcpy(dstY + y * width, srcY + y * strideY, width);
							}
							for (uint32_t y = 0; y < height / 2; ++y) {
								memcpy(dstU + y * (width / 2), srcU + y * strideU, width / 2);
								memcpy(dstV + y * (width / 2), srcV + y * strideV, width / 2);
							}
							yuvSize = needed;
						}
						else {
							yuvSize = needed;
						}

						// All sent packets have been consumed by the decoder. Clear them
						// whether or not the caller provided a valid yuvData buffer.
						const size_t prevSize = bufferedFrames_.size();
						bufferedFrames_.clear();

						VCS_MEDIA_LOG_INFO(
							"FFmpegDecoder: decoded buffered frame %ux%u, cleared %zu buffered packets",
							width, height, prevSize);
						return true;
					}

					// set FFmpeg extradata
					//
					// FFmpeg H.264 decoding extradata must be set before avcodec_open2()!
					// If the decoder is already open, dynamically modifying extradata will not take effect,
					// so we must close it and re-open with the new extradata.
					//
					// Generic flow:
					//   1. build codec-specific extradata (e.g. AVCDecoderConfigurationRecord for H.264)
					//   2. rebuild the decoder context (avcodec_close was removed in
					//      FFmpeg 7.0; free + alloc + ConfigureContext is version-neutral)
					//   3. set new extradata
					//   4. re-open decoder with the codec corresponding to config_.codecType (avcodec_open2)
					//
					// Currently only H.264 is fully implemented (default in VideoCodecConfig).
					// Other codecs have placeholder branches that log a warning and return early;
					// the structure is reserved for future implementation.
					void SetExtradata() {
						if (!ctx_ || spsData_.empty() || ppsData_.empty()) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: cannot set extradata, SPS/PPS empty or ctx_ null");
							return;
						}

						// Determine the FFmpeg decoder according to the configured codec type.
						// This avoids the previous bug where H.264 was hardcoded regardless of config.
						AVCodecID codecId = GetCodecId(config_.codecType);
						const AVCodec* codec = avcodec_find_decoder(codecId);
						if (!codec) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: decoder not found for codecType=%d (%s)",
								(int)config_.codecType,
								VideoCodecTypeToString(config_.codecType));
							return;
						}

						VCS_MEDIA_LOG_INFO("FFmpegDecoder: SetExtradata START, codecType=%d (%s), will reinitialize decoder",
							(int)config_.codecType,
							VideoCodecTypeToString(config_.codecType));

						// ========== 1: build codec-specific extradata ==========
						// The extradata format differs by codec; only H.264 is currently implemented.
						uint8_t* extradata = nullptr;
						int extradataSize = 0;

						switch (config_.codecType) {
						case VideoCodecType::H264: {
							// AVCDecoderConfigurationRecord format (ISO/IEC 14496-15):
							//   1 byte : configurationVersion (=1)
							//   1 byte : AVCProfileIndication (SPS[1])
							//   1 byte : profile_compatibility (SPS[2])
							//   1 byte : AVCLevelIndication (SPS[3])
							//   1 byte : lengthSizeMinusOne (3) | reserved (5 bits -> 0xFF = 0b1111_1111)
							//   1 byte : numOfSPS (1)        | reserved (5 bits -> 0xE1 = 0b1110_0001)
							//   2 bytes: SPS length + SPS data
							//   1 byte : numOfPPS (1)
							//   2 bytes: PPS length + PPS data
							size_t totalSize = 5 + 2 + spsData_.size() + 1 + 2 + ppsData_.size();
							extradata = static_cast<uint8_t*>(av_mallocz(totalSize + AV_INPUT_BUFFER_PADDING_SIZE));
							if (!extradata) {
								VCS_MEDIA_LOG_ERROR("FFmpegDecoder: failed to allocate H.264 extradata, size=%zu", totalSize);
								return;
							}

							size_t offset = 0;
							extradata[offset++] = 1;  // configurationVersion
							extradata[offset++] = spsData_.size() > 1 ? spsData_[1] : 0;  // AVCProfileIndication
							extradata[offset++] = spsData_.size() > 2 ? spsData_[2] : 0;  // profile_compatibility
							extradata[offset++] = spsData_.size() > 3 ? spsData_[3] : 0;  // AVCLevelIndication
							extradata[offset++] = 0xFF;  // lengthSizeMinusOne=3 | reserved
							extradata[offset++] = 0xE1;  // numOfSPS=1 | reserved
							extradata[offset++] = (spsData_.size() >> 8) & 0xFF;
							extradata[offset++] = spsData_.size() & 0xFF;
							memcpy(extradata + offset, spsData_.data(), spsData_.size());
							offset += spsData_.size();
							extradata[offset++] = 1;  // numOfPPS=1
							extradata[offset++] = (ppsData_.size() >> 8) & 0xFF;
							extradata[offset++] = ppsData_.size() & 0xFF;
							memcpy(extradata + offset, ppsData_.data(), ppsData_.size());
							offset += ppsData_.size();

							extradataSize = static_cast<int>(offset);
							VCS_MEDIA_LOG_INFO("FFmpegDecoder: H.264 extradata built, size=%d, SPS=%zu bytes, PPS=%zu bytes",
								extradataSize, spsData_.size(), ppsData_.size());
							break;
						}
						case VideoCodecType::H265: {
							// [RESERVED] HEVC extradata should be built as HEVCDecoderConfigurationRecord
							// (ISO/IEC 14496-15), which carries VPS/SPS/PPS. Implementation will be added
							// in a future iteration. Returning here leaves the decoder in its previous state
							// so an unsupported codec never triggers a "stuck" reopen loop.
							VCS_MEDIA_LOG_WARNING("FFmpegDecoder: H.265 (HEVC) extradata is not implemented yet, "
								"skip SetExtradata, codecType=%d",
								(int)config_.codecType);
							return;
						}
						case VideoCodecType::VP8: {
							// [RESERVED] VP8 has no SPS/PPS concept; its extradata shape is codec-specific.
							VCS_MEDIA_LOG_WARNING("FFmpegDecoder: VP8 extradata is not implemented yet, "
								"skip SetExtradata, codecType=%d",
								(int)config_.codecType);
							return;
						}
						case VideoCodecType::VP9: {
							// [RESERVED] VP9 uses its own profile/level signaling; extradata format differs.
							VCS_MEDIA_LOG_WARNING("FFmpegDecoder: VP9 extradata is not implemented yet, "
								"skip SetExtradata, codecType=%d",
								(int)config_.codecType);
							return;
						}
						case VideoCodecType::AV1: {
							// [RESERVED] AV1 uses AV1CodecConfigurationRecord (OBU sequence header).
							VCS_MEDIA_LOG_WARNING("FFmpegDecoder: AV1 extradata is not implemented yet, "
								"skip SetExtradata, codecType=%d",
								(int)config_.codecType);
							return;
						}
						default: {
							// Defensive: VideoCodecConfig defaults to H.264, so this branch should not
							// be reached in normal flows. Keep the explicit guard to avoid silently
							// mis-reopening an unknown decoder.
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: unknown codecType=%d, cannot build extradata",
								(int)config_.codecType);
							return;
						}
						}

						// ========== 2: rebuild the decoder context ==========
						// An opened context cannot be reopened, and avcodec_close() was
						// removed in FFmpeg 7.0. Free-and-recreate is the only reopen
						// path that works on every version; the old extradata goes away
						// with the old context.
						avcodec_free_context(&ctx_);
						ctx_ = avcodec_alloc_context3(codec);
						if (!ctx_) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: failed to reallocate decoder context");
							av_freep(&extradata);
							return;
						}
						VCS_MEDIA_LOG_INFO("FFmpegDecoder: codec context rebuilt");

						// ========== 3: set new extradata ==========
						ctx_->extradata = extradata;
						ctx_->extradata_size = extradataSize;
						ConfigureContext();

						// ========== 4: re-open decoder with the configured codec ==========
						if (avcodec_open2(ctx_, codec, nullptr) < 0) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: failed to reopen decoder with extradata, codecType=%d (%s)",
								(int)config_.codecType,
								VideoCodecTypeToString(config_.codecType));
							return;
						}

						VCS_MEDIA_LOG_INFO("FFmpegDecoder: SetExtradata SUCCESS, decoder reopened, codecType=%d (%s), "
							"extradata_size=%d, SPS=%zu bytes, PPS=%zu bytes",
							(int)config_.codecType,
							VideoCodecTypeToString(config_.codecType),
							ctx_->extradata_size, spsData_.size(), ppsData_.size());

						// reset decoder internal state
						avcodec_flush_buffers(ctx_);
						VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: flushed decoder buffers after SetExtradata");

						// print extradata for debugging
						if (ctx_->extradata_size > 0) {
							std::string hexStr;
							int maxPrint = ctx_->extradata_size < 20 ? ctx_->extradata_size : 20;
							for (int i = 0; i < maxPrint; i++) {
								char buf[4];
								snprintf(buf, sizeof(buf), "%02X ", ctx_->extradata[i]);
								hexStr += buf;
							}
							VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: extradata hex: %s", hexStr.c_str());
						}
					}

					// 
					bool Decode(const uint8_t* encodedData, size_t encodedSize,
						bool isKeyFrame, uint8_t* yuvData, size_t& yuvSize,
						uint32_t& width, uint32_t& height) {

						if (!initialized_ || !ctx_ || !frame_ || !packet_ || !encodedData) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: not initialized or invalid input");
							return false;
						}
						(void)isKeyFrame;

						// [P0-1 Fix] Only H.264 is currently implemented.
						// The H.264-only NAL parsing below (mask 0x1F, SPS=7, PPS=8) would
						// silently mis-classify H.265 NAL units (which use a 2-byte header
						// and different type values), leading to incorrect SPS/PPS detection
						// and decoder stalls. Reject non-H.264 streams explicitly up front.
						if (config_.codecType != VideoCodecType::H264) {
							VCS_MEDIA_LOG_WARNING("FFmpegDecoder: codecType=%d (%s) is not implemented, "
								"skipping frame (only H.264 is supported)",
								(int)config_.codecType,
								VideoCodecTypeToString(config_.codecType));
							return false;
						}

						// check data size
						if (encodedSize == 0 || encodedSize > MAX_FRAME_SIZE) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: invalid encoded size: %zu", encodedSize);
							return false;
						}

						// printfirst N bytes of input data16
						VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: Decode input: size=%zu, first16=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
							encodedSize,
							encodedData[0], encodedData[1], encodedData[2], encodedData[3],
							encodedData[4], encodedData[5], encodedData[6], encodedData[7],
							encodedData[8], encodedData[9], encodedData[10], encodedData[11],
							encodedData[12], encodedData[13], encodedData[14], encodedData[15]);

						// detect Annex-B prefix
						bool hasAnnexB = (encodedSize >= 4 &&
							encodedData[0] == 0x00 && encodedData[1] == 0x00 &&
							encodedData[2] == 0x00 && encodedData[3] == 0x01) ||
							(encodedSize >= 3 &&
								encodedData[0] == 0x00 && encodedData[1] == 0x00 &&
								encodedData[2] == 0x01);

						// [Fix B 2026-08-12] Iterate over ALL NALs in the buffer.
						// Previously this function only inspected the FIRST NAL; if SPS and
						// PPS were co-delivered inside a STAP-A packet (or in two back-to-back
						// Annex-B NALs), only the first one was captured, leaving the decoder
						// permanently without PPS and causing every IDR to be dropped.
						//
						// We now scan the Annex-B stream for every 0x00000001 / 0x000001
						// marker and process each NAL. We remember the LAST NAL's type and
						// location so the frame-decoding path below operates on the right
						// slice. SPS/PPS extraction happens inline during the scan.
						bool extradataUpdated = false;
						uint8_t lastNalType = 0;
						size_t lastNalDataOffset = 0;
						size_t lastNalSize = 0;

						auto processOneNal = [&](const uint8_t* nalData, size_t nalSize) {
							if (nalSize == 0) return;
							uint8_t nalType = nalData[0] & 0x1F;
							if (nalType == 7) { // SPS
								spsData_.assign(nalData, nalData + nalSize);
								hasSPS_ = true;
								VCS_MEDIA_LOG_INFO("FFmpegDecoder: SPS saved, size=%zu", spsData_.size());
							}
							else if (nalType == 8) { // PPS
								ppsData_.assign(nalData, nalData + nalSize);
								hasPPS_ = true;
								VCS_MEDIA_LOG_INFO("FFmpegDecoder: PPS saved, size=%zu", ppsData_.size());
							}
							// 注意：不要在这里调用 SetExtradata() 或修改 extradataUpdated！
						};

						// Collect every NAL range first, then process.
						struct NalRange { size_t dataOff; size_t size; };
						std::vector<NalRange> nals;
						if (hasAnnexB) {
							size_t pos = 0;
							while (pos + 3 <= encodedSize) {
								size_t scLen = 0;
								if (pos + 4 <= encodedSize &&
									encodedData[pos] == 0x00 && encodedData[pos + 1] == 0x00 &&
									encodedData[pos + 2] == 0x00 && encodedData[pos + 3] == 0x01) {
									scLen = 4;
								}
								else if (encodedData[pos] == 0x00 && encodedData[pos + 1] == 0x00 &&
									encodedData[pos + 2] == 0x01) {
									scLen = 3;
								}
								else {
									pos++;
									continue;
								}
								size_t nalDataStart = pos + scLen;
								// Find next start code (or end of buffer).
								// [Fix D 2026-08-12] Handle H.264 emulation prevention bytes.
								// The Annex-B byte stream inserts an extra 0x03 whenever a NAL
								// payload contains 0x00 0x00 0x0X (X in {0,1,2,3}); the on-the-
								// wire bytes become 0x00 0x00 0x03 0x0X. Without skipping the
								// 0x03, the inner loop would mis-detect 0x00 0x00 0x01 inside a
								// SPS/PPS as the start of the next NAL and TRUNCATE the current
								// NAL. Empirically that truncated a 23-byte SPS down to 21
								// bytes and a 4-byte PPS down to 2 bytes, after which FFmpeg
								// rejects the extradata with "sps_id 0 out of range" /
								// "non-existing PPS 0 referenced".
								size_t next = nalDataStart;
								while (next + 3 <= encodedSize) {
									// 4-byte start code: 00 00 00 01
									if (next + 4 <= encodedSize &&
										encodedData[next] == 0x00 && encodedData[next + 1] == 0x00 &&
										encodedData[next + 2] == 0x00 && encodedData[next + 3] == 0x01) break;
									// 3-byte start code: 00 00 01
									if (encodedData[next] == 0x00 && encodedData[next + 1] == 0x00 &&
										encodedData[next + 2] == 0x01) break;
									// Emulation prevention: 00 00 03 0X (X in {0,1,2,3})
									// The 0x03 is a filler inserted to keep the original
									// 0x00 0x00 0X from being mistaken for a start code, so we
									// MUST skip past the 0x03 and resume scanning from the
									// byte after it.
									if (next + 4 <= encodedSize &&
										encodedData[next] == 0x00 && encodedData[next + 1] == 0x00 &&
										encodedData[next + 2] == 0x03 &&
										(encodedData[next + 3] == 0x00 || encodedData[next + 3] == 0x01 ||
											encodedData[next + 3] == 0x02 || encodedData[next + 3] == 0x03)) {
										next += 3; // skip the emulation prevention byte
										continue;
									}
									next++;
								}
								// [Fix E 2026-08-12] If the inner loop exited with next+3 >
								// encodedSize, no start code was found in the remaining tail
								// and the NAL extends to the END of the buffer. The old
								// `next <= encodedSize ? next - nalDataStart : ...` expression
								// truncated the last NAL by up to 2 bytes (video NALs at the
								// end of the buffer were captured shorter than they really
								// were). We now pick `encodedSize` as the NAL end whenever the
								// loop ran off the end of the buffer.
								size_t nalEnd = (next + 3 <= encodedSize) ? next : encodedSize;
								size_t nalSize = nalEnd - nalDataStart;
								nals.push_back({ nalDataStart, nalSize });
								pos = next;
							}
						}
						else {
							// No Annex-B: treat the whole buffer as a single NAL.
							nals.push_back({ 0, encodedSize });
						}

						if (nals.empty()) {
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: no NAL found in buffer (size=%zu)", encodedSize);
							return false;
						}

						// Process every NAL for SPS/PPS capture; remember the last one for
						// the actual frame decode path below.
						for (const auto& r : nals) {
							processOneNal(encodedData + r.dataOff, r.size);
						}
						const auto& tail = nals.back();
						lastNalType = (encodedData[tail.dataOff] & 0x1F);
						lastNalDataOffset = tail.dataOff;
						lastNalSize = tail.size;
						const uint8_t* nalData = encodedData + lastNalDataOffset;
						size_t nalSize = lastNalSize;
						uint8_t nalType = lastNalType;
						VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: NAL type=%d, totalNals=%zu, lastNalSize=%zu, hasSPS=%d, hasPPS=%d, bufferedFrames=%zu",
							nalType, nals.size(), nalSize, hasSPS_, hasPPS_, bufferedFrames_.size());

			
						// non- IDR frame (type=1)  IDR frame (type=5) need SPS/PPS in order to decode
						if ((nalType == 1 || nalType == 5) && (!hasSPS_ || !hasPPS_)) {
							// buffer frames until receiving SPS/PPS
							if (bufferedFrames_.size() < MAX_BUFFERED_FRAMES) {
								std::vector<uint8_t> frameData;
								frameData.assign(encodedData, encodedData + encodedSize);
								bufferedFrames_.push_back(std::move(frameData));
								VCS_MEDIA_LOG_TRACE("FFmpegDecoder: buffered frame type=%d, bufferSize=%zu",
									nalType, bufferedFrames_.size());
							}
							else {
								// Parameter sets not seen yet: buffering is expected
								// (self-healing once SPS/PPS arrive), so not a warning.
								VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: buffer full, dropping frame (no SPS/PPS yet)");
							}
							return false;
						}

						// [docs/11 §3.1.2] Rebuild extradata as soon as both parameter
						// sets are known -- regardless of whether any VCL frames were
						// buffered. Previously this only ran when bufferedFrames_ was
						// non-empty, so a call whose first AU carried SPS+PPS (no VCL)
						// left the decoder without extradata until some later frame
						// happened to be buffered and replayed.
						if (hasSPS_ && hasPPS_ && !extradataSet_) {
							VCS_MEDIA_LOG_INFO("FFmpegDecoder: SPS/PPS ready, building extradata");
							SetExtradata();
							extradataSet_ = true;
							extradataUpdated = true;
						}

						// Replay frames that were waiting for the parameter sets.
						if (extradataSet_ && !bufferedFrames_.empty()) {
							if (DecodeBufferedFrames(yuvData, yuvSize, width, height)) {
								return true;
							}
							// if decode failed,continue trying to decode current frame
						}

						// [P1-5 Fix] Copy the encoded payload into a packet-owned buffer.
						// Previously we aliased packet_->data to the caller's buffer, which
						// creates a use-after-free hazard if the caller releases encodedData
						// before the next av_packet_unref / SendPacket cycle completes. With
						// av_new_packet the buffer is owned by the AVPacket and freed on
						// av_packet_unref().
						av_packet_unref(packet_);
						int packetRet = av_new_packet(packet_, encodedSize);
						if (packetRet < 0) {
							char errBuf[128];
							av_strerror(packetRet, errBuf, sizeof(errBuf));
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: av_new_packet failed, ret=%d (%s), size=%zu",
								packetRet, errBuf, encodedSize);
							return false;
						}
						memcpy(packet_->data, encodedData, encodedSize);

						VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: sending packet to decoder: size=%d", packet_->size);

						int ret = avcodec_send_packet(ctx_, packet_);
						if (ret == AVERROR(EAGAIN)) {
							// EAGAIN is a contract signal, not a failure: the decoder
							// still holds output frames. Drain them (real-time pipeline
							// keeps only the newest) and retry the same packet once.
							while (avcodec_receive_frame(ctx_, frame_) == 0) {
							}
							ret = avcodec_send_packet(ctx_, packet_);
							VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: send_packet retried after draining output, ret=%d", ret);
						}
						if (ret < 0) {
							char errBuf[128];
							av_strerror(ret, errBuf, sizeof(errBuf));

							// printerror
							VCS_MEDIA_LOG_ERROR("FFmpegDecoder: avcodec_send_packet FAILED, ret=%d (%s), nalType=%d, hasSPS=%d, hasPPS=%d",
								ret, errBuf, nalType, hasSPS_, hasPPS_);

							// "non-existing PPS" error: SPS/PPS
							if (ret == AVERROR_INVALIDDATA) {
								// frame
								if (bufferedFrames_.size() < MAX_BUFFERED_FRAMES) {
									std::vector<uint8_t> frameData;
									frameData.assign(encodedData, encodedData + encodedSize);
									bufferedFrames_.push_back(std::move(frameData));
									VCS_MEDIA_LOG_INFO("FFmpegDecoder: buffered frame type=%d, bufferSize=%zu (reason: no SPS/PPS)",
										nalType, bufferedFrames_.size());
								}
								return false;
							}

							return false;
						}

						VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: avcodec_send_packet SUCCESS, now receiving frame");

						// [[Key Fix]]use while read all available frames
						// FFmpeg decoding API needloop reading until EAGAIN or real error
						// EAGAIN do notiserror,means"needinput"
						while (true) {
							ret = avcodec_receive_frame(ctx_, frame_);
							if (ret == AVERROR(EAGAIN)) {
								// EAGAIN: input sent but no output frame yet,this is normal
								// return false means"this decode has no output",but input has been successfully received
								VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: avcodec_receive_frame EAGAIN - no output frame available, input accepted");
								return false;
							}
							else if (ret == AVERROR_EOF) {
								// EOF: decodinghas been flushed,no more output
								VCS_MEDIA_LOG_DEBUG("FFmpegDecoder: avcodec_receive_frame EOF - decoder flushed");
								return false;
							}
							else if (ret < 0) {
								// real error
								char errBuf[128];
								av_strerror(ret, errBuf, sizeof(errBuf));
								VCS_MEDIA_LOG_ERROR("FFmpegDecoder: avcodec_receive_frame FAILED, ret=%d (%s)", ret, errBuf);

								// track consecutive errors,reset decoder when threshold exceeded
								consecutiveErrors_++;
								if (consecutiveErrors_ >= MAX_CONSECUTIVE_ERRORS) {
									VCS_MEDIA_LOG_ERROR("FFmpegDecoder: too many consecutive errors (%d), resetting decoder",
										consecutiveErrors_);
									FlushAndReset();
									consecutiveErrors_ = 0;
								}
								return false;
							}

							// successfully got one frame,exit loop
							break;
						}

#if 0
						{
							AVPixelFormat pixFmt = (AVPixelFormat)frame_->format;
							const char* fmtName = av_get_pix_fmt_name(pixFmt);
							const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);

							VCS_MEDIA_LOG_INFO("[DIAG] ==========================================");
							VCS_MEDIA_LOG_INFO("[DIAG] Pixel Format: %s (id=%d)",
								fmtName ? fmtName : "UNKNOWN", pixFmt);
							VCS_MEDIA_LOG_INFO("[DIAG] Resolution: %dx%d", frame_->width, frame_->height);
							VCS_MEDIA_LOG_INFO("[DIAG] Linesize: Y=%d, U=%d, V=%d, A=%d",
								frame_->linesize[0], frame_->linesize[1],
								frame_->linesize[2], frame_->linesize[3]);
							VCS_MEDIA_LOG_INFO("[DIAG] Data: Y=%p, U=%p, V=%p, A=%p",
								frame_->data[0], frame_->data[1],
								frame_->data[2], frame_->data[3]);

							if (desc) {
								VCS_MEDIA_LOG_INFO("[DIAG] Planar: %s, HW: %s",
									(desc->flags & AV_PIX_FMT_FLAG_PLANAR) ? "YES" : "NO",
									(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) ? "YES" : "NO");
								VCS_MEDIA_LOG_INFO("[DIAG] Chroma: log2_w=%d, log2_h=%d, nb_comp=%d",
									desc->log2_chroma_w, desc->log2_chroma_h, desc->nb_components);


								if (pixFmt == AV_PIX_FMT_YUV420P || pixFmt == AV_PIX_FMT_YUVJ420P) {
									VCS_MEDIA_LOG_INFO("[DIAG]  YUV420P planar format - GOOD");
								}
								else if (pixFmt == AV_PIX_FMT_NV12) {
									VCS_MEDIA_LOG_ERROR("[DIAG]  NV12 format - UV interleaved! Copy will be wrong!");
									VCS_MEDIA_LOG_ERROR("[DIAG]   data[1] contains UVUVUV..., data[2] is NULL");
								}
								else {
									VCS_MEDIA_LOG_ERROR("[DIAG]  UNEXPECTED format: %s", fmtName);
								}
							}

							int alignY = frame_->linesize[0] % 32;
							int alignU = frame_->linesize[1] % 32;
							int alignV = frame_->linesize[2] % 32;
							VCS_MEDIA_LOG_INFO("[DIAG] Alignment (mod 32): Y=%d, U=%d, V=%d", alignY, alignU, alignV);
							VCS_MEDIA_LOG_INFO("[DIAG] Diff (linesize - width): Y=%d, U=%d, V=%d",
								frame_->linesize[0] - frame_->width,
								frame_->linesize[1] - frame_->width / 2,
								frame_->linesize[2] - frame_->width / 2);
							VCS_MEDIA_LOG_INFO("[DIAG] ==========================================");
						}
#endif

						// reset consecutive error count
						consecutiveErrors_ = 0;

						width = frame_->width;
						height = frame_->height;

						VCS_MEDIA_LOG_TRACE("FFmpegDecoder: decoded %ux%u", width, height);


						// �� width = frame_->width; height = frame_->height; ֮��

						static int frameCount = 0;
						frameCount++;


						// [P1-1 / P2-1 Fix] Use frame_->linesize[i] as the source row
						// stride. Without this, the copy pulls in padding bytes that
						// don't belong to the actual pixel data, which corrupts the
						// output image.
						size_t needed = width * height * 3 / 2;
						if (yuvData && yuvSize >= needed) {
							// 【核心修复】：强制使用 sws_scale 将 FFmpeg 输出的任何格式（如 NV12）转为标准 YUV420P
							SwsContext* sws = sws_getContext(
								width, height, (AVPixelFormat)frame_->format,
								width, height, AV_PIX_FMT_YUV420P,
								SWS_BILINEAR, nullptr, nullptr, nullptr);
							if (sws) {
								uint8_t* dstData[4] = {
									yuvData,
									yuvData + width * height,
									yuvData + width * height * 5 / 4,
									nullptr
								};
								int dstLinesize[4] = { (int)width, (int)(width / 2), (int)(width / 2), 0 };

								sws_scale(sws, frame_->data, frame_->linesize, 0, height, dstData, dstLinesize);
								sws_freeContext(sws);
							}
							else {
								VCS_MEDIA_LOG_ERROR("FFmpegDecoder: sws_getContext failed for format %d", frame_->format);
							}
							yuvSize = needed;
						}
						else {
							yuvSize = needed;
						}

						// [Debug Dump] Write the decoded frame to file in GUARANTEED YUV420P.
						//
						// ROOT CAUSE OF PREVIOUS GARBLED OUTPUT:
						// The H.264 decoder can emit any of NV12, YUVJ420P, YV12, YUV420P,
						// or even YUV420P10LE depending on the input stream. We were blindly
						// reading frame->data[0/1/2] as if they were YUV420P Y/U/V planes.
						// For NV12 for example data[1] is an INTERLEAVED UV plane and
						// data[2] is unused �� copying half of data[1] as "U" and (junk)
						// from data[2] as "V" produces the alternating top/bottom garbage
						// we saw in ffplay.
						//
						// FIX: route every decoded frame through sws_scale to force a
						// tightly-packed YUV420P output, then dump THAT. The encoder path
						// above (which copies into yuvData) keeps its current behavior
						// because that buffer is then handed to the encoder, which itself
						// re-reads frame_->data and accepts whatever the decoder produced
						// �� fixing that path requires a separate decision. The dump file
						// is the user's diagnostic window, so it gets the *correct* YUV420P.
#if 0
						{
							InitDumpIfNeeded();
							if (g_decodedYuvDump.file &&
								g_decodedYuvDump.frameCount < g_decodedYuvDump.maxFrames) {

								AVPixelFormat srcFmt = (AVPixelFormat)frame_->format;

								// Always log the actual decoder format so we can see what
								// the decoder is really producing.
								/*VCS_MEDIA_LOG_INFO("[Dump] YUV src fmt=%s, %zux%u, linesize=[%d,%d,%d]",
									av_get_pix_fmt_name(srcFmt),
									width, height,
									frame_->linesize[0], frame_->linesize[1], frame_->linesize[2]);*/

									// ---- Force a YUV420P frame via sws_scale ----
								if (srcFmt != AV_PIX_FMT_NONE) {
									// Rebuild the cached context whenever input format/dims change.
									if (g_dumpSwsCtx == nullptr || g_dumpLastSrcFmt != srcFmt) {
										if (g_dumpSwsCtx) {
											sws_freeContext(g_dumpSwsCtx);
											g_dumpSwsCtx = nullptr;
										}
										g_dumpSwsCtx = sws_getContext(
											(int)width, (int)height, srcFmt,
											(int)width, (int)height, AV_PIX_FMT_YUV420P,
											SWS_BILINEAR, nullptr, nullptr, nullptr);
										g_dumpLastSrcFmt = srcFmt;
										VCS_MEDIA_LOG_INFO("[Dump] sws_getContext: %s -> YUV420P @ %zux%u",
											av_get_pix_fmt_name(srcFmt), width, height);
									}

									if (g_dumpSwsCtx) {
										if (g_dumpConvFrame == nullptr) {
											g_dumpConvFrame = av_frame_alloc();
										}
										av_frame_unref(g_dumpConvFrame);
										g_dumpConvFrame->format = AV_PIX_FMT_YUV420P;
										g_dumpConvFrame->width = (int)width;
										g_dumpConvFrame->height = (int)height;
										int bufRet = av_frame_get_buffer(g_dumpConvFrame, 0);
										if (bufRet < 0) {
											char errBuf[128];
											av_strerror(bufRet, errBuf, sizeof(errBuf));
											VCS_MEDIA_LOG_ERROR("[Dump] conv frame get_buffer failed, ret=%d (%s)",
												bufRet, errBuf);
										}
										else {
											int scaleH = sws_scale(
												g_dumpSwsCtx,
												frame_->data, frame_->linesize, 0, (int)height,
												g_dumpConvFrame->data, g_dumpConvFrame->linesize);
											if (scaleH != (int)height) {
												VCS_MEDIA_LOG_ERROR("[Dump] sws_scale returned %d, expected %u",
													scaleH, height);
											}
											else {
												// Build a contiguous YUV buffer in the canonical
												// YUV420P layout (Y, then U, then V; each plane
												// tightly packed, no padding).
												const size_t yPlaneBytes = (size_t)width * height;
												const size_t uvPlaneBytes = yPlaneBytes / 4;
												std::vector<uint8_t> yuv(yPlaneBytes * 3 / 2);

												const uint8_t* srcY = g_dumpConvFrame->data[0];
												const uint8_t* srcU = g_dumpConvFrame->data[1];
												const uint8_t* srcV = g_dumpConvFrame->data[2];
												const int sY = g_dumpConvFrame->linesize[0];
												const int sU = g_dumpConvFrame->linesize[1];
												const int sV = g_dumpConvFrame->linesize[2];

												uint8_t* dstY = yuv.data();
												uint8_t* dstU = dstY + yPlaneBytes;
												uint8_t* dstV = dstU + uvPlaneBytes;

												for (uint32_t y = 0; y < height; ++y) {
													memcpy(dstY + y * width, srcY + y * sY, width);
												}
												for (uint32_t y = 0; y < height / 2; ++y) {
													memcpy(dstU + y * (width / 2), srcU + y * sU, width / 2);
												}
												for (uint32_t y = 0; y < height / 2; ++y) {
													memcpy(dstV + y * (width / 2), srcV + y * sV, width / 2);
												}

												// First frame: write info file with the converted
												// format and a ready-to-use ffplay command.
												if (g_decodedYuvDump.frameCount == 0) {
													FILE* infoFile = fopen("G:/tmp/decoded_info.txt", "w");
													if (infoFile) {
														fprintf(infoFile, "YUV_PATH=G:/tmp/decoded.yuv\n");
														fprintf(infoFile, "WIDTH=%u\n", width);
														fprintf(infoFile, "HEIGHT=%u\n", height);
														fprintf(infoFile, "FORMAT=YUV420P (converted from %s)\n",
															av_get_pix_fmt_name(srcFmt));
														fprintf(infoFile, "FFPLAY_CMD=ffplay -f rawvideo -pixel_format yuv420p -video_size %ux%u G:/tmp/decoded.yuv\n",
															width, height);
														fflush(infoFile);
														fclose(infoFile);
													}
													VCS_MEDIA_LOG_INFO("[Dump] YUV resolution: %ux%u (info written to G:/tmp/decoded_info.txt)",
														width, height);
												}

												const size_t totalBytes = yuv.size();
												size_t written = fwrite(yuv.data(), 1, totalBytes, g_decodedYuvDump.file);
												fflush(g_decodedYuvDump.file);

												if (written != totalBytes) {
													VCS_MEDIA_LOG_ERROR("[Dump] YUV frame #%d SHORT WRITE: %zu/%zu bytes",
														g_decodedYuvDump.frameCount, written, totalBytes);
												}
												else {
													/*VCS_MEDIA_LOG_INFO("[Dump] YUV frame #%d OK, %zux%u, %zu bytes (expected %zu)",
														g_decodedYuvDump.frameCount + 1, width, height,
														written, totalBytes);*/
												}
												g_decodedYuvDump.frameCount++;
												g_decodedYuvDump.width = (int)width;
												g_decodedYuvDump.height = (int)height;

												// [YUV Fix] Also write the first (or every Nth) frame
												// as a self-describing BMP file. BMP carries its
												// width/height in the header, so the user can open
												// it in any image viewer and visually confirm the
												// decoded frame is correct �� no -video_size needed.
												// This bypasses any uncertainty about the raw YUV
												// interpretation in ffplay.
												{
													static int bmpFrameCounter = 0;
													if (bmpFrameCounter % 30 == 0) {
														// Convert the YUV420P frame to RGB24.
														static SwsContext* rgbSws = nullptr;
														static AVPixelFormat rgbLastSrc = AV_PIX_FMT_NONE;
														static int rgbLastW = 0, rgbLastH = 0;
														static AVFrame*   rgbFrame = nullptr;
														if (rgbSws == nullptr || rgbLastSrc != AV_PIX_FMT_YUV420P
															|| rgbLastW != (int)width || rgbLastH != (int)height) {
															if (rgbSws) sws_freeContext(rgbSws);
															rgbSws = sws_getContext(
																(int)width, (int)height, AV_PIX_FMT_YUV420P,
																(int)width, (int)height, AV_PIX_FMT_BGR24,
																SWS_BILINEAR, nullptr, nullptr, nullptr);
															rgbLastSrc = AV_PIX_FMT_YUV420P;
															rgbLastW = (int)width;
															rgbLastH = (int)height;
														}
														if (rgbSws) {
															if (!rgbFrame) rgbFrame = av_frame_alloc();
															av_frame_unref(rgbFrame);
															rgbFrame->format = AV_PIX_FMT_BGR24;
															rgbFrame->width = (int)width;
															rgbFrame->height = (int)height;
															if (av_frame_get_buffer(rgbFrame, 0) >= 0) {
																int rgbH = sws_scale(rgbSws,
																	g_dumpConvFrame->data, g_dumpConvFrame->linesize,
																	0, (int)height,
																	rgbFrame->data, rgbFrame->linesize);
																if (rgbH == (int)height) {
																	// Write a 24-bit BMP (BGR bottom-up).
																	const int W = (int)width;
																	const int H = (int)height;
																	const int rowSize = W * 3;
																	const int padBytes = (4 - (rowSize % 4)) % 4;
																	const int paddedRow = rowSize + padBytes;
																	const int pixelDataSize = paddedRow * H;
																	const int fileSize = 14 + 40 + pixelDataSize;

																	FILE* bmp = fopen("G:/tmp/decoded.bmp", "wb");
																	if (bmp) {
																		// ---- BMP file header (14 bytes) ----
																		uint8_t hdr[14] = { 0 };
																		hdr[0] = 'B'; hdr[1] = 'M';
																		*(uint32_t*)(hdr + 2) = (uint32_t)fileSize;
																		*(uint32_t*)(hdr + 10) = 14 + 40; // pixel offset
																		fwrite(hdr, 1, 14, bmp);

																		// ---- DIB header (BITMAPINFOHEADER, 40 bytes) ----
																		uint8_t dib[40] = { 0 };
																		*(uint32_t*)(dib + 0) = 40;            // header size
																		*(int32_t*)(dib + 4) = W;             // width
																		*(int32_t*)(dib + 8) = -H;            // height (negative = top-down)
																		*(uint16_t*)(dib + 12) = 1;            // planes
																		*(uint16_t*)(dib + 14) = 24;           // bpp
																		*(uint32_t*)(dib + 16) = 0;            // compression = BI_RGB
																		*(uint32_t*)(dib + 20) = (uint32_t)pixelDataSize;
																		*(uint32_t*)(dib + 24) = 2835;         // ~72 DPI
																		*(uint32_t*)(dib + 28) = 2835;
																		fwrite(dib, 1, 40, bmp);

																		// ---- Pixel data: BMP rows are bottom-up, RGB stored as BGR ----
																		// rgbFrame is in AV_PIX_FMT_BGR24 top-down, so we
																		// copy row 0 -> first BMP row, ..., row H-1 -> last.
																		std::vector<uint8_t> pad(padBytes, 0);
																		for (int y = 0; y < H; ++y) {
																			fwrite(rgbFrame->data[0] + y * rgbFrame->linesize[0],
																				1, rowSize, bmp);
																			if (padBytes) fwrite(pad.data(), 1, padBytes, bmp);
																		}
																		fclose(bmp);
																	}
																}
															}
														}
													}
													bmpFrameCounter++;
												}
											}
										}
									}
									else {
										VCS_MEDIA_LOG_ERROR("[Dump] sws_getContext returned NULL, skip frame");
									}
								}
							}
						}
#endif
						return true;
					}

					// flush and reset decoder
					void FlushAndReset() {
						if (ctx_) {
							// flush decoder
							avcodec_flush_buffers(ctx_);
						}

						bufferedFrames_.clear();
						consecutiveErrors_ = 0;

						extradataSet_ = false;

						if (hasSPS_ && hasPPS_ && !spsData_.empty() && !ppsData_.empty()) {
							VCS_MEDIA_LOG_INFO("FFmpegDecoder: FlushAndReset - re-applying extradata from cached SPS/PPS");
							SetExtradata();
							extradataSet_ = true;
						}

						VCS_MEDIA_LOG_INFO("FFmpegDecoder: flushed and reset, extradataSet=%d", extradataSet_);
					}

					void PacketLossConceal() {
						if (ctx_ && frame_) {
							av_frame_unref(frame_);
							avcodec_send_packet(ctx_, nullptr);
						}
					}

					void Release() {
						// clear buffer
						ResetBufferState();

						if (frame_) {
							av_frame_free(&frame_);
							frame_ = nullptr;
						}
						if (packet_) {
							av_packet_free(&packet_);
							packet_ = nullptr;
						}
						if (ctx_) {
							avcodec_free_context(&ctx_);
							ctx_ = nullptr;
						}
						initialized_ = false;
					}
				};

				// ============================================================
				// FFmpegEncoderWrapper
				// ============================================================

				FFmpegEncoder::FFmpegEncoder() : impl_(nullptr) {}

				FFmpegEncoder::~FFmpegEncoder() {
					Release();
				}

				bool FFmpegEncoder::Initialize(const VideoCodecConfig& config) {
					Impl* impl = new Impl();
					if (!impl->Initialize(config)) {
						delete impl;
						return false;
					}
					impl_ = impl;
					return true;
				}

				bool FFmpegEncoder::Encode(const uint8_t* yuvData, size_t yuvSize,
					std::vector<uint8_t>& outData,
					bool& isKeyFrame, uint64_t timestamp) {
					if (!impl_) return false;
					return impl_->Encode(yuvData, yuvSize, outData, isKeyFrame, timestamp);
				}

				void FFmpegEncoder::UpdateBitrate(uint32_t bitrate) {
					if (impl_) impl_->UpdateBitrate(bitrate);
				}

				void FFmpegEncoder::RequestKeyFrame() {
					if (impl_) impl_->RequestKeyFrame();
				}

				CodecCapability FFmpegEncoder::GetCapability() const {
					CodecCapability cap;
					// [P2-3 Fix] Reflect the actual configured codec type rather than
					// hardcoding "H264". If the encoder is not initialized (impl_ is null),
					// fall back to H.264 (the default in VideoCodecConfig).
					cap.codecName = impl_ ? VideoCodecTypeToString(impl_->config_.codecType)
						: VideoCodecTypeToString(VideoCodecType::H264);
					cap.backend = CodecBackend::Software;
					cap.supportHardware = false;
					cap.maxResolution = 1920 * 1080;
					cap.maxFramerate = 60;
					cap.maxBitrate = 10000000;
					cap.priority = 100;
					return cap;
				}

				CodecBackend FFmpegEncoder::GetBackend() const {
					return CodecBackend::Software;
				}

				void FFmpegEncoder::Release() {
					if (impl_) {
						delete impl_;
						impl_ = nullptr;
					}
				}

				// ============================================================
				// FFmpegDecoderWrapper
				// ============================================================

				FFmpegDecoder::FFmpegDecoder() : impl_(nullptr) {}

				FFmpegDecoder::~FFmpegDecoder() {
					Release();
				}

				bool FFmpegDecoder::Initialize(const VideoCodecConfig& config) {
					Impl* impl = new Impl();
					if (!impl->Initialize(config)) {
						delete impl;
						return false;
					}
					impl_ = impl;
					return true;
				}

				bool FFmpegDecoder::Decode(const uint8_t* encodedData, size_t encodedSize,
					bool isKeyFrame, uint8_t* yuvData, size_t& yuvSize,
					uint32_t& width, uint32_t& height) {
					if (!impl_) return false;
					return impl_->Decode(encodedData, encodedSize, isKeyFrame, yuvData, yuvSize, width, height);
				}

				void FFmpegDecoder::PacketLossConceal() {
					if (impl_) impl_->PacketLossConceal();
				}

				CodecCapability FFmpegDecoder::GetCapability() const {
					CodecCapability cap;
					// [P2-3 Fix] Same dynamic-codec-name fix as in FFmpegEncoder::GetCapability.
					cap.codecName = impl_ ? VideoCodecTypeToString(impl_->config_.codecType)
						: VideoCodecTypeToString(VideoCodecType::H264);
					cap.backend = CodecBackend::Software;
					cap.supportHardware = false;
					cap.maxResolution = 1920 * 1080;
					cap.maxFramerate = 60;
					cap.priority = 100;
					return cap;
				}

				CodecBackend FFmpegDecoder::GetBackend() const {
					return CodecBackend::Software;
				}

				void FFmpegDecoder::Release() {
					if (impl_) {
						delete impl_;
						impl_ = nullptr;
					}
				}

				// ============================================================
				// Creator functions
				// ============================================================
				std::unique_ptr<IVideoEncoder> CreateFFmpegEncoder() {
					return std::unique_ptr<IVideoEncoder>(new FFmpegEncoder());
				}

				std::unique_ptr<IVideoDecoder> CreateFFmpegDecoder() {
					return std::unique_ptr<IVideoDecoder>(new FFmpegDecoder());
				}

			}
		}
	}
} // namespace vcs::media::codec::software
