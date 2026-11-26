/**
 * @file MediaControl.cpp
 * @brief Media control implementation (terminal version)
 *
 * 媒体模块入口。保留 SetupMediaResources / TeardownMediaResources /
 * StartMedia / StopMedia / Observer / Config 等模块
 * 基础设施；删除会议/成员/混流/录制/OSD 等 MCU 功能。
 *
 * 设备管理在 MediaDeviceManager 内部完成，上层不注册设备。
 */

#include "MediaControl.h"
#include "EventBus.h"
#include "MediaDeviceManager.h"
#include "MediaWorker.h"
#include "MediaPacket.h"
#include "VideoFrameQueue.h"
#include "codec/CodecManager.h"
#include "ILogger.h"

#include <boost/endian/conversion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace vcs::base;

namespace vcs {
	namespace media {

		namespace {
			constexpr int kAudioFrameIntervalMs = 20;
			constexpr int kVideoFrameIntervalMs = 33;

			// audio.mic.volume is the legacy MicrophoneVolume (0..100) and the
			// only mic level knob the INI exposes. It drives the send-side
			// software trim: 0 = mute, 50 = unity, 100 = +12 dB.
			float MicVolumeToGain(int volume) {
				if (volume <= 0) return 0.0f;
				if (volume <= 50) return static_cast<float>(volume) / 50.0f;
				const float db = static_cast<float>(volume - 50) / 50.0f * 12.0f;
				return std::pow(10.0f, db / 20.0f);
			}

			// RFC 3550 Section 5.1 header layout: 12 fixed bytes, then CC
			// 32-bit CSRC entries, then an optional extension header.
			// Returns the payload range (padding already excluded), or false
			// when the datagram is too short for the header it advertises.
			bool RtpPayloadRange(const uint8_t* data, size_t len,
				size_t& payloadOffset, size_t& payloadLen) {
				if (!data || len < 12) return false;
				payloadOffset = 12 + static_cast<size_t>(data[0] & 0x0F) * 4;
				if (data[0] & 0x10) {  // X bit: 4-byte extension header, length in words
					if (len < payloadOffset + 4) return false;
					const uint16_t words =
						(static_cast<uint16_t>(data[payloadOffset + 2]) << 8) |
						static_cast<uint16_t>(data[payloadOffset + 3]);
					payloadOffset += 4 + static_cast<size_t>(words) * 4;
				}
				if (payloadOffset > len) return false;
				payloadLen = len - payloadOffset;
				if (data[0] & 0x20) {  // P bit: last octet holds the count
					if (payloadLen == 0) return false;
					const size_t pad = data[len - 1];
					if (pad == 0 || pad > payloadLen) return false;
					payloadLen -= pad;
				}
				return true;
			}

			// 媒体状态是"一个主题一个状态事件"：MediaStateChanged 是粘性事件，
			// sticky 表只按 type 索引，所以 payload 里只带上状态与所属会话。
			void PublishMediaState(const std::shared_ptr<vcs::IEventBus>& bus,
				const std::string& confId,
				const std::string& sessionId,
				const char* state) {
				if (!bus) return;
				vcs::Event ev;
				ev.type = vcs::EventType::MediaStateChanged;
				ev.severity = vcs::EventSeverity::Info;
				ev.source = vcs::EventSource::Media;
				ev.conferenceId = confId;
				ev.sessionId = sessionId;
				ev.payload = std::string("{\"state\":\"") + state + "\"}";
				bus->Publish(std::move(ev));
			}
		}

		// ============================================================
		// MediaTransportCallback
		// ============================================================
		MediaTransportCallback::MediaTransportCallback(RTPEngine* rtpEngine,
			MediaWorker* worker,
			vcs::MediaStreamKind kind,
			std::string memberId,
			std::string conferenceId)
			: rtpEngine_(rtpEngine),
			worker_(worker),
			kind_(kind),
			memberId_(std::move(memberId)),
			conferenceId_(std::move(conferenceId)) {
			VCS_MEDIA_LOG_DEBUG(
				"[MediaTransportCallback] created: memberId=%s, confId=%s, mode=%s",
				memberId_.c_str(), conferenceId_.c_str(),
				worker_ ? "async-worker" : "sync");

			// Resolve the counter handle once. Construction happens after
			// RTPEngine::AddMember, so the statistics entry exists; if it does
			// not (defensive), OnRtpReceived keeps the locked fallback.
			if (rtpEngine_) {
				if (MediaStatisticsCollector* collector =
						rtpEngine_->GetStatisticsCollector()) {
					statsCounter_ = collector->GetStreamCounter(memberId_, kind_);
				}
			}
		}

		MediaTransportCallback::~MediaTransportCallback() {
			VCS_MEDIA_LOG_DEBUG("[MediaTransportCallback] destroyed: memberId=%s",
				memberId_.c_str());
		}

		void MediaTransportCallback::OnRtpReceived(
			const vcs::RtpPacket& packet,
			const vcs::SessionContext& context) {

			if (packet.length < 12 || packet.data == nullptr) {
				VCS_MEDIA_LOG_WARNING(
					"[MediaTransportCallback] short/null packet: memberId=%s, len=%zu",
					memberId_.c_str(), packet.length);
				return;
			}

			// CSRC list and extension header move the payload start past
			// byte 12; reading data+12 would feed header bytes into the
			// decoder for any peer that populates them.
			size_t payloadOffset = 0;
			size_t payloadLen = 0;
			if (!RtpPayloadRange(packet.data, packet.length, payloadOffset, payloadLen)) {
				VCS_MEDIA_LOG_WARNING(
					"[MediaTransportCallback] malformed RTP header: memberId=%s, len=%zu, "
					"cc=%u, x=%u, p=%u",
					memberId_.c_str(), packet.length,
					static_cast<unsigned>(packet.data[0] & 0x0F),
					static_cast<unsigned>((packet.data[0] >> 4) & 0x01),
					static_cast<unsigned>((packet.data[0] >> 5) & 0x01));
				return;
			}
			if (payloadLen == 0) {
				// Padding-only / header-only datagram: nothing to decode.
				return;
			}

			// 收包计数只在这里做：这是唯一同时知道"哪一路流"和"线上字节数"
			// 的地方（MediaWorker 只拿 payload，RTPEngine 只能按 PT 猜，
			// 且它的 FU-A 分片在重组完成前就返回、根本记不到数）。
			// 句柄在构造时解析：热路径只做原子累加，不再每包取统计库互斥。
			if (statsCounter_) {
				statsCounter_->AddReceived(packet.length, 1, 0, 0);
			}
			else if (rtpEngine_) {
				if (MediaStatisticsCollector* collector =
						rtpEngine_->GetStatisticsCollector()) {
					collector->AddReceived(
						context.memberId.empty() ? memberId_ : context.memberId,
						kind_, packet.length, 1, 0, 0);
				}
			}

			if (worker_) {
				MediaPacket mp;
				mp.conferenceId = conferenceId_;
				mp.memberId = context.memberId.empty() ? memberId_ : context.memberId;
				mp.payloadType = static_cast<uint8_t>(packet.data[1] & 0x7F);
				mp.markerBit = (packet.data[1] & 0x80) != 0;
				mp.sequenceNumber = boost::endian::big_to_native(*reinterpret_cast<const uint16_t*>(packet.data + 2));
				mp.timestamp = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(packet.data + 4));
				mp.ssrc = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(packet.data + 8));
				mp.payload.assign(packet.data + payloadOffset,
					packet.data + payloadOffset + payloadLen);
				mp.receivedAt = std::chrono::steady_clock::now();

