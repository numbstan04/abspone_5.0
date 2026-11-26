/**
 * @file VideoProcessor.cpp
 * @brief Video processor implementation (terminal version)
 *
 * 接收：VideoFrameQueue::Pop → decoder->Decode → sink->Render
 * 发送：source->Subscribe 回调入 encodeQueue → encodeThread → encoder->Encode
 *        → RTPEngine::SendVideoRtp
 *
 * 线程：
 *   - decodeThread（全局一个）：遍历所有流的 queue，Pop + Decode + Render
 *   - encodeThread（每流一个）：消费该流的 encodeQueue，Encode + SendVideoRtp
 */

#include "VideoProcessor.h"
#include "RTPEngine.h"
#include "IVideoSource.h"
#include "IVideoSink.h"
#include "PlatformTime.h"
#include "H264SdpFmtp.h"
#include "ILogger.h"
#include "SharedMutex.h"
#include "SpscQueue.h"  


#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>



namespace vcs {
	namespace media {

		namespace {

			constexpr size_t kDecodeScratchCapacity = 1920 * 1080 * 3 / 2;  // YUV420 max
			constexpr int    kEncodePollIntervalMs = 2;
			constexpr uint64_t kFirThrottleMs = 1000;

			const char* KindName(VideoStreamKind kind) {
				return kind == VideoStreamKind::Main ? "main" : "secondary";
			}

			// 扫描 H.264 Annex-B AU，判断是否携带 SPS(7)/PPS(8)。
			// 参数集 AU 的 isKeyFrame 为 false（解包器只在 NAL type 5 置位），
			// 因此同步门必须单独识别它们，否则会被当作普通非关键帧丢弃。
			bool CollectH264ParameterSets(const uint8_t* data, size_t size,
				std::string* outTypes) {
				if (!data || size < 3) return false;
				bool sawSps = false;
				bool sawPps = false;
				size_t pos = 0;
				while (pos + 3 <= size) {
					size_t scLen = 0;
					if (pos + 4 <= size &&
						data[pos] == 0x00 && data[pos + 1] == 0x00 &&
						data[pos + 2] == 0x00 && data[pos + 3] == 0x01) {
						scLen = 4;
					}
					else if (data[pos] == 0x00 && data[pos + 1] == 0x00 &&
						data[pos + 2] == 0x01) {
						scLen = 3;
					}
					if (scLen == 0) {
						pos++;
						continue;
					}
					size_t nalStart = pos + scLen;
					if (nalStart >= size) break;
					uint8_t nalType = data[nalStart] & 0x1F;
					if (nalType == 7) sawSps = true;
					else if (nalType == 8) sawPps = true;
					pos = nalStart;
				}
				if (!sawSps && !sawPps) return false;
				if (outTypes) {
					if (sawSps) *outTypes += "7";
					if (sawPps) {
						if (!outTypes->empty()) *outTypes += ",";
						*outTypes += "8";
					}
				}
				return true;
			}

		} // anonymous namespace

		// ============================================================
		// VideoProcessor::Impl
		// ============================================================
		struct VideoProcessor::Impl {
			struct StreamContext {
				ResolvedVideoStream resolved;
				std::string streamId;
				std::string consumerId;

				// ---- 接收 ----
				std::shared_ptr<codec::IVideoDecoder> decoder;
				VideoFrameQueue* queue = nullptr;
				std::vector<uint8_t> decodeScratchBuf;
				// Counters read by the 1 Hz media-info tick from another thread,
				// so they are atomic (the tick must never see a torn value).
				std::atomic<uint64_t> framesDecoded{ 0 };
				std::atomic<uint64_t> framesDropped{ 0 };
				uint64_t preIdrParamSets = 0;   // 同步门前转发给解码器的参数集 AU 数
				std::atomic<bool> firstIdrDecoded{ false };
				std::atomic<uint16_t> lastDecodedWidth{ 0 };
				std::atomic<uint16_t> lastDecodedHeight{ 0 };
				bool preIdrParamSetLogged = false;
				bool idrDecodeFailLogged = false;   // 未同步期间 IDR 解码失败只提示一次
				bool remoteSizeLogged = false;   // 首帧解码后记录远端真实分辨率（仅解码线程）

				// ---- SDP sprop（Phase B2）----
				// currentSprop 只由 SetCodec 线程读写，用于去重；
				// spropAu 用 atomic_load/store 跨线程交换，避免引入新锁。
				std::string currentSprop;
				std::shared_ptr<const std::vector<uint8_t>> spropAu;
				std::atomic<bool> spropFed{ true };  // 无可投喂内容时为 true

				// ---- 发送 ----
				std::shared_ptr<codec::IVideoEncoder> encoder;
				SpscQueue<std::shared_ptr<const VideoFrame>> encodeQueue;
				std::thread encodeThread;
				std::atomic<bool> encodeStopRequested{ false };
				uint16_t videoSeq = 0;
				uint64_t lastFirTimeMs = 0;
				// SSRC carried by the last FIR (0 = bootstrap before the first
				// inbound packet). Used so a bootstrap request does not swallow
				// the first request that carries the peer's real SSRC.
				uint32_t lastFirSsrc = 0;
				std::atomic<uint64_t> framesEncoded{ 0 };
				std::atomic<uint64_t> firRequests{ 0 };

				// ---- SetCodec 缓存 ----
				std::string currentCodecName;
				uint8_t currentPayloadType = 0;
				codec::VideoCodecType currentCodecType = codec::VideoCodecType::H264;
				// 编码尺寸在 SetCodec（会议执行线程）里更新，同时被
				// GetStreamReports（MediaWorker tick 线程）读取，所以必须是原子，
				// 否则媒体信息每秒采样一次就是一次数据竞争。
				std::atomic<uint16_t> currentWidth{ 0 };
				std::atomic<uint16_t> currentHeight{ 0 };
				uint32_t currentBitrate = 0;