				if (!worker_->Enqueue(kind_, std::move(mp))) {
					VCS_MEDIA_LOG_WARNING(
						"[MediaTransportCallback] worker lane full, dropping: "
						"memberId=%s, kind=%s",
						memberId_.c_str(),
						vcs::MediaStreamKindToString(kind_));
				}
				return;
			}

			if (rtpEngine_) {
				const std::string& targetId =
					context.memberId.empty() ? memberId_ : context.memberId;
				rtpEngine_->OnRtpReceived(targetId, packet);
			}
			else {
				VCS_MEDIA_LOG_ERROR(
					"[MediaTransportCallback] rtpEngine null, packet dropped: memberId=%s",
					memberId_.c_str());
			}
		}

		void MediaTransportCallback::OnRtcpReceived(
			const vcs::RtcpPacket& packet,
			const vcs::SessionContext& context) {

			// This callback object is bound to one MediaStreamKind, so kind_ is
			// the stream the RTCP arrived on - the packet itself does not carry
			// that information. RTPEngine parses PLI/FIR and drives the encoder.
			if (!rtpEngine_ || !packet.data || packet.length == 0) {
				return;
			}
			rtpEngine_->HandleInboundRtcp(
				context.memberId.empty() ? memberId_ : context.memberId,
				kind_, packet.data, packet.length);
		}

		void MediaTransportCallback::OnTransportConnected(
			const vcs::SessionContext& context) {
			VCS_MEDIA_LOG_INFO("[MediaTransportCallback] transport connected: memberId=%s, confId=%s",
				context.memberId.c_str(), context.conferenceId.c_str());
		}

		void MediaTransportCallback::OnTransportDisconnected(
			const vcs::SessionContext& context, const std::string& reason) {
			VCS_MEDIA_LOG_INFO("[MediaTransportCallback] transport disconnected: memberId=%s, reason=%s",
				context.memberId.c_str(), reason.c_str());
		}

		void MediaTransportCallback::OnTransportError(
			const vcs::SessionContext& context, int errorCode,
			const std::string& errorMessage) {
			VCS_MEDIA_LOG_ERROR("[MediaTransportCallback] transport error: memberId=%s, code=%d, msg=%s",
				context.memberId.c_str(), errorCode, errorMessage.c_str());
		}

		// ============================================================
		// MediaControl::Impl
		// ============================================================
		struct MediaControl::Impl {
			// 会话媒体上下文（终端只有一个，map size 为 1）
			struct ConferenceContext {
				// 快照查询与 1 Hz tick 的 key：confId 是 map 键，sessionId
				// 是统计库里的成员键（StopMedia 之后成员即被摘除）。
				std::string confId;
				std::string sessionId;

				std::shared_ptr<AudioProcessor> audioProcessor;
				std::shared_ptr<VideoProcessor> videoProcessor;
				std::shared_ptr<RTPEngine>      rtpEngine;
				std::shared_ptr<MediaWorker>    worker;
				std::shared_ptr<VideoFrameQueuePool> videoQueuePool;
				vcs::media_config::VideoCodecConfig codecConfig;
				vcs::media_config::MediaResourceConfig resourceConfig;
			};

			std::unordered_map<std::string, std::shared_ptr<ConferenceContext>> tempMediaContexts_;
			std::unordered_map<std::string, MemberMediaTransports> memberTransports_;
			std::unordered_map<std::string,
				std::map<vcs::MediaStreamKind,
				std::unique_ptr<MediaTransportCallback>>> memberCallbacks_;

			IMediaControlObserver* observer_ = nullptr;

			mutable std::mutex resourcesMutex_;
			std::unordered_map<std::string, std::shared_ptr<ConferenceMediaResource>> resources_;

			// Protects tempMediaContexts_, memberTransports_ and
			// memberCallbacks_. Never held across a transport or processor
			// call: those block or call back into the media layer.
			mutable std::mutex sessionsMutex_;

			std::shared_ptr<vcs::IEventBus> eventBus_;
			std::shared_ptr<codec::ICodecFactory> codecFactory;
			std::shared_ptr<vcs::base::IConfig> config_;
			std::unique_ptr<MediaDeviceManager> deviceManager_;

			// Build the context completely, then publish it: readers must
			// never observe a half-filled context.
			void PublishContext(const std::string& confId,
				const std::shared_ptr<ConferenceContext>& ctx) {
				std::lock_guard<std::mutex> lock(sessionsMutex_);
				tempMediaContexts_[confId] = ctx;
			}

			std::shared_ptr<ConferenceContext> FindContext(const std::string& id) const {
				std::lock_guard<std::mutex> lock(sessionsMutex_);
				auto it = tempMediaContexts_.find(id);
				return it == tempMediaContexts_.end() ? nullptr : it->second;
			}

			// ============================================================
			// 1 Hz tick：媒体层唯一的时钟（不额外建定时线程）
			//
			// 跑在 MediaWorker 线程上，而同一线程还负责音视频包分发，所以这里
			// 只做"读数 + 差值 + 落库"：不回调上层、不写日志、不调外部模块。
			// handler 的异常隔离与追赶上限由 MediaWorker::RunLoop 兜底。
			// ============================================================
			void OnSecondTick(const std::string& confId) {
				auto ctx = FindContext(confId);
				if (!ctx || !ctx->rtpEngine) return;

				MediaStatisticsCollector* collector =
					ctx->rtpEngine->GetStatisticsCollector();
				if (!collector || !collector->HasMember(ctx->sessionId)) return;

				// 处理器计数是累计量，1 Hz 拉一次就够；逐帧推送只会把
				// 编解码热路径拖进统计库的锁里。
				if (ctx->audioProcessor) {
					const AudioStreamReport report =
						ctx->audioProcessor->GetReport();
					collector->UpdateAudioProcess(ctx->sessionId,
						report.framesEncoded, report.framesDecoded);
				}
				if (ctx->videoProcessor) {
					const std::vector<VideoStreamReport> reports =
						ctx->videoProcessor->GetStreamReports();
					for (std::size_t i = 0; i < reports.size(); ++i) {
						const VideoStreamReport& r = reports[i];
						const vcs::MediaStreamKind kind =
							(r.kind == VideoStreamKind::Main)
								? vcs::MediaStreamKind::Video
								: vcs::MediaStreamKind::SecondaryVideo;
						collector->UpdateVideoProcess(ctx->sessionId, kind,
							r.encodedWidth, r.encodedHeight,
							r.decodedWidth, r.decodedHeight,
							r.framesEncoded, r.framesDecoded,
							r.framesDropped, r.firRequests,
							r.firstKeyframeDecoded);
					}
				}

				// 累计量 → 每秒量（bitrate / 包速率 / fps / 丢包率）
				collector->Tick(ctx->sessionId);
			}

			// 记录每个 kind 的流形态：SSRC、对端地址、本端地址。
			// 地址只用于排障，取不到就留空（接口默认实现返回空）。
			static void RecordStreamShape(MediaStatisticsCollector* collector,
				const std::string& sessionId,
				const MemberMediaTransports& transports) {
				if (!collector) return;
				for (auto it = transports.begin(); it != transports.end(); ++it) {
					const std::shared_ptr<IRtpTransport>& transport = it->second;
					if (!transport) continue;

					std::string localAddress = transport->GetLocalAddress();
					const uint16_t localPort = transport->GetLocalPort();
					if (!localAddress.empty() && localPort != 0) {
						localAddress += ":";
						localAddress += std::to_string(localPort);
					}
					collector->SetStreamActive(sessionId, it->first, true,
						transport->GetSsrc(),
						transport->GetRemoteAddress(),
						localAddress);
				}
			}

			// 快照装配：live 会话优先，其次挂断前抓下的最终快照。
			// 锁内只读 key / 拷贝快照，统计库自己的锁与 infoMutex 不嵌套。
			static Response CollectSnapshot(
				const std::shared_ptr<ConferenceMediaResource>& resource,
				vcs::MediaInfoSnapshot& out) {

				std::string sessionId;
				{
					std::lock_guard<std::mutex> lock(resource->infoMutex);
					sessionId = resource->sessionId;
				}

				MediaStatisticsCollector* collector = resource->rtpEngine
					? resource->rtpEngine->GetStatisticsCollector() : nullptr;
				if (collector && !sessionId.empty() &&
					collector->HasMember(sessionId) &&
					collector->GetSnapshot(sessionId, out)) {
					out.confId = resource->confId;
					out.sessionId = sessionId;
					out.live = true;
					return Response::Success();
				}

				{
					std::lock_guard<std::mutex> lock(resource->infoMutex);
					if (!resource->finalInfo.streams.empty()) {
						out = resource->finalInfo;
						return Response::Success();
					}
				}

				return Response::Error(ResponseCode::NotFound,
					"no media session for: " + resource->confId);
			}

			// Replace a session's callback objects and hand back the raw
			// pointers for SetCallback. The objects stay owned by
			// memberCallbacks_ until the session is torn down, so callers
			// must not keep these pointers beyond that point.
			void RebuildCallbacks(const std::string& sessionId,
				RTPEngine* engine, MediaWorker* worker,
				const MemberMediaTransports& transports,
				std::vector<std::pair<vcs::MediaStreamKind, MediaTransportCallback*>>& out) {
				std::lock_guard<std::mutex> lock(sessionsMutex_);
				auto& cbMap = memberCallbacks_[sessionId];
				cbMap.clear();
				for (const auto& kv : transports) {
					auto& cb = cbMap[kv.first];
					cb.reset(new MediaTransportCallback(
						engine, worker, kv.first, sessionId, sessionId));
					out.push_back(std::make_pair(kv.first, cb.get()));
				}
			}

			// IRtpTransport 契约：回调对象必须活得比传输久，销毁前先
			// SetCallback(nullptr)。传输侧保证该调用返回后不再有在途派发
			// （gateway 的 StopReceiving 也已先行），因此返回即可安全销毁。
			// 必须在 sessionsMutex_ 之外调用。
			static void DetachCallbacks(const MemberMediaTransports& transports) {
				for (const auto& kv : transports) {
					if (kv.second) kv.second->SetCallback(nullptr);
				}
			}
		};

		// ============================================================
		// Construction / Destruction
		// ============================================================
		MediaControl::MediaControl(std::shared_ptr<IPlatformMediaFactory> platformFactory)
			: impl_(std::make_unique<Impl>()) {

			impl_->codecFactory = codec::CodecManager::Instance().CreateDefaultFactory();
			if (!impl_->codecFactory) {
				VCS_MEDIA_LOG_ERROR("[MediaControl] CreateDefaultFactory returned null");
			}

			if (platformFactory) {
				impl_->deviceManager_ = std::make_unique<MediaDeviceManager>(
					std::move(platformFactory));
			}
			else {
				VCS_MEDIA_LOG_WARNING("[MediaControl] platformFactory is null; "
					"device manager will be unavailable");
			}

			VCS_MEDIA_LOG_INFO("[MediaControl] created (codec=%s, deviceManager=%s)",
				impl_->codecFactory ? "yes" : "no",
				impl_->deviceManager_ ? "yes" : "no");
		}

		MediaControl::~MediaControl() {
			VCS_MEDIA_LOG_INFO("[MediaControl] destroying...");

			// Swap both tables out under their locks; external calls happen with no
			// lock held (worker Stop joins a thread, SetCallback enters a transport).
			std::unordered_map<std::string, std::shared_ptr<ConferenceMediaResource>> resources;
			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				resources.swap(impl_->resources_);
			}
			std::unordered_map<std::string, MemberMediaTransports> sessions;
			{
				std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
				sessions.swap(impl_->memberTransports_);
			}

			for (auto& kv : resources) {
				if (kv.second && kv.second->worker) {
					kv.second->worker->Stop();
				}
			}

			// 销毁回调对象前先摘挂：回调持有 rtpEngine/worker 裸指针，必须
			// 先于 resources.clear() 和 memberCallbacks_.clear()。
			for (const auto& kv : sessions) {
				Impl::DetachCallbacks(kv.second);
			}

			resources.clear();
			sessions.clear();

			impl_->tempMediaContexts_.clear();
			impl_->memberCallbacks_.clear();

			VCS_MEDIA_LOG_INFO("[MediaControl] destroyed");
		}

		// ============================================================
		// SetupMediaResources
		// ============================================================
		Response MediaControl::SetupMediaResources(
			const std::string& confId,
			const vcs::media_config::MediaResourceConfig& config) {

			if (confId.empty()) {
				VCS_MEDIA_LOG_WARNING("[MediaControl] SetupMediaResources: empty confId");
				return Response::Error(ResponseCode::InvalidParams, "empty confId");
			}

			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				if (impl_->resources_.count(confId) > 0) {
					VCS_MEDIA_LOG_WARNING("[MediaControl] SetupMediaResources: confId=%s already exists",
						confId.c_str());
					return Response::Error(ResponseCode::Conflict,
						"media resources already exist: " + confId);
				}
			}

			auto resource = std::make_shared<ConferenceMediaResource>();
			resource->confId = confId;
			resource->config = config;
			resource->createdAt = std::chrono::steady_clock::now();

			// RTPEngine
			resource->rtpEngine = std::make_shared<RTPEngine>();
			{
				MediaConfig rtpCfg;
				resource->rtpEngine->Initialize(rtpCfg, impl_->config_);
			}
			// 装配自证（docs/12 §8/§15 P0-4）：产物形态 + 实际后端。
			// 后端由 BackendFactory 按产物选择，这里只报告结果，不做分支。
#ifdef VCS_WITH_WEBRTC
			const char* const artifact = "standard";
#else
			const char* const artifact = "lite";
#endif
			VCS_MEDIA_LOG_INFO("[MediaControl] setup: confId=%s artifact=%s backend=%s",
				confId.c_str(), artifact,
				resource->rtpEngine->GetBackendType().c_str());

			// VideoProcessor
			resource->videoProc = std::make_shared<VideoProcessor>();
			{
				VideoProcessorConfig vcfg;
				vcfg.inputCodec = codec::VideoCodecType::H264;
				vcfg.inputBackend = codec::CodecBackend::Software;
				vcfg.outputCodec = codec::VideoCodecType::H264;
				vcfg.outputBackend = codec::CodecBackend::Software;
				vcfg.framerate = 30;
				vcfg.bitrate = 2000000;
				vcfg.keyFrameInterval = 60;
				vcfg.testMode = false;
				if (!resource->videoProc->Initialize(vcfg, impl_->codecFactory)) {
					VCS_MEDIA_LOG_ERROR("[MediaControl] videoProc->Initialize failed: confId=%s",
						confId.c_str());
					return Response::Error(ResponseCode::InternalError,
						"video processor initialize failed");
				}
			}

			// AudioProcessor
			resource->audioProc = std::make_shared<AudioProcessor>();
			{
				AudioProcessorConfig acfg;
				acfg.sampleRate = 8000;
				acfg.channels = 1;
				acfg.frameSizeMs = kAudioFrameIntervalMs;
				acfg.codecType = codec::AudioCodecType::G711U;
				acfg.codecBackend = codec::CodecBackend::Software;
				acfg.vadEnabled = true;
				if (impl_->config_) {
					int micVolume = 0;
					if (impl_->config_->GetMicrophoneVolume(micVolume).IsSuccess()) {
						acfg.micGain = MicVolumeToGain(micVolume);
						VCS_MEDIA_LOG_INFO(
							"[MediaControl] audio mic volume=%d, send gain=%.2fx",
							micVolume, acfg.micGain);
					}
				}
				if (!resource->audioProc->Initialize(acfg, impl_->codecFactory)) {
					VCS_MEDIA_LOG_ERROR("[MediaControl] audioProc->Initialize failed: confId=%s",
						confId.c_str());
					return Response::Error(ResponseCode::InternalError,
						"audio processor initialize failed");
				}
			}

			// MediaWorker（接收方向异步解耦 + 媒体层 1 Hz 时钟）
			//
			// tick 捕获 this：worker 在本对象析构/TeardownMediaResources 里
			// 先被 Stop() join，返回之后才轮到 impl_ 与各子系统，因此 tick
			// 执行期间 impl_ 必然存活。
			{
				MediaWorker::Config wcfg;
				if (config.workerVideoQueueDepth > 0) {
					wcfg.video.depth = config.workerVideoQueueDepth;
				}
				if (config.workerVideoBatchSize > 0) {
					wcfg.video.batch = config.workerVideoBatchSize;
				}
				const std::string tickConfId = confId;
				resource->worker = std::make_shared<MediaWorker>(
					confId, resource->rtpEngine.get(), wcfg,
					[this, tickConfId]() { impl_->OnSecondTick(tickConfId); });
				resource->worker->Start();
			}

			// VideoFrameQueuePool（接收视频帧 SPSC）
			resource->videoQueuePool = std::make_shared<VideoFrameQueuePool>(30);
			resource->rtpEngine->SetVideoFrameQueuePool(resource->videoQueuePool);
			resource->videoProc->SetVideoFrameQueuePool(resource->videoQueuePool);

			// 存入 registry
			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				impl_->resources_.emplace(confId, resource);
			}

			VCS_MEDIA_LOG_INFO("[MediaControl] SetupMediaResources: confId=%s, "
				"worker audio/video/aux lanes started (video=%u/%u)",
				confId.c_str(),
				config.workerVideoQueueDepth,
				config.workerVideoBatchSize);
			return Response::Success();
		}

		// ============================================================
		// TeardownMediaResources
		// ============================================================
		vcs::ErrorCode MediaControl::TeardownMediaResources(const std::string& confId) {
			std::shared_ptr<ConferenceMediaResource> resource;
			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				auto it = impl_->resources_.find(confId);
				if (it == impl_->resources_.end()) {
					return vcs::ErrorCode::ConferenceNotFound;
				}
				resource = it->second;
				impl_->resources_.erase(it);
			}

			// 1. 先摘挂：之后不会再有包进入 MediaTransportCallback（它持有
			//    rtpEngine/worker 裸指针，必须先于二者销毁）。SetCallback 在
			//    锁外调用，返回后传输侧保证无在途派发。
			MemberMediaTransports transports;
			{
				std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
				auto tpIt = impl_->memberTransports_.find(confId);
				if (tpIt != impl_->memberTransports_.end()) transports = tpIt->second;
				impl_->tempMediaContexts_.erase(confId);
				impl_->memberTransports_.erase(confId);
			}
			Impl::DetachCallbacks(transports);

			// 2. 停 worker，让它 drain SPSC（此时 rtpEngine 仍存活）
			if (resource->worker) {
				resource->worker->Stop();
			}
			resource->worker.reset();

			// 3. 再销毁 processor / RTPEngine
			resource->videoProc.reset();
			resource->audioProc.reset();
			resource->rtpEngine.reset();
			resource->videoQueuePool.reset();

			// 4. 回调对象现在可以安全销毁
			{
				std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
				impl_->memberCallbacks_.erase(confId);
			}

			VCS_MEDIA_LOG_INFO("[MediaControl] TeardownMediaResources: confId=%s done",
				confId.c_str());
			return vcs::ErrorCode::Success;
		}

		bool MediaControl::HasMediaResources(const std::string& confId) const {
			std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
			return impl_->resources_.count(confId) > 0;
		}

		// ============================================================
		// StartMedia
		// ============================================================
		Response MediaControl::StartMedia(const std::string& confId,
			const std::string& sessionId,
			const MemberMediaTransports& transports) {

			if (confId.empty() || sessionId.empty()) {
				return Response::Error(ResponseCode::InvalidParams,
					"empty confId or sessionId");
			}
			if (transports.empty()) {
				return Response::Error(ResponseCode::InvalidOperation, "empty transports");
			}
			for (const auto& kv : transports) {
				if (!kv.second) {
					return Response::Error(ResponseCode::InvalidOperation, "null transport");
				}
			}

			std::shared_ptr<ConferenceMediaResource> resource;
			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				auto it = impl_->resources_.find(confId);
				if (it == impl_->resources_.end()) {
					return Response::Error(ResponseCode::NotFound, "no media resource");
				}
				resource = it->second;
			}

			// [幂等守卫] 同一会话重复 StartMedia：transports 相同 → 幂等成功；
			// transports 变了（重复 CONFIRMED / UPDATE 重协商，gateway 已换新
			// 传输并停掉旧的）→ 原地热替换：member 与 processors 不动，仅换
			// 传输、重设 SSRC、重挂回调、刷新后端发送表。若不拦截，第二次
			// 调用会走到 audioProc->Start 失败路径：RemoveMember 摘掉首次调用
			// 建立的 member（通话期发送全弃），回调对象悬空。
			if (resource->rtpEngine && resource->rtpEngine->HasMember(sessionId)) {
				MemberMediaTransports stored;
				{
					std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
					auto it = impl_->memberTransports_.find(sessionId);
					if (it != impl_->memberTransports_.end()) stored = it->second;
				}
				bool sameTransports = (stored.size() == transports.size());
				if (sameTransports) {
					for (const auto& kv : transports) {
						auto it = stored.find(kv.first);
						if (it == stored.end() || it->second != kv.second) {
							sameTransports = false;
							break;
						}
					}
				}
				if (sameTransports) {
					VCS_MEDIA_LOG_INFO(
						"[MediaControl] StartMedia: member already active, idempotent no-op: "
						"confId=%s, sessionId=%s", confId.c_str(), sessionId.c_str());
					return Response::Success("media already started");
				}

				VCS_MEDIA_LOG_INFO(
					"[MediaControl] StartMedia: transports changed, hot-swap: "
					"confId=%s, sessionId=%s", confId.c_str(), sessionId.c_str());

				const uint32_t baseSsrc =
					static_cast<uint32_t>(std::hash<std::string>{}(sessionId));
				{
					std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
					impl_->memberTransports_[sessionId] = transports;
				}
				for (const auto& kv : transports) {
					if (kv.first == MediaStreamKind::Audio) {
						kv.second->SetSsrc(baseSsrc ^ 0x40000000u);
					}
					else if (kv.first == MediaStreamKind::Video) {
						kv.second->SetSsrc(baseSsrc ^ 0x80000000u);
					}
					else if (kv.first == MediaStreamKind::SecondaryVideo) {
						kv.second->SetSsrc(baseSsrc ^ 0xC0000000u);
					}
					else {
						kv.second->SetSsrc(baseSsrc);
					}
				}
				// 热替换会 RemoveMember → AddMember，统计 member（含协商结果）
				// 随之重建；先拷出已协商编码，换完再写回，否则媒体信息会在
				// 下一次协商前退回 "NA"。
				MediaStatisticsCollector* collector =
					resource->rtpEngine->GetStatisticsCollector();
				vcs::MediaInfoSnapshot previousInfo;
				if (collector) {
					collector->GetSnapshot(sessionId, previousInfo);
				}

				// 刷新 member 的传输集与 SSRC（NativeRtpBackend 按 member 发送）。
				// 音频本地 SSRC 必须一并传入，否则 WebRTC 引擎的
				// AudioSendStream 会因 ssrc==0 而不创建，热替换后本端不再发音频。
				resource->rtpEngine->RemoveMember(sessionId);
				resource->rtpEngine->AddMember(sessionId,
					baseSsrc ^ 0x80000000u, transports, baseSsrc ^ 0x40000000u);
				// 热替换同样要刷新媒体信息里的 SSRC / 地址，否则排障看到的
				// 还是被替换掉的那组传输。
				Impl::RecordStreamShape(collector, sessionId, transports);
				if (collector) {
					for (std::size_t i = 0; i < previousInfo.streams.size(); ++i) {
						const vcs::MediaStreamSnapshot& old = previousInfo.streams[i];
						if (!old.codec.codecName.empty()) {
							collector->SetNegotiatedCodec(
								sessionId, old.kind, old.codec);
						}
					}
				}
				{
					std::lock_guard<std::mutex> lock(resource->infoMutex);
					resource->sessionId = sessionId;
					resource->finalInfo = vcs::MediaInfoSnapshot();
				}
				// 重挂回调：旧回调对象随旧传输弃用（旧传输已被 gateway Disconnect）。
				// 显式摘挂一遍，不依赖调用方时序；返回后 RebuildCallbacks
				// 才能安全销毁旧对象。
				Impl::DetachCallbacks(stored);
				std::vector<std::pair<MediaStreamKind, MediaTransportCallback*>> callbacks;
				impl_->RebuildCallbacks(sessionId, resource->rtpEngine.get(),
					resource->worker.get(), transports, callbacks);
				for (const auto& cb : callbacks) {
					auto tpIt = transports.find(cb.first);
					if (tpIt == transports.end() || !tpIt->second->SetCallback(cb.second)) {
						VCS_MEDIA_LOG_ERROR("[MediaControl] hot-swap SetCallback failed: kind=%s",
							MediaStreamKindToString(cb.first));
						return Response::Error(ResponseCode::InternalError,
							"hot-swap SetCallback failed");
					}
				}
				resource->rtpEngine->BeginReceive();
				return Response::Success("media transports updated");
			}

			// 清理旧绑定（幂等）并保存 transports。旧传输如果还在，必须先摘挂
			// 才能销毁旧回调对象，否则旧传输线程可能打到已释放的回调上。
			MemberMediaTransports previous;
			{
				std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
				auto prevIt = impl_->memberTransports_.find(sessionId);
				if (prevIt != impl_->memberTransports_.end()) previous = prevIt->second;
				impl_->memberTransports_[sessionId] = transports;
			}
			Impl::DetachCallbacks(previous);
			{
				std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
				impl_->memberCallbacks_.erase(sessionId);
			}

			// [修复] audio / video / secondary video 分配不同 SSRC。