				StreamContext() : encodeQueue(64) {}
			};

			VideoProcessorConfig config;
			std::shared_ptr<codec::ICodecFactory> codecFactory;
			std::weak_ptr<RTPEngine> rtpEngine;
			std::shared_ptr<VideoFrameQueuePool> queuePool;


			std::string sessionId_;
			bool testMode = false;

			std::unordered_map<VideoStreamKind, std::shared_ptr<StreamContext>> streams;
			mutable vcs::base::SharedMutex streamsMtx;

			std::thread decodeThread;
			std::atomic<bool> decodeRunning{ false };
			std::atomic<bool> decodeStopRequested{ false };
			int intervalMs = 33;

			// ---- helpers ----
			bool CreateDecoder(StreamContext& ctx);
			bool CreateEncoder(StreamContext& ctx);
			bool ResizeEncoderToFrame(StreamContext& ctx, uint16_t width, uint16_t height);
			void DestroyStream(StreamContext& ctx);

			void DecodeLoop();
			void EncodeLoop(std::shared_ptr<StreamContext> ctx);
			void DecodeOneFrame(StreamContext& ctx);
			void RequestFir(StreamContext& ctx, uint32_t remoteSsrcOverride = 0);

			bool RecreateEncoderDecoder(StreamContext& ctx,
				const std::string& codecName,
				codec::VideoCodecType codecType,
				uint16_t width, uint16_t height,
				uint32_t bitrate);

			void PushSendPayloadType(uint8_t payloadType);
			void ApplySdpFmtp(StreamContext& ctx, const std::string& fmtp);
		};

		// ============================================================
		// CreateDecoder / CreateEncoder / DestroyStream
		// ============================================================
		bool VideoProcessor::Impl::CreateDecoder(StreamContext& ctx) {
			if (!codecFactory) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] CreateDecoder: no codecFactory");
				return false;
			}