// 原来的实现用 std::hash(sessionId) 给所有 transport 赋同一个
// SSRC，终端发出去的 audio 包和 video 包 SSRC 完全相同。
// 远端 RTPEngine 按 SSRC 路由时（if ssrc == audioSsrc ... else
// if ssrc == videoSsrc ...），audio 分支永远先命中，video 包被
// 错判成 audio 送进 AudioProcessor，VideoProcessor 永远收不到
// 数据 —— 这就是服务端日志里 "Missing reference picture" 的根源。
			const uint32_t baseSsrc = static_cast<uint32_t>(std::hash<std::string>{}(sessionId));
			const uint32_t audioSsrc = baseSsrc ^ 0x40000000u;
			const uint32_t videoSsrc = baseSsrc ^ 0x80000000u;
			const uint32_t secondaryVideoSsrc = baseSsrc ^ 0xC0000000u;

			for (const auto& kv : transports) {
				if (kv.first == MediaStreamKind::Audio) {
					kv.second->SetSsrc(audioSsrc);
				}
				else if (kv.first == MediaStreamKind::Video) {
					kv.second->SetSsrc(videoSsrc);
				}
				else if (kv.first == MediaStreamKind::SecondaryVideo) {
					kv.second->SetSsrc(secondaryVideoSsrc);
				}
				else {
					kv.second->SetSsrc(baseSsrc);
				}
			}

			// RTPEngine 的 AddMember 只接一个 ssrc，用于 FIR 的 localSsrc，
			// 传 videoSsrc 最合适（FIR 是视频反馈）。
			resource->rtpEngine->AddMember(sessionId, videoSsrc, transports, audioSsrc);

			// 媒体信息：新通话开始，清掉上一通留下的最终快照，并登记每路流的
			// SSRC / 地址。此后 RTPEngine 的收发计数按 kind 直接进统计库。
			Impl::RecordStreamShape(
				resource->rtpEngine->GetStatisticsCollector(), sessionId, transports);
			{
				std::lock_guard<std::mutex> lock(resource->infoMutex);
				resource->sessionId = sessionId;
				resource->finalInfo = vcs::MediaInfoSnapshot();
			}

			// 启动失败必须全量回滚。上层按“媒体是否 live”判断是否已收敛，
			// 半成品（成员在、处理器在、回调缺）会让它把失败当成成功，之后
			// 永不重试。AddMember 之后的每一条失败路径都必须走这里。
			auto abortStart = [&]() {
				Impl::DetachCallbacks(transports);
				if (resource->videoProc) resource->videoProc->Stop();
				if (resource->audioProc) resource->audioProc->Stop();
				if (resource->rtpEngine) resource->rtpEngine->RemoveMember(sessionId);
				{
					std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
					impl_->memberCallbacks_.erase(sessionId);
					impl_->memberTransports_.erase(sessionId);
				}
				{
					std::lock_guard<std::mutex> lock(resource->infoMutex);
					if (resource->sessionId == sessionId) {
						resource->sessionId.clear();
					}
					resource->finalInfo = vcs::MediaInfoSnapshot();
				}
			};

			// ---- 构造视频流 ----
			auto mkVideoStream = [&](MediaStreamKind kind,
				VideoStreamKind vk,
				const std::string& defaultSourceId,
				const std::string& defaultSinkId,
				const vcs::media_config::VideoSinkConfig& sinkCfg)
				-> std::shared_ptr<VideoProcessor::ResolvedVideoStream> {

				auto tIt = transports.find(kind);
				if (tIt == transports.end()) return nullptr;

				VideoStreamConfig vc;
				vc.kind = vk;
				vc.localSsrc = (kind == MediaStreamKind::Video)
					? videoSsrc
					: secondaryVideoSsrc;
				vc.payloadType = (vk == VideoStreamKind::Main) ? 97 : 98;
				vc.codecType = codec::VideoCodecType::H264;
				vc.backend = codec::CodecBackend::Software;

				const auto& resCfg = resource->config;
				if (vk == VideoStreamKind::Main) {
					vc.width = resCfg.mainVideoSinkConfig.width > 0
						? resCfg.mainVideoSinkConfig.width : 1280;
					vc.height = resCfg.mainVideoSinkConfig.height > 0
						? resCfg.mainVideoSinkConfig.height : 720;
				}
				else {
					vc.width = resCfg.secondaryVideoSinkConfig.width > 0
						? resCfg.secondaryVideoSinkConfig.width : 1920;
					vc.height = resCfg.secondaryVideoSinkConfig.height > 0
						? resCfg.secondaryVideoSinkConfig.height : 1080;
				}
				vc.framerate = (vk == VideoStreamKind::Main) ? 30 : 15;
				vc.bitrate = (vk == VideoStreamKind::Main) ? 2000000 : 1000000;
				vc.keyFrameInterval = 60;

				const std::string& cfgSrc =
					(vk == VideoStreamKind::Main) ? resCfg.mainVideoSourceId
					: resCfg.secondaryVideoSourceId;
				const std::string& cfgSink =
					(vk == VideoStreamKind::Main) ? resCfg.mainVideoSinkId
					: resCfg.secondaryVideoSinkId;
				vc.sourceId = cfgSrc.empty() ? defaultSourceId : cfgSrc;
				vc.sinkId = cfgSink.empty() ? defaultSinkId : cfgSink;

				auto rs = std::make_shared<VideoProcessor::ResolvedVideoStream>();
				rs->config = vc;

				if (!vc.sourceId.empty() && impl_->deviceManager_) {
					rs->source = impl_->deviceManager_->GetVideoSource(vc.sourceId);
				}
				if (!vc.sinkId.empty() && impl_->deviceManager_) {
					rs->sink = impl_->deviceManager_->GetVideoSink(vc.sinkId, sinkCfg);
				}
				return rs;
			};

			std::vector<VideoProcessor::ResolvedVideoStream> resolvedStreams;
			if (auto mainStream = mkVideoStream(
				MediaStreamKind::Video, VideoStreamKind::Main,
				"camera:0", "main_window",
				resource->config.mainVideoSinkConfig)) {
				resolvedStreams.push_back(*mainStream);
			}
			if (auto secStream = mkVideoStream(
				MediaStreamKind::SecondaryVideo, VideoStreamKind::Secondary,
				"screen:0", "secondary_window",
				resource->config.secondaryVideoSinkConfig)) {
				resolvedStreams.push_back(*secStream);
			}

			// ---- 构造音频流 ----
			AudioStreamConfig ac;
			ac.localSsrc = audioSsrc;
			// Default fallback only (no negotiation applied): G711U/PT 0.
			// OnMemberCodecNegotiated overwrites codec + send PT before the
			// receive gate opens.
			ac.payloadType = 0;
			ac.codecType = codec::AudioCodecType::G711U;
			ac.backend = codec::CodecBackend::Software;
			ac.sampleRate = 8000;
			ac.channels = 1;
			ac.frameSizeMs = kAudioFrameIntervalMs;

			const auto& resCfg = resource->config;
			ac.sourceId = resCfg.audioSourceId.empty() ? "mic:default" : resCfg.audioSourceId;
			ac.sinkId = resCfg.audioSinkId.empty() ? "speaker:0" : resCfg.audioSinkId;

			AudioProcessor::ResolvedAudioStream ra;
			ra.config = ac;
			if (!ac.sourceId.empty() && impl_->deviceManager_) {
				ra.source = impl_->deviceManager_->GetAudioSource(ac.sourceId);
			}
			if (!ac.sinkId.empty() && impl_->deviceManager_) {
				ra.sink = impl_->deviceManager_->GetAudioSink(ac.sinkId,
					resCfg.audioSinkConfig);
			}

			// ---- 默认编码登记（协商前） ----
			// 与处理器实际启动参数一致：G711U/PT0、H264/PT97|98。SDP 协商
			// 结果在 StartMedia 之后由 OnMemberCodecNegotiated 覆盖；这样协商
			// 缺失时媒体信息显示的是真正在跑的编码，而不是 "NA"。
			if (MediaStatisticsCollector* collector =
					resource->rtpEngine->GetStatisticsCollector()) {
				vcs::NegotiatedCodec audioDefault;
				audioDefault.codecName = "PCMU";
				audioDefault.payloadType = ac.payloadType;
				audioDefault.clockRate = ac.sampleRate;

				vcs::NegotiatedCodec videoDefault;
				videoDefault.codecName = "H264";
				videoDefault.payloadType = 97;
				videoDefault.clockRate = 90000;

				collector->SetNegotiatedCodec(sessionId,
					vcs::MediaStreamKind::Audio, audioDefault);
				collector->SetNegotiatedCodec(sessionId,
					vcs::MediaStreamKind::Video, videoDefault);
				// 辅流默认 PT 98（StartMedia 构造 VideoStreamConfig 时同一取值）
				collector->SetNegotiatedCodec(sessionId,
					vcs::MediaStreamKind::SecondaryVideo, videoDefault);
			}

			// ---- 注入 RTPEngine 并启动 processor ----
			resource->audioProc->SetRtpEngine(resource->rtpEngine);
			resource->videoProc->SetRtpEngine(resource->rtpEngine);

			// RTPEngine → AudioProcessor 接收回调
			{
				auto weakAp = std::weak_ptr<AudioProcessor>(resource->audioProc);
				resource->rtpEngine->SetAudioOutputCallback(
					[weakAp](const std::string& /*memberId*/,
						uint8_t /*pt*/,
						const uint8_t* data, size_t len,
						uint32_t ssrc, uint64_t timestamp,
						uint16_t /*seq*/) {
					if (!data || len < 12) return;
					if (auto ap = weakAp.lock()) {
						ap->OnRtpPacket(ssrc, data + 12, len - 12, timestamp);
					}
				});
			}

			// [WebRTC media engine] NetEq/AEC 之后的 PCM → AudioProcessor 渲染
			{
				auto weakAp = std::weak_ptr<AudioProcessor>(resource->audioProc);
				resource->rtpEngine->SetAudioPlayoutPcmCallback(
					[weakAp](const std::string& memberId,
						const int16_t* pcm, size_t samples,
						int sampleRate, int channels) {
					if (!pcm || samples == 0) return;
					if (auto ap = weakAp.lock()) {
						ap->RenderPcm(memberId, pcm, samples, sampleRate, channels);
					}
				});
			}

			// 远端 PLI/FIR（要求我们发关键帧）→ 本地编码器下一个 AU 出 IDR。
			// 用 weak_ptr：StopMedia 先停收包并摘回调，再停 processor，回调
			// 即便在拆链窗口被触发也只会拿到空指针。
			{
				auto weakVp = std::weak_ptr<VideoProcessor>(resource->videoProc);
				resource->rtpEngine->SetRemoteKeyFrameRequestCallback(
					[weakVp](const std::string& /*memberId*/, vcs::MediaStreamKind kind) {
						if (auto vp = weakVp.lock()) {
							vp->RequestKeyFrame(
								kind == vcs::MediaStreamKind::SecondaryVideo
									? VideoStreamKind::Secondary
									: VideoStreamKind::Main);
						}
					});
			}

			if (!resource->audioProc->Start(sessionId, ra)) {
				VCS_MEDIA_LOG_WARNING("[MediaControl] audioProc->Start failed");
				abortStart();
				return Response::Error(ResponseCode::InternalError, "audio start failed");
			}
			if (!resolvedStreams.empty()) {
				if (!resource->videoProc->Start(sessionId, resolvedStreams)) {
					VCS_MEDIA_LOG_WARNING("[MediaControl] videoProc->Start failed");
					abortStart();
					return Response::Error(ResponseCode::InternalError, "video start failed");
				}
				// [docs/09 §2.4] T1 快路径：媒体就绪即请求远端 IDR。RTCP 发送
				// 独立于收包门，FIR 先行，对方 IDR 编码恰与门打开重叠；
				// SDP 未带 remoteSsrc 时由解码首包的 T2 兜底。
				resource->videoProc->RequestRemoteKeyFrame(VideoStreamKind::Main);
				resource->videoProc->RequestRemoteKeyFrame(VideoStreamKind::Secondary);
			}

			// ---- 每个 kind 创建 MediaTransportCallback + SetCallback ----
			std::vector<std::pair<MediaStreamKind, MediaTransportCallback*>> callbacks;
			impl_->RebuildCallbacks(sessionId, resource->rtpEngine.get(),
				resource->worker.get(), transports, callbacks);
			for (const auto& cb : callbacks) {
				auto tpIt = transports.find(cb.first);
				if (tpIt == transports.end() || !tpIt->second->SetCallback(cb.second)) {
					VCS_MEDIA_LOG_ERROR("[MediaControl] SetCallback failed: kind=%s",
						MediaStreamKindToString(cb.first));
					abortStart();
					return Response::Error(ResponseCode::InternalError, "SetCallback failed");
				}
			}

			// ---- RTPEngine 内部接收门 ----
			// BeginReceive 只装 RTPEngine 的 backend 回调（media 内部门，幂等）。
			// 逐 transport 的 StartReceiving（外部收包门）已移交 gateway 的
			// StartTransport：媒体就绪 → 传输开始收包，由 gateway/会议侧把守
			// （docs/09 §2.2）。
			resource->rtpEngine->BeginReceive();

			// ---- 记录 tempMediaContexts_ ----
			auto ctx = std::make_shared<Impl::ConferenceContext>();
			ctx->confId = confId;
			ctx->sessionId = sessionId;
			ctx->rtpEngine = resource->rtpEngine;
			ctx->videoProcessor = resource->videoProc;
			ctx->audioProcessor = resource->audioProc;
			ctx->worker = resource->worker;
			ctx->videoQueuePool = resource->videoQueuePool;
			ctx->resourceConfig = resource->config;
			impl_->PublishContext(confId, ctx);

			VCS_MEDIA_LOG_INFO("[MediaControl] StartMedia: confId=%s, sessionId=%s, "
				"streams=%zu, videoStreams=%zu",
				confId.c_str(), sessionId.c_str(),
				transports.size(), resolvedStreams.size());
			PublishMediaState(impl_->eventBus_, confId, sessionId, "Started");
			return Response::Success("media started");
		}

		// ============================================================
		// StopMedia
		// ============================================================
		Response MediaControl::StopMedia(const std::string& confId,
			const std::string& sessionId) {
			if (sessionId.empty()) {
				return Response::Error(ResponseCode::InvalidParams, "empty sessionId");
			}

			// 1. 停接收：先拷出传输集，StopReceiving 在锁外调用
			MemberMediaTransports transports;
			{
				std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
				auto tpIt = impl_->memberTransports_.find(sessionId);
				if (tpIt != impl_->memberTransports_.end()) transports = tpIt->second;
			}
			for (const auto& kv : transports) {
				if (kv.second) kv.second->StopReceiving();
			}
			// 收包已停，再显式摘挂：返回后第 3 步删回调对象才安全。
			Impl::DetachCallbacks(transports);

			// 2. 停 processor + 移除 RTPEngine 成员
			std::shared_ptr<ConferenceMediaResource> resource;
			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				auto it = impl_->resources_.find(confId);
				if (it != impl_->resources_.end()) resource = it->second;
			}
			if (resource) {
				// 最终快照必须在停处理器、摘成员之前抓：之后统计库里就没有
				// 这个 session 了。挂断后 UI 仍按 legacy 习惯显示最后一次媒体
				// 信息，所以这份快照要留到下一次 StartMedia。
				MediaStatisticsCollector* collector = resource->rtpEngine
					? resource->rtpEngine->GetStatisticsCollector() : nullptr;
				if (collector) {
					vcs::MediaInfoSnapshot finalInfo;
					if (collector->GetSnapshot(sessionId, finalInfo)) {
						finalInfo.confId = confId;
						finalInfo.sessionId = sessionId;
						finalInfo.live = false;
						std::lock_guard<std::mutex> lock(resource->infoMutex);
						resource->finalInfo = std::move(finalInfo);
					}
				}

				resource->videoProc->Stop();
				resource->audioProc->Stop();
				resource->rtpEngine->RemoveMember(sessionId);
			}

			// 3. 清理
			{
				std::lock_guard<std::mutex> lock(impl_->sessionsMutex_);
				impl_->memberCallbacks_.erase(sessionId);
				impl_->memberTransports_.erase(sessionId);
			}

			VCS_MEDIA_LOG_INFO("[MediaControl] StopMedia: confId=%s, sessionId=%s done",
				confId.c_str(), sessionId.c_str());
			PublishMediaState(impl_->eventBus_, confId, sessionId, "Stopped");
			return Response::Success("media stopped");
		}

		// ============================================================
		// Codec config
		// ============================================================
		Response MediaControl::SetConferenceCodecConfig(
			const std::string& confId,
			const vcs::media_config::VideoCodecConfig& config) {

			auto ctx = impl_->FindContext(confId);
			if (!ctx) {
				return Response::Error(ResponseCode::NotFound, "conference not found");
			}
			ctx->codecConfig = config;
			if (ctx->videoProcessor) {
				ctx->videoProcessor->SetCodec(VideoStreamKind::Main, "H264", 97,
					static_cast<uint16_t>(config.width),
					static_cast<uint16_t>(config.height),
					config.bitrate);
			}
			VCS_MEDIA_LOG_INFO("[MediaControl] SetConferenceCodecConfig: confId=%s, %ux%u@%u",
				confId.c_str(), config.width, config.height, config.bitrate);
			return Response::Success("codec config set");
		}

		vcs::media_config::VideoCodecConfig
			MediaControl::GetConferenceCodecConfig(const std::string& confId) const {
			auto ctx = impl_->FindContext(confId);
			if (!ctx) {
				return vcs::media_config::VideoCodecConfig{};
			}
			return ctx->codecConfig;
		}

		// ============================================================
		// Codec negotiation
		// ============================================================
		void MediaControl::OnMemberCodecNegotiated(
			const std::string& confId,
			const std::string& memberId,
			const NegotiatedCodec& audioCodec,
			const NegotiatedCodec& videoCodec) {

			VCS_MEDIA_LOG_INFO("[MediaControl] OnMemberCodecNegotiated: confId=%s, "
				"audio=%s(pt=%u), video=%s(pt=%u)",
				confId.c_str(),
				audioCodec.codecName.c_str(), audioCodec.payloadType,
				videoCodec.codecName.c_str(), videoCodec.payloadType);

			auto ctx = impl_->FindContext(confId);
			if (!ctx) {
				VCS_MEDIA_LOG_WARNING("[MediaControl] OnMemberCodecNegotiated: no context");
				return;
			}

			if (!audioCodec.codecName.empty() && ctx->audioProcessor) {
				ctx->audioProcessor->SetCodec(audioCodec.codecName, audioCodec.payloadType);
			}

			if (!videoCodec.codecName.empty() && ctx->videoProcessor) {
				const uint16_t w = videoCodec.width > 0 ? videoCodec.width : 0;
				const uint16_t h = videoCodec.height > 0 ? videoCodec.height : 0;
				const uint32_t br = videoCodec.bitrate > 0 ? videoCodec.bitrate : 0;
				ctx->videoProcessor->SetCodec(VideoStreamKind::Main,
					videoCodec.codecName,
					videoCodec.payloadType,
					w, h, br, videoCodec.fmtp);
			}

			// 协商结果落库：媒体信息里展示的"协商到的编码"必须来自 SDP 结果，
			// 而不是处理器上的默认值。辅流与主流共用同一套 SDP 描述，除非将来
			// 单独协商；这里同步登记，active=false 的流由查询方自行忽略。
			if (MediaStatisticsCollector* collector = ctx->rtpEngine
					? ctx->rtpEngine->GetStatisticsCollector() : nullptr) {
				if (!audioCodec.codecName.empty()) {
					collector->SetNegotiatedCodec(ctx->sessionId,
						vcs::MediaStreamKind::Audio, audioCodec);
				}
				if (!videoCodec.codecName.empty()) {
					collector->SetNegotiatedCodec(ctx->sessionId,
						vcs::MediaStreamKind::Video, videoCodec);
					collector->SetNegotiatedCodec(ctx->sessionId,
						vcs::MediaStreamKind::SecondaryVideo, videoCodec);
				}
			}

			if (impl_->observer_) {
				impl_->observer_->OnMemberCodecNegotiated(
					confId, memberId, audioCodec, videoCodec);
			}
		}

		// ============================================================
		// Media info
		//
		// 终端只有一个成员，memberId 就是 sessionId；按会话键扫描资源表，
		// 不假设 confId == sessionId（gateway 传下来的会议号可以不同）。
		// ============================================================
		Response MediaControl::GetMemberMediaInfo(const std::string& memberId,
			vcs::MediaInfoSnapshot& out) const {

			if (memberId.empty()) {
				return Response::Error(ResponseCode::InvalidParams, "empty memberId");
			}

			std::vector<std::shared_ptr<ConferenceMediaResource>> resources;
			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				resources.reserve(impl_->resources_.size());
				for (auto it = impl_->resources_.begin(); it != impl_->resources_.end(); ++it) {
					resources.push_back(it->second);
				}
			}

			for (std::size_t i = 0; i < resources.size(); ++i) {
				if (!resources[i]) continue;
				std::string sessionId;
				{
					std::lock_guard<std::mutex> lock(resources[i]->infoMutex);
					sessionId = resources[i]->sessionId;
				}
				if (sessionId == memberId) {
					return Impl::CollectSnapshot(resources[i], out);
				}
			}

			return Response::Error(ResponseCode::NotFound,
				"unknown member: " + memberId);
		}

		Response MediaControl::GetConferenceMediaInfo(const std::string& confId,
			vcs::MediaInfoSnapshot& out) const {

			if (confId.empty()) {
				return Response::Error(ResponseCode::InvalidParams, "empty confId");
			}

			std::shared_ptr<ConferenceMediaResource> resource;
			{
				std::lock_guard<std::mutex> lock(impl_->resourcesMutex_);
				auto it = impl_->resources_.find(confId);
				if (it != impl_->resources_.end()) resource = it->second;
			}
			if (!resource) {
				return Response::Error(ResponseCode::NotFound,
					"unknown conference: " + confId);
			}
			return Impl::CollectSnapshot(resource, out);
		}

		// ============================================================
		// Observer / config
		// ============================================================
		void MediaControl::SetEventBus(std::shared_ptr<vcs::IEventBus> bus) {
			impl_->eventBus_ = std::move(bus);
			VCS_MEDIA_LOG_INFO("[MediaControl] SetEventBus: %s",
				impl_->eventBus_ ? "injected" : "cleared");
		}

		void MediaControl::SetObserver(IMediaControlObserver* observer) {
			impl_->observer_ = observer;
		}

		IMediaControlObserver* MediaControl::GetObserver() const {
			return impl_->observer_;
		}

		void MediaControl::SetConfig(std::shared_ptr<base::IConfig> config) {
			impl_->config_ = std::move(config);
			VCS_MEDIA_LOG_INFO("[MediaControl] SetConfig: %s",
				impl_->config_ ? "injected" : "cleared");
		}

		// ============================================================
		// Device enumeration
		// ============================================================
		std::vector<std::string> MediaControl::ListVideoSources() const {
			std::vector<std::string> out;
			if (!impl_->deviceManager_) return out;
			for (const auto& info : impl_->deviceManager_->ListVideoSources()) {
				out.push_back(info.id);
			}
			return out;
		}

		std::vector<std::string> MediaControl::ListAudioSources() const {
			if (!impl_->deviceManager_) return {};
			return impl_->deviceManager_->ListAudioSources();
		}

		std::vector<std::string> MediaControl::ListVideoSinks() const {
			std::vector<std::string> out;
			if (!impl_->deviceManager_) return out;
			for (const auto& info : impl_->deviceManager_->ListVideoSinks()) {
				out.push_back(info.id);
			}
			return out;
		}

		std::vector<std::string> MediaControl::ListAudioSinks() const {
			if (!impl_->deviceManager_) return {};
			return impl_->deviceManager_->ListAudioSinks();
		}

	}
} // namespace vcs::media