			const auto& c = ctx.resolved.config;
			auto dec = codecFactory->CreateVideoDecoder(c.codecType, c.backend);
			if (!dec) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] CreateVideoDecoder returned null "
					"(kind=%s, codec=%d)",
					KindName(c.kind), (int)c.codecType);
				return false;
			}

			codec::VideoCodecConfig dc;
			dc.codecType = c.codecType;
			dc.backend = c.backend;
			dc.width = c.width;
			dc.height = c.height;
			if (!dec->Initialize(dc)) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] decoder->Initialize failed "
					"(kind=%s, codec=%d)",
					KindName(c.kind), (int)c.codecType);
				return false;
			}


			ctx.decoder = std::move(dec);
			ctx.decodeScratchBuf.reserve(kDecodeScratchCapacity);
			ctx.currentCodecType = c.codecType;
			ctx.currentWidth = static_cast<uint16_t>(c.width);
			ctx.currentHeight = static_cast<uint16_t>(c.height);
			ctx.currentBitrate = c.bitrate;
			return true;
		}

		bool VideoProcessor::Impl::CreateEncoder(StreamContext& ctx) {
			if (!codecFactory) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] CreateEncoder: no codecFactory");
				return false;
			}

			const auto& c = ctx.resolved.config;
			auto enc = codecFactory->CreateVideoEncoder(c.codecType, c.backend);
			if (!enc) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] CreateVideoEncoder returned null "
					"(kind=%s, codec=%d)",
					KindName(c.kind), (int)c.codecType);
				return false;
			}

			codec::VideoCodecConfig ec;
			ec.codecType = c.codecType;
			ec.backend = c.backend;
			ec.width = c.width;
			ec.height = c.height;
			ec.framerate = c.framerate;
			ec.bitrate = c.bitrate;
			ec.keyFrameInterval = c.keyFrameInterval;
			if (!enc->Initialize(ec)) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] encoder->Initialize failed "
					"(kind=%s, codec=%d)",
					KindName(c.kind), (int)c.codecType);
				return false;
			}

			ctx.encoder = std::move(enc);
			return true;
		}

		// 编码尺寸以采集帧为准：VideoSinkConfig.width/height 是显示目标，
		// 不代表采集分辨率（Windows 采集固定 640x360）。首帧尺寸与配置不一致
		// 时按帧尺寸重建编码器，避免首帧触发编码器内部的尺寸不匹配重建。
		bool VideoProcessor::Impl::ResizeEncoderToFrame(
			StreamContext& ctx, uint16_t width, uint16_t height) {
			if (!codecFactory || width == 0 || height == 0) return false;

			ctx.encoder.reset();
			ctx.resolved.config.width = width;
			ctx.resolved.config.height = height;
			ctx.currentWidth = width;
			ctx.currentHeight = height;

			if (!CreateEncoder(ctx)) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] ResizeEncoderToFrame: encoder failed "
					"(kind=%s, %ux%u)", KindName(ctx.resolved.config.kind), width, height);
				return false;
			}

			VCS_MEDIA_LOG_INFO("[VideoProcessor] encoder resized to capture size %ux%u "
				"(kind=%s)", width, height, KindName(ctx.resolved.config.kind));
			return true;
		}

		void VideoProcessor::Impl::DestroyStream(StreamContext& ctx) {
			ctx.encoder.reset();
			ctx.decoder.reset();
			ctx.queue = nullptr;
			ctx.decodeScratchBuf.clear();
			ctx.decodeScratchBuf.shrink_to_fit();
		}

		// ============================================================
		// RecreateEncoderDecoder
		// ============================================================
		bool VideoProcessor::Impl::RecreateEncoderDecoder(
			StreamContext& ctx,
			const std::string& codecName,
			codec::VideoCodecType codecType,
			uint16_t width, uint16_t height,
			uint32_t bitrate) {

			if (!codecFactory) return false;

			const uint16_t w = (width == 0) ? ctx.currentWidth.load() : width;
			const uint16_t h = (height == 0) ? ctx.currentHeight.load() : height;
			const uint32_t br = (bitrate == 0) ? ctx.currentBitrate : bitrate;

			ctx.encoder.reset();
			ctx.decoder.reset();

			ctx.resolved.config.codecType = codecType;
			ctx.resolved.config.width = w;
			ctx.resolved.config.height = h;
			ctx.resolved.config.bitrate = br;

			if (ctx.resolved.sink && !CreateDecoder(ctx)) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] RecreateEncoderDecoder: decoder failed");
				return false;
			}
			if (ctx.resolved.source && !CreateEncoder(ctx)) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] RecreateEncoderDecoder: encoder failed");
				return false;
			}

			ctx.currentCodecName = codecName;
			ctx.currentCodecType = codecType;
			ctx.currentWidth = w;
			ctx.currentHeight = h;
			ctx.currentBitrate = br;
			return true;
		}

		// 发包 PT 每包由 RTPEngine 从 member context 读取，这里只登记。
		void VideoProcessor::Impl::PushSendPayloadType(uint8_t payloadType) {
			auto rtp = rtpEngine.lock();
			if (!rtp) {
				VCS_MEDIA_LOG_WARNING("[VideoProcessor] PushSendPayloadType: rtpEngine "
					"gone, video PT %u not applied", (unsigned)payloadType);
				return;
			}
			rtp->UpdateMemberVideoSendPayloadType(sessionId_, payloadType);
		}

		// fmtp 只消费 packetization-mode / profile-level-id /
		// sprop-parameter-sets；sprop 转 Annex-B 后交给解码线程投喂一次。
		void VideoProcessor::Impl::ApplySdpFmtp(StreamContext& ctx,
			const std::string& fmtp) {
			if (ctx.currentCodecType != codec::VideoCodecType::H264 || fmtp.empty()) {
				return;
			}

			const std::string pmode = h264sdp::FmtpParam(fmtp, "packetization-mode");
			const std::string plid = h264sdp::FmtpParam(fmtp, "profile-level-id");
			const std::string sprop = h264sdp::FmtpParam(fmtp, "sprop-parameter-sets");
			VCS_MEDIA_LOG_INFO("[VideoProcessor] fmtp: kind=%s, "
				"packetization-mode=%s, profile-level-id=%s, sprop=%s",
				KindName(ctx.resolved.config.kind),
				pmode.empty() ? "(absent)" : pmode.c_str(),
				plid.empty() ? "(absent)" : plid.c_str(),
				sprop.empty() ? "absent" : "present");

			if (pmode == "0") {
				VCS_MEDIA_LOG_WARNING("[VideoProcessor] fmtp: kind=%s "
					"packetization-mode=0 (single NAL) but our sender uses FU-A",
					KindName(ctx.resolved.config.kind));
			}

			if (sprop.empty() || sprop == ctx.currentSprop) return;
			std::vector<uint8_t> au;
			std::string nalTypes;
			if (!h264sdp::BuildParamSetAuFromSprop(sprop, au, &nalTypes)) {
				VCS_MEDIA_LOG_WARNING("[VideoProcessor] fmtp: kind=%s sprop parse "
					"failed (len=%zu), falling back to in-band parameter sets",
					KindName(ctx.resolved.config.kind), sprop.size());
				return;
			}
			const size_t auBytes = au.size();
			ctx.currentSprop = sprop;
			std::atomic_store(&ctx.spropAu,
				std::make_shared<const std::vector<uint8_t>>(std::move(au)));
			ctx.spropFed.store(false, std::memory_order_release);
			VCS_MEDIA_LOG_INFO("[VideoProcessor] fmtp: kind=%s sprop prepared "
				"nals=%s, bytes=%zu",
				KindName(ctx.resolved.config.kind), nalTypes.c_str(), auBytes);
		}

		// ============================================================
		// DecodeOneFrame / DecodeLoop
		// ============================================================
		void VideoProcessor::Impl::DecodeOneFrame(StreamContext& ctx) {
			if (!ctx.queue || !ctx.decoder || !ctx.resolved.sink) return;

			auto frame = ctx.queue->TryPop();
			if (!frame) return;

			// [docs/11 §3.5] SDP fmtp 里的 sprop 参数集在首个待解帧之前
			// 投喂一次。只在解码线程执行；SetCodec 用 atomic 交换
			// shared_ptr 传递，不新增锁。纯参数集 AU 无输出帧，
			// Decode 返回 false 属正常，不计丢帧。
			if (!ctx.spropFed.load(std::memory_order_acquire)) {
				auto spropAu = std::atomic_load(&ctx.spropAu);
				if (ctx.decoder && spropAu && !spropAu->empty()) {
					if (ctx.decodeScratchBuf.size() < kDecodeScratchCapacity) {
						ctx.decodeScratchBuf.resize(kDecodeScratchCapacity);
					}
					size_t yuvSize = ctx.decodeScratchBuf.size();
					uint32_t spW = 0, spH = 0;
					ctx.decoder->Decode(spropAu->data(), spropAu->size(), false,
						ctx.decodeScratchBuf.data(), yuvSize, spW, spH);
					VCS_MEDIA_LOG_INFO("[VideoProcessor] SDP sprop fed: session=%s, "
						"kind=%s, bytes=%zu",
						sessionId_.c_str(), KindName(ctx.resolved.config.kind),
						spropAu->size());
				}
				else if (!ctx.decoder) {
					VCS_MEDIA_LOG_WARNING("[VideoProcessor] SDP sprop dropped: decoder "
						"missing, session=%s, kind=%s",
						sessionId_.c_str(), KindName(ctx.resolved.config.kind));
				}
				ctx.spropFed.store(true, std::memory_order_release);
			}

			// [docs/11 §3.1.1] 关键帧同步门：未同步期间不上屏。
			// 非关键帧到达时以该包观测到的 ssrc 请求远端 IDR（T2 兜底，
			// 覆盖门开前丢包/中途加入/解码器冷启动；节流在 RequestFir 内）。
			//
			// 例外：携带 SPS/PPS 的参数集 AU 必须送进解码器取状态。它们的
			// isKeyFrame 为 false（解包器只在 NAL type 5 置位），旧逻辑会把
			// 它们和其它非关键帧一起丢弃，导致解码器永远拿不到参数集。
			// 这类 AU 不设同步标志、不上屏、不计入 framesDropped。
			if (!ctx.firstIdrDecoded.load()) {
				std::string paramNals;
				if (!frame->isKeyFrame &&
					frame->codecType == EncodedVideoFrame::CodecType::H264 &&
					CollectH264ParameterSets(frame->data, frame->size, &paramNals)) {
					if (ctx.decodeScratchBuf.size() < kDecodeScratchCapacity) {
						ctx.decodeScratchBuf.resize(kDecodeScratchCapacity);
					}
					size_t yuvSize = ctx.decodeScratchBuf.size();
					uint32_t w = 0, h = 0;
					ctx.decoder->Decode(
						frame->data, frame->size, false,
						ctx.decodeScratchBuf.data(), yuvSize, w, h);
					ctx.preIdrParamSets++;
					if (!ctx.preIdrParamSetLogged) {
						ctx.preIdrParamSetLogged = true;
						VCS_MEDIA_LOG_INFO("[VideoProcessor] pre-IDR parameter set forwarded: "
							"session=%s, kind=%s, nals=%s",
							sessionId_.c_str(), KindName(ctx.resolved.config.kind),
							paramNals.c_str());
					}
					return;
				}
				if (!frame->isKeyFrame) {
					if (frame->ssrc != 0) {
						RequestFir(ctx, frame->ssrc);
					}
					ctx.framesDropped++;
					return;
				}
				// 关键帧到达只代表"可以试解一次"，同步标志必须等解码真正
				// 成功后再置位：对端常只发 PPS+IDR、不带 SPS，此时解码必然
				// 失败，若在这里就认定已同步，就再也不会请求关键帧，画面
				// 永久停在黑屏（2026-11-01 入会无画面）。
				ctx.idrDecodeFailLogged = false;
				VCS_MEDIA_LOG_INFO("[VideoProcessor] keyframe received, attempting decode: session=%s, kind=%s",
					sessionId_.c_str(), KindName(ctx.resolved.config.kind));
			}

			if (ctx.decodeScratchBuf.size() < kDecodeScratchCapacity) {
				ctx.decodeScratchBuf.resize(kDecodeScratchCapacity);
			}

			if (frame->data && frame->size > 4) {
				VCS_MEDIA_LOG_TRACE(">>> DecodeFrame First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
					frame->data[0], frame->data[1], frame->data[2], frame->data[3],
					frame->data[4], frame->data[5], frame->data[6], frame->data[7]);
			}

			size_t yuvSize = ctx.decodeScratchBuf.size();
			uint32_t w = 0, h = 0;
			bool ok = ctx.decoder->Decode(
				frame->data, frame->size, frame->isKeyFrame,
				ctx.decodeScratchBuf.data(), yuvSize, w, h);

			if (!ok) {
				ctx.framesDropped++;
				if (frame->isKeyFrame) {
					// 关键帧解不出来（典型：AU 里只有 PPS+IDR，缺 SPS）。
					// 保持同步门关闭并重新请求 IDR，对端补发参数集后自愈。
					if (!ctx.idrDecodeFailLogged) {
						ctx.idrDecodeFailLogged = true;
						VCS_MEDIA_LOG_WARNING("[VideoProcessor] keyframe decode failed, "
							"still out of sync (parameter sets missing?), re-requesting keyframe: "
							"session=%s, kind=%s, ssrc=0x%08X",
							sessionId_.c_str(), KindName(ctx.resolved.config.kind), frame->ssrc);
					}
					if (frame->ssrc != 0) {
						RequestFir(ctx, frame->ssrc);
					}
				}
				VCS_MEDIA_LOG_DEBUG("[VideoProcessor] decode failed, dropping frame.");
				return; // 解码失败直接扔掉
			}
			ctx.framesDecoded.fetch_add(1);
			ctx.lastDecodedWidth.store(w);
			ctx.lastDecodedHeight.store(h);
			if (!ctx.firstIdrDecoded.load()) {
				ctx.firstIdrDecoded.store(true);
				VCS_MEDIA_LOG_INFO("[VideoProcessor] keyframe sync established: session=%s, kind=%s",
					sessionId_.c_str(), KindName(ctx.resolved.config.kind));
			}

			// 远端真实分辨率以解码输出为准（SDP 的 H.264 不带分辨率）。
			if (!ctx.remoteSizeLogged && w > 0 && h > 0) {
				ctx.remoteSizeLogged = true;
				VCS_MEDIA_LOG_INFO("[VideoProcessor] remote video %ux%u: session=%s, kind=%s",
					w, h, sessionId_.c_str(), KindName(ctx.resolved.config.kind));
			}

			auto out = std::make_shared<VideoFrame>();
			out->data = std::make_shared<std::vector<uint8_t>>(
				ctx.decodeScratchBuf.begin(),
				ctx.decodeScratchBuf.begin() + static_cast<std::ptrdiff_t>(yuvSize));
			out->dataSize = yuvSize;
			out->width = w;
			out->height = h;
			out->timestamp = frame->timestamp;
			out->pixelFormat = VideoFrame::PixelFormat::YUV420P;
			out->memberId = ctx.streamId;

			if (ctx.resolved.sink->Render(out)) {
				// 渲染成功
			}
		}

		void VideoProcessor::Impl::DecodeLoop() {
			VCS_MEDIA_LOG_INFO("[VideoProcessor] DecodeLoop started: session=%s, interval=%dms",
				sessionId_.c_str(), intervalMs);

			while (!decodeStopRequested.load(std::memory_order_acquire)) {
				// 快照当前流（shared_ptr 保证迭代期间不会被 RemoveStream 释放）
				std::vector<std::shared_ptr<StreamContext>> snapshot;
				{
					auto lock = streamsMtx.GetReadLock();
					snapshot.reserve(streams.size());
					for (auto& kv : streams) {
						if (kv.second) snapshot.push_back(kv.second);
					}
				}

				for (auto& ctx : snapshot) {
					if (!ctx) continue;
					// 一次迭代把该流队列里的帧全部消费
					while (ctx->queue && !decodeStopRequested.load()) {
						auto before = ctx->framesDecoded.load() + ctx->framesDropped.load() + ctx->preIdrParamSets;
						DecodeOneFrame(*ctx);
						auto after = ctx->framesDecoded.load() + ctx->framesDropped.load() + ctx->preIdrParamSets;
						if (after == before) break;  // 队列空
					}
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
			}

			VCS_MEDIA_LOG_INFO("[VideoProcessor] DecodeLoop stopped: session=%s",
				sessionId_.c_str());
		}

		// ============================================================
		// EncodeLoop
		// ============================================================
		void VideoProcessor::Impl::EncodeLoop(std::shared_ptr<StreamContext> ctx) {
			if (!ctx) return;
			VCS_MEDIA_LOG_INFO("[VideoProcessor] EncodeLoop started: session=%s, kind=%s",
				sessionId_.c_str(),
				KindName(ctx->resolved.config.kind));

			while (!ctx->encodeStopRequested.load(std::memory_order_acquire)) {
				std::shared_ptr<const VideoFrame> frame;
				if (!ctx->encodeQueue.TryPop(frame)) {
					std::this_thread::sleep_for(
						std::chrono::milliseconds(kEncodePollIntervalMs));
					continue;
				}
				if (!frame || !ctx->encoder) continue;

				const uint8_t* yuv = frame->GetData();
				if (!yuv || frame->dataSize == 0) continue;

#if 0
				if (ctx->resolved.sink) {
					auto displayFrame = std::make_shared<VideoFrame>();
					// 深拷贝一份数据，防止在渲染时被编码器释放
					displayFrame->data = std::make_shared<std::vector<uint8_t>>(yuv, yuv + frame->dataSize);
					displayFrame->dataSize = frame->dataSize;
					displayFrame->width = frame->width;
					displayFrame->height = frame->height;
					displayFrame->pixelFormat = VideoFrame::PixelFormat::YUV420P;

					// 注意：必须使用绑定的 streamId，否则 GdiVideoSink 会丢弃
					displayFrame->memberId = ctx->streamId;

					ctx->resolved.sink->Render(displayFrame);
				}
#endif
				// ====================================================

				// 编码分辨率跟随采集帧（缩放在本管线尚未实现）：
				// 配置里的 1280x720 只是显示目标，采集实际为 640x360。
				if (frame->width > 0 && frame->height > 0 &&
					(frame->width != ctx->currentWidth.load() ||
					 frame->height != ctx->currentHeight.load())) {
					if (!ResizeEncoderToFrame(*ctx,
						static_cast<uint16_t>(frame->width),
						static_cast<uint16_t>(frame->height))) {
						continue;
					}
				}

				std::vector<uint8_t> encoded;
				bool isKeyFrame = false;
				uint64_t ts = frame->timestamp;

				if (!ctx->encoder->Encode(yuv, frame->dataSize, encoded, isKeyFrame, ts)) {
					VCS_MEDIA_LOG_WARNING("[VideoProcessor] encode failed: kind=%s, size=%zu",
						KindName(ctx->resolved.config.kind),
						frame->dataSize);
					continue;
				}
				if (encoded.empty()) continue;

				VCS_MEDIA_LOG_TRACE("[VideoProcessor] Encoded 1 frame, isKeyFrame=%d, size=%zu", isKeyFrame, encoded.size());

				auto rtp = rtpEngine.lock();
				if (!rtp) continue;

				// RTP 时间戳：90kHz
				const uint64_t nowMs = vcs::base::platform::UtcNowEpochMs();
				const uint32_t rtpTs = static_cast<uint32_t>(nowMs * 90);
				const uint16_t seq = static_cast<uint16_t>(ctx->videoSeq + 1);
				uint16_t lastSeq = seq;

				try {
					rtp->SendVideoRtp(sessionId_, encoded.data(), encoded.size(),
						ctx->resolved.config.localSsrc,
						rtpTs, seq, isKeyFrame, &lastSeq,
						ctx->resolved.config.kind == VideoStreamKind::Main
							? vcs::MediaStreamKind::Video
							: vcs::MediaStreamKind::SecondaryVideo);
					ctx->videoSeq = lastSeq;
				}
				catch (const std::exception& e) {
					VCS_MEDIA_LOG_ERROR("[VideoProcessor] SendVideoRtp threw: %s", e.what());
				}
				ctx->framesEncoded.fetch_add(1);
			}

			VCS_MEDIA_LOG_INFO("[VideoProcessor] EncodeLoop stopped: session=%s, kind=%s",
				sessionId_.c_str(),
				KindName(ctx->resolved.config.kind));
		}

		// ============================================================
		// RequestFir
		// ============================================================
		void VideoProcessor::Impl::RequestFir(StreamContext& ctx, uint32_t remoteSsrcOverride) {
			// [docs/09 §2.4] SDP 未携带对端 ssrc 时，用接收首包观测到的 ssrc 兜底。
			// 两者都不知道时仍发一次 SSRC=0 的引导请求：对端（如 Yealink）
			// 收到后才开始推流，不发就永远等不到"首个包"来学 SSRC。
			uint32_t remoteSsrc = ctx.resolved.config.remoteSsrc;
			if (remoteSsrc == 0 && remoteSsrcOverride != 0) {
				remoteSsrc = remoteSsrcOverride;
			}

			const uint64_t nowMs = vcs::base::platform::UtcNowEpochMs();
			if (ctx.lastFirTimeMs != 0 && nowMs - ctx.lastFirTimeMs < kFirThrottleMs) {
				// 引导请求（SSRC=0）不能吞掉第一个带真实 SSRC 的请求。
				if (!(ctx.lastFirSsrc == 0 && remoteSsrc != 0)) {
					return;
				}
			}
			ctx.lastFirTimeMs = nowMs;
			ctx.lastFirSsrc = remoteSsrc;
			ctx.firRequests.fetch_add(1);

			auto rtp = rtpEngine.lock();
			if (!rtp) return;

			const uint32_t localSsrc = ctx.resolved.config.localSsrc;
			VCS_MEDIA_LOG_INFO("[VideoProcessor] FIR request: session=%s, kind=%s, "
				"remoteSsrc=0x%08X, localSsrc=0x%08X%s",
				sessionId_.c_str(),
				KindName(ctx.resolved.config.kind),
				remoteSsrc, localSsrc,
				remoteSsrc == 0 ? " (bootstrap, peer SSRC not learned yet)" : "");
			rtp->SendFir(sessionId_, remoteSsrc, localSsrc);
		}

		// ============================================================
		// VideoProcessor public API
		// ============================================================
		VideoProcessor::VideoProcessor() : impl_(std::make_unique<Impl>()) {
			VCS_MEDIA_LOG_INFO("[VideoProcessor] created");
		}

		VideoProcessor::~VideoProcessor() {
			Stop();
			VCS_MEDIA_LOG_INFO("[VideoProcessor] destroyed");
		}

		bool VideoProcessor::Initialize(
			const VideoProcessorConfig& config,
			std::shared_ptr<codec::ICodecFactory> codecFactory) {

			if (!codecFactory) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] Initialize: codecFactory is null");
				return false;
			}

			impl_->config = config;
			impl_->codecFactory = std::move(codecFactory);
			impl_->testMode = config.testMode;

			VCS_MEDIA_LOG_INFO("[VideoProcessor] Initialize: input=%s %ux%u, output=%s %ux%u, testMode=%d",
				codec::VideoCodecTypeToString(config.inputCodec),
				config.inputWidth, config.inputHeight,
				codec::VideoCodecTypeToString(config.outputCodec),
				config.outputWidth, config.outputHeight,
				(int)config.testMode);
			return true;
		}

		void VideoProcessor::SetRtpEngine(std::weak_ptr<RTPEngine> rtpEngine) {
			impl_->rtpEngine = std::move(rtpEngine);
		}

		void VideoProcessor::SetVideoFrameQueuePool(
			std::shared_ptr<VideoFrameQueuePool> queuePool) {
			auto lock = impl_->streamsMtx.GetWriteLock();
			impl_->queuePool = std::move(queuePool);
			VCS_MEDIA_LOG_INFO("[VideoProcessor] SetVideoFrameQueuePool: %s",
				impl_->queuePool ? "set" : "nullptr");
		}

		std::shared_ptr<VideoFrameQueuePool>
			VideoProcessor::GetVideoFrameQueuePool() const {
			auto lock = impl_->streamsMtx.GetReadLock();
			return impl_->queuePool;
		}

		bool VideoProcessor::Start(
			const std::string& sessionId,
			const std::vector<ResolvedVideoStream>& streams) {

			if (sessionId.empty()) {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] Start: empty sessionId");
				return false;
			}
			if (streams.empty()) {
				VCS_MEDIA_LOG_WARNING("[VideoProcessor] Start: no streams");
				return false;
			}

			auto lock = impl_->streamsMtx.GetWriteLock();

			if (impl_->decodeRunning.load()) {
				VCS_MEDIA_LOG_WARNING("[VideoProcessor] Start: already running (session=%s)",
					impl_->sessionId_.c_str());
				return false;
			}

			impl_->sessionId_ = sessionId;

			for (const auto& rs : streams) {
				auto ctx = std::make_shared<Impl::StreamContext>();
				ctx->resolved = rs;
				ctx->streamId = sessionId + "_" + KindName(rs.config.kind);
				ctx->consumerId = ctx->streamId;

				// 接收：decoder + queue + sink
				if (rs.sink) {
					if (!impl_->CreateDecoder(*ctx)) {
						VCS_MEDIA_LOG_ERROR("[VideoProcessor] Start: CreateDecoder failed "
							"(kind=%s)", KindName(rs.config.kind));
						return false;
					}
					if (!impl_->queuePool) {
						VCS_MEDIA_LOG_ERROR("[VideoProcessor] Start: queuePool not set");
						return false;
					}
					if (!impl_->queuePool->HasQueue(sessionId)) {
						impl_->queuePool->CreateQueue(sessionId);
					}
					ctx->queue = impl_->queuePool->GetQueue(sessionId);
					if (!ctx->queue) {
						VCS_MEDIA_LOG_ERROR("[VideoProcessor] Start: queue null for session=%s",
							sessionId.c_str());
						return false;
					}
					if (!rs.sink->BindStream(ctx->streamId)) {
						VCS_MEDIA_LOG_WARNING("[VideoProcessor] Start: sink %s already bound",
							rs.sink->GetId().c_str());
					}
				}

				// 发送：encoder + encodeThread + Subscribe
				if (rs.source) {
					if (!impl_->CreateEncoder(*ctx)) {
						VCS_MEDIA_LOG_ERROR("[VideoProcessor] Start: CreateEncoder failed "
							"(kind=%s)", KindName(rs.config.kind));
						return false;
					}

					ctx->encodeStopRequested = false;
					std::shared_ptr<Impl::StreamContext> ctxCopy = ctx;
					ctx->encodeThread = std::thread(
						[this, ctxCopy] { impl_->EncodeLoop(ctxCopy); });

					auto ctxRaw = ctx.get();
					rs.source->Subscribe(ctx->consumerId,
						[ctxRaw](std::shared_ptr<const VideoFrame> f) {
						if (ctxRaw && f) {
							ctxRaw->encodeQueue.TryPush(std::move(f));
						}
					});
				}

				impl_->streams[rs.config.kind] = std::move(ctx);
			}

			impl_->decodeStopRequested = false;
			impl_->decodeRunning = true;
			impl_->intervalMs = 33;
			impl_->decodeThread = std::thread([this] { impl_->DecodeLoop(); });

			for (auto& ctx : impl_->streams) {
				if (ctx.second && ctx.second->encoder) {
					ctx.second->encoder->RequestKeyFrame();
					VCS_MEDIA_LOG_INFO("[VideoProcessor] Forced KeyFrame request on start for kind=%d", (int)ctx.first);
				}
			}

			VCS_MEDIA_LOG_INFO("[VideoProcessor] Start: session=%s, streams=%zu",
				sessionId.c_str(), streams.size());
			return true;
		}

		void VideoProcessor::Stop() {
			// 1. 停 decodeThread
			impl_->decodeStopRequested = true;
			if (impl_->decodeThread.joinable()) {
				impl_->decodeThread.join();
			}
			impl_->decodeRunning = false;

			// 2. 移出所有流（持锁交换，释放锁后再清理）
			std::vector<std::shared_ptr<Impl::StreamContext>> snapshot;
			{
				auto lock = impl_->streamsMtx.GetWriteLock();
				snapshot.reserve(impl_->streams.size());
				for (auto& kv : impl_->streams) snapshot.push_back(kv.second);
				impl_->streams.clear();
			}

			// 3. 停止每流的 encodeThread，取消订阅，解绑 sink
			for (auto& ctx : snapshot) {
				if (!ctx) continue;

				ctx->encodeStopRequested = true;
				if (ctx->encodeThread.joinable()) {
					ctx->encodeThread.join();
				}

				if (ctx->resolved.source) {
					ctx->resolved.source->Unsubscribe(ctx->consumerId);
				}
				if (ctx->resolved.sink) {
					ctx->resolved.sink->UnbindStream();
				}
				impl_->DestroyStream(*ctx);
			}

			impl_->sessionId_.clear();
			VCS_MEDIA_LOG_INFO("[VideoProcessor] Stop: done");
		}

		bool VideoProcessor::AddStream(const ResolvedVideoStream& stream) {
			auto lock = impl_->streamsMtx.GetWriteLock();

			if (impl_->streams.count(stream.config.kind) > 0) {
				VCS_MEDIA_LOG_WARNING("[VideoProcessor] AddStream: kind=%s already exists",
					KindName(stream.config.kind));
				return false;
			}

			auto ctx = std::make_shared<Impl::StreamContext>();
			ctx->resolved = stream;
			ctx->streamId = impl_->sessionId_ + "_" + KindName(stream.config.kind);
			ctx->consumerId = ctx->streamId;

			if (stream.sink) {
				if (!impl_->CreateDecoder(*ctx)) return false;
				if (!impl_->queuePool) return false;
				if (!impl_->queuePool->HasQueue(impl_->sessionId_)) {
					impl_->queuePool->CreateQueue(impl_->sessionId_);
				}
				ctx->queue = impl_->queuePool->GetQueue(impl_->sessionId_);
				stream.sink->BindStream(ctx->streamId);
			}

			if (stream.source) {
				if (!impl_->CreateEncoder(*ctx)) return false;
				ctx->encodeStopRequested = false;
				std::shared_ptr<Impl::StreamContext> ctxCopy = ctx;
				ctx->encodeThread = std::thread(
					[this, ctxCopy] { impl_->EncodeLoop(ctxCopy); });

				auto ctxRaw = ctx.get();
				stream.source->Subscribe(ctx->consumerId,
					[ctxRaw](std::shared_ptr<const VideoFrame> f) {
					if (ctxRaw && f) ctxRaw->encodeQueue.TryPush(std::move(f));
				});
			}

			impl_->streams[stream.config.kind] = std::move(ctx);
			VCS_MEDIA_LOG_INFO("[VideoProcessor] AddStream: kind=%s",
				KindName(stream.config.kind));
			return true;
		}

		bool VideoProcessor::RemoveStream(VideoStreamKind kind) {
			std::shared_ptr<Impl::StreamContext> ctx;
			{
				auto lock = impl_->streamsMtx.GetWriteLock();
				auto it = impl_->streams.find(kind);
				if (it == impl_->streams.end()) {
					VCS_MEDIA_LOG_WARNING("[VideoProcessor] RemoveStream: kind=%s not found",
						KindName(kind));
					return false;
				}
				ctx = it->second;
				impl_->streams.erase(it);
			}

			if (!ctx) return true;

			ctx->encodeStopRequested = true;
			if (ctx->encodeThread.joinable()) ctx->encodeThread.join();

			if (ctx->resolved.source) ctx->resolved.source->Unsubscribe(ctx->consumerId);
			if (ctx->resolved.sink)   ctx->resolved.sink->UnbindStream();

			impl_->DestroyStream(*ctx);
			VCS_MEDIA_LOG_INFO("[VideoProcessor] RemoveStream: kind=%s", KindName(kind));
			return true;
		}

		void VideoProcessor::SetCodec(VideoStreamKind kind,
			const std::string& codecName,
			uint8_t payloadType,
			uint16_t width,
			uint16_t height,
			uint32_t bitrate,
			const std::string& fmtp) {
			std::shared_ptr<Impl::StreamContext> ctx;
			{
				auto lock = impl_->streamsMtx.GetReadLock();
				auto it = impl_->streams.find(kind);
				if (it == impl_->streams.end()) {
					VCS_MEDIA_LOG_WARNING("[VideoProcessor] SetCodec: kind=%s not found",
						KindName(kind));
					return;
				}
				ctx = it->second;
			}

			codec::VideoCodecType newType = ctx->currentCodecType;
			if (!codec::TryParseVideoCodecType(codecName, newType)) {
				VCS_MEDIA_LOG_WARNING("[VideoProcessor] SetCodec: cannot parse '%s'",
					codecName.c_str());
				return;
			}

			const uint16_t newW = (width == 0) ? ctx->currentWidth.load() : width;
			const uint16_t newH = (height == 0) ? ctx->currentHeight.load() : height;
			const uint32_t newBr = (bitrate == 0) ? ctx->currentBitrate : bitrate;

			const bool firstInit = ctx->currentCodecName.empty();
			const bool typeChanged = (newType != ctx->currentCodecType);
			const bool resChanged = (newW != ctx->currentWidth.load() ||
				newH != ctx->currentHeight.load());
			const bool brChanged = (newBr != ctx->currentBitrate);

			if (!firstInit && !typeChanged && !resChanged) {
				if (brChanged && ctx->encoder && newBr > 0) {
					ctx->encoder->UpdateBitrate(newBr);
					ctx->currentBitrate = newBr;
				}
				ctx->currentCodecName = codecName;
				ctx->currentPayloadType = payloadType;
				impl_->ApplySdpFmtp(*ctx, fmtp);
				impl_->PushSendPayloadType(payloadType);
				VCS_MEDIA_LOG_INFO("[VideoProcessor] SetCodec (cheap): kind=%s, name=%s, "
					"pt=%u, bitrate=%u",
					KindName(kind), codecName.c_str(), payloadType, newBr);
				return;
			}

			// 完整重建：先停该流 encodeThread，重建后重启
			ctx->encodeStopRequested = true;
			if (ctx->encodeThread.joinable()) ctx->encodeThread.join();
			ctx->encodeStopRequested = false;

			bool ok = impl_->RecreateEncoderDecoder(*ctx, codecName, newType, newW, newH, newBr);
			if (ok) {
				ctx->currentPayloadType = payloadType;
				// 解码器已重建，允许重新投喂 SDP 参数集。
				ctx->currentSprop.clear();
				ctx->spropFed.store(true, std::memory_order_release);
				impl_->ApplySdpFmtp(*ctx, fmtp);
				impl_->PushSendPayloadType(payloadType);
			}
			else {
				VCS_MEDIA_LOG_ERROR("[VideoProcessor] SetCodec full recreate FAILED: kind=%s",
					KindName(kind));
			}

			if (ctx->resolved.source) {
				std::shared_ptr<Impl::StreamContext> ctxCopy = ctx;
				ctx->encodeThread = std::thread(
					[this, ctxCopy] { impl_->EncodeLoop(ctxCopy); });
			}

			VCS_MEDIA_LOG_INFO("[VideoProcessor] SetCodec (recreate): kind=%s, name=%s, "
				"%ux%u, bitrate=%u, ok=%d",
				KindName(kind), codecName.c_str(), newW, newH, newBr, (int)ok);
		}

		void VideoProcessor::RequestKeyFrame(VideoStreamKind kind) {
			std::shared_ptr<Impl::StreamContext> ctx;
			{
				auto lock = impl_->streamsMtx.GetReadLock();
				auto it = impl_->streams.find(kind);
				if (it == impl_->streams.end()) return;
				ctx = it->second;
			}
			if (ctx && ctx->encoder) {
				ctx->encoder->RequestKeyFrame();
				VCS_MEDIA_LOG_INFO("[VideoProcessor] RequestKeyFrame: kind=%s", KindName(kind));
			}
		}

		// [docs/09 §2.4] 请求远端发关键帧（RTCP FIR，节流见 RequestFir）。
		void VideoProcessor::RequestRemoteKeyFrame(VideoStreamKind kind) {
			std::shared_ptr<Impl::StreamContext> ctx;
			{
				auto lock = impl_->streamsMtx.GetReadLock();
				auto it = impl_->streams.find(kind);
				if (it == impl_->streams.end()) return;
				ctx = it->second;
			}
			if (ctx) {
				impl_->RequestFir(*ctx);
			}
		}

		bool VideoProcessor::HasVideo(VideoStreamKind kind) const {
			auto lock = impl_->streamsMtx.GetReadLock();
			return impl_->streams.count(kind) > 0;
		}

		uint32_t VideoProcessor::GetVideoSsrc(VideoStreamKind kind) const {
			auto lock = impl_->streamsMtx.GetReadLock();
			auto it = impl_->streams.find(kind);
			return (it != impl_->streams.end()) ? it->second->resolved.config.localSsrc : 0;
		}

		std::vector<VideoStreamReport> VideoProcessor::GetStreamReports() const {
			std::vector<VideoStreamReport> reports;
			auto lock = impl_->streamsMtx.GetReadLock();
			reports.reserve(impl_->streams.size());
			for (const auto& kv : impl_->streams) {
				const auto& ctx = kv.second;
				if (!ctx) continue;

				VideoStreamReport report;
				report.kind = kv.first;
				report.encodedWidth = ctx->currentWidth.load();
				report.encodedHeight = ctx->currentHeight.load();
				report.decodedWidth = ctx->lastDecodedWidth.load();
				report.decodedHeight = ctx->lastDecodedHeight.load();
				report.framesEncoded = ctx->framesEncoded.load();
				report.framesDecoded = ctx->framesDecoded.load();
				report.framesDropped = ctx->framesDropped.load();
				report.firRequests = ctx->firRequests.load();
				report.firstKeyframeDecoded = ctx->firstIdrDecoded.load();
				reports.push_back(report);
			}
			return reports;
		}

		void VideoProcessor::SetTestMode(bool enable) {
			impl_->testMode = enable;
			VCS_MEDIA_LOG_INFO("[VideoProcessor] SetTestMode: %d", (int)enable);
		}

		bool VideoProcessor::IsTestMode() const {
			return impl_->testMode;
		}

	}
} // namespace vcs::media
