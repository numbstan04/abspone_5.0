/**
 * @file NativeRtpBackend.cpp
 * @brief Native UDP RTP backend (Q5, skeleton with sectioned namespaces)
 *
 * This file is organized as four anonymous namespaces per the Q5 design
 * (Section 20.7). The skeleton implements the IRtpBackend contract end-to-end
 * (every virtual method has a body that returns a sensible default), so the
 * class compiles and links even before the 1544 lines of legacy logic are
 * migrated. The actual RTP/RTCP machinery will be moved from the old
 * `RTPEngine.cpp` into the matching namespace as we land each Q5 step.
 *
 * Section map:
 *   namespace NativeRtp::Ingress  -- receive path
 *   namespace NativeRtp::Egress   -- send path
 *   namespace NativeRtp::Ssrc     -- SSRC pool
 *   namespace NativeRtp::Rtcp     -- RTCP compound builder
 *
 * Out-of-namespace: the NativeRtpBackend class itself implements IRtpBackend.
 */

#include "NativeRtpBackend.h"


#include "ILogger.h"

// Transport contract lives in media (vcs::IRtpTransport); gateway implements it.
#include "IRtpTransport.h"

// Full definition of MediaStreamKind (the header only forward-declares it).
#include "MediaStreamKind.h"

#include "Optional.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include <algorithm>

namespace vcs {
	namespace media {

		// =====================================================================
		//  Anonymous namespace: NativeRtp::Ingress
		//  Receive path: demux RTP vs RTCP, parse minimal RTP header fields,
		//  hand the parsed packet to the orchestrator's RtpReceiveCallback.
		// =====================================================================
		namespace NativeRtp {
			namespace Ingress {

				// Parse an RTP header (RFC 3550 Section 5.1). Returns false if `len` is
				// too small to contain even a fixed 12-byte header. The parser also
				// walks past CSRC entries and the optional extension header so that
				// the caller receives the *payload* start (i.e. the first byte after
				// the header) and the payload length. This is what the orchestrator
				// (RTPEngine::HandleParsedRtp) needs to drive H.264 FU-A
				// depacketization, audio decoders, and so on.
				//
				// The parser does NOT validate V==2 (some legacy devices ship V=0
				// keep-alive bytes that the orchestrator might want to inspect)  - 
				// the orchestrator's "short packet" check already drops anything
				// shorter than 12 bytes.
				inline bool ParseRtpHeader(const uint8_t* data, size_t len,
					uint8_t&  outPt, uint32_t& outSsrc, uint64_t& outTimestamp,
					uint16_t& outSeq, size_t& outHeaderLen, bool& outMarkerBit) {
					if (!data || len < 12) return false;
					outMarkerBit = (data[1] & 0x80) != 0;
					outPt = static_cast<uint8_t>(data[1] & 0x7F);
					outSeq = static_cast<uint16_t>((data[2] << 8) | data[3]);
					outTimestamp = (static_cast<uint64_t>(data[4]) << 24) |
						(static_cast<uint64_t>(data[5]) << 16) |
						(static_cast<uint64_t>(data[6]) << 8) |
						static_cast<uint64_t>(data[7]);
					outSsrc = (static_cast<uint32_t>(data[8]) << 24) |
						(static_cast<uint32_t>(data[9]) << 16) |
						(static_cast<uint32_t>(data[10]) << 8) |
						static_cast<uint32_t>(data[11]);
					outHeaderLen = 12 + (static_cast<size_t>(data[0] & 0x0F) * 4);
					if (data[0] & 0x10) {
						if (len < outHeaderLen + 4) return false;
						const uint16_t extLen = static_cast<uint16_t>((data[outHeaderLen + 2] << 8) | data[outHeaderLen + 3]);
						outHeaderLen += 4 + static_cast<size_t>(extLen) * 4;
					}
					if (outHeaderLen > len) return false;
					return true;
				}

				// Convenience overload: callers that don't care about the header
				// length can just call this. Returns false on a too-short packet.
				inline bool ParseRtpHeader(const uint8_t* data, size_t len,
					uint8_t&  outPt, uint32_t& outSsrc, uint64_t& outTimestamp,
					uint16_t& outSeq, bool& outMarkerBit) {
					size_t ignored = 0;
					return ParseRtpHeader(data, len, outPt, outSsrc, outTimestamp, outSeq, ignored, outMarkerBit);
				}

				// Heuristic: RTCP packets have PT in [200, 204] (SR, RR, SDES, BYE, APP).
				// IMPORTANT: do NOT mask the high bit. RFC 3550 Section 6.1 puts the 8-bit PT
				// field in the second byte of an RTCP packet (no M bit in that
				// position), so an RTCP SR has byte 1 = 0xC8 (PT=200). RTP packets
				// use the high bit of byte 1 as the M (marker) bit and only the lower
				// 7 bits as the PT; the `& 0x7F` here would mis-detect a legitimate
				// RTCP packet (PT=200, byte 1 = 0xC8) as RTP with M=1 PT=72. The
				// caller's pre-filter (OnRtpReceived vs OnRtcpReceived) handles the
				// inherent ambiguity for the overlap range [0xC8, 0xCC].
				inline bool IsRtcp(const uint8_t* data, size_t len) {
					if (!data || len < 2) return false;
					const uint8_t pt = data[1];
					return pt >= 200 && pt <= 204;
				}

				// [Q5 step 5d-receive] External ingress entry point. Called by the
				// orchestrator (RTPEngine::OnRtpReceived) after the gateway transport
				// hands a raw UDP datagram to the media layer. This function:
				//   1. Detects RTCP (PT in [200,204]) and dispatches to the
				//      RtcpReceiveCallback with the *raw* bytes (the orchestrator's
				//      RTCP handler is responsible for parsing).
				//   2. Otherwise parses the RTP header (fixed 12 bytes + CSRC +
				//      optional extension) and dispatches the *payload* to the
				//      RtpReceiveCallback with the parsed (pt, ssrc, ts, seq) tuple.
				//
				// The function takes the callbacks by value (copies of the
				// std::function) so the body has no private-member access. The caller
				// snapshots the callbacks under the mutex and passes copies in, which
				// also makes the function naturally re-entrant.
				inline void HandleIncoming(RtpReceiveCallback  on_rtp,
					RtcpReceiveCallback on_rtcp,
					const std::string&  memberId,
					const uint8_t*      data,
					size_t              len) {
					if (IsRtcp(data, len)) {
						if (on_rtcp) on_rtcp(memberId, data, len);
						return;
					}
					uint8_t  pt = 0;
					uint32_t ssrc = 0;
					uint64_t ts = 0;
					uint16_t seq = 0;
					bool     markerBit = false;
					size_t   headerLen = 0;
					if (!ParseRtpHeader(data, len, pt, ssrc, ts, seq, headerLen, markerBit)) return;

					size_t payloadLen = len - headerLen;
					if (data[0] & 0x20) {  // P bit: last octet holds the padding count
						const size_t pad = payloadLen > 0 ? data[len - 1] : 0;
						if (pad == 0 || pad > payloadLen) return;
						payloadLen -= pad;
					}

					if (on_rtp) {
						on_rtp(memberId, pt, data + headerLen, payloadLen, ssrc, ts, seq, markerBit);
					}
				}

			} // namespace Ingress
		} // namespace NativeRtp


		// =====================================================================
		//  Anonymous namespace: NativeRtp::Ingress::H264
		//  H.264 NALU depacketization (RFC 6184). Migrated from
		//  RTPEngine::H264FuADepacketizer as part of Q5 step 5f-receive. Handles
		//  every NAL unit packing mode defined in RFC 6184 Section 5:
		//      1-23  : Single NAL unit packet
		//      24    : STAP-A (Single-Time Aggregation Packet)
		//      25    : STAP-B
		//      26    : MTAP16 (Multi-Time Aggregation Packet, 16-bit offset)
		//      27    : MTAP24 (Multi-Time Aggregation Packet, 24-bit offset)
		//      28    : FU-A (Fragmentation Unit, no DON)
		//      29    : FU-B (Fragmentation Unit, with DON)
		// =====================================================================
		namespace NativeRtp {
			namespace Ingress {
				namespace H264 {

					// H.264 FU-A fragment buffer. Per-member state held by the
					// depacketizer while it waits for fragments to complete a frame.
					struct FragmentBuffer {
						std::vector<uint8_t> data;      // Reassembled complete NAL data
						uint8_t nalType = 0;           // NAL unit type
						uint16_t lastSeq = 0;          // Last packet sequence number
						bool started = false;           // Whether started
						uint64_t timestamp = 0;        // Timestamp
						std::vector<uint8_t> frameBuffer; // 拼接 SPS, PPS, 多个 IDR slice
						bool frameHasKey = false;         // 本帧是否包含 IDR
						uint64_t frameTimestamp = 0;
					};

					// [Q5 step 5f-receive] Full RFC 6184 depacketizer. Migrated from
					// RTPEngine::H264FuADepacketizer::ProcessFuA. The signature now returns
					// the (nalu, isKeyFrame, complete) triple so the orchestrator can
					// decide whether to push the frame to the video queue.
					inline IRtpBackend::H264DepacketizeResult Process(
						std::unordered_map<std::string, FragmentBuffer>& memberBuffers,
						std::mutex& mu, const std::string& memberId,
						const uint8_t* data, size_t size,
						uint16_t sequenceNumber, uint64_t timestamp, bool markerBit) {
						using Result = IRtpBackend::H264DepacketizeResult;
						Result r; r.complete = false; r.isKeyFrame = false;
						std::lock_guard<std::mutex> lock(mu);
						if (size < 2) return r;

						FragmentBuffer& frag = memberBuffers[memberId];

						// [Fix 2026-11-01] Timestamp change = AU boundary. The packet
						// that reveals the boundary belongs to the NEW access unit and
						// must not be thrown away. For an H.264 keyframe the first
						// packet of the AU is the SPS packet, so the old code discarded
						// exactly the parameter set the decoder needs: every keyframe
						// reached FFmpeg as PPS+IDR only ("hasSPS=0"), decode failed and
						// the terminal re-requested an IDR forever.
						//
						// The previous AU is returned as the result of this call; the
						// current packet is then appended to the fresh buffer below.
						bool flushedPrevious = false;
						if (!frag.frameBuffer.empty() && frag.frameTimestamp != timestamp) {
							// Timestamp moved on while an AU was still buffered: the
							// peer did not set the marker bit (or it was lost).
							VCS_MEDIA_LOG_DEBUG("[H264RX] AU delimited by timestamp change "
								"(marker missing): member=%s prevTs=%llu newTs=%llu buffered=%zu",
								memberId.c_str(),
								(unsigned long long)frag.frameTimestamp, (unsigned long long)timestamp,
								frag.frameBuffer.size());
							r.nalu = std::move(frag.frameBuffer);
							r.isKeyFrame = frag.frameHasKey;
							r.complete = true;
							frag.frameBuffer.clear(); frag.frameHasKey = false;
							frag.started = false; frag.data.clear();
							frag.frameTimestamp = timestamp;
							flushedPrevious = true;
						}
						if (frag.frameBuffer.empty()) frag.frameTimestamp = timestamp;

						auto appendToFrame = [&frag](const uint8_t* bytes, size_t len) {
							frag.frameBuffer.insert(frag.frameBuffer.end(), bytes, bytes + len);
						};
						static const uint8_t kSc[4] = { 0x00, 0x00, 0x00, 0x01 };

						uint8_t fuIndicator = data[0];
						uint8_t fuHeader = data[1];
						uint8_t nalType = fuIndicator & 0x1F;

						if (nalType >= 1 && nalType <= 23) {
							appendToFrame(kSc, 4);
							appendToFrame(data, size);
							if (nalType == 5) frag.frameHasKey = true;
						}
						else if (nalType == 24) {
							// STAP-A (RFC 6184 Section 5.7.1): 1-byte STAP header,
							// then repeated [16-bit size][NALU]. Peers commonly
							// aggregate SPS+PPS this way; passing the aggregate
							// through unchanged would hand the decoder a "NALU"
							// whose type byte is 24, so unpack it here.
							size_t off = 1;
							size_t nals = 0;
							while (off + 2 <= size) {
								const size_t nalSize =
									(static_cast<size_t>(data[off]) << 8) |
									static_cast<size_t>(data[off + 1]);
								off += 2;
								if (nalSize == 0 || off + nalSize > size) {
									VCS_MEDIA_LOG_WARNING(
										"[H264] Malformed STAP-A: member=%s, nalSize=%zu, "
										"remaining=%zu; dropping the rest",
										memberId.c_str(), nalSize, size - off);
									break;
								}
								appendToFrame(kSc, 4);
								appendToFrame(data + off, nalSize);
								if ((data[off] & 0x1F) == 5) frag.frameHasKey = true;
								off += nalSize;
								++nals;
							}
							if (nals == 0) {
								VCS_MEDIA_LOG_WARNING(
									"[H264] STAP-A without usable NAL units: member=%s, size=%zu",
									memberId.c_str(), size);
							}
						}
						else if (nalType != 28 && nalType != 29) {
							// 0 reserved, 25 STAP-B, 26/27 MTAP, 30/31 FU-B: all
							// need DON/DONL fields of the interleaved mode we
							// never negotiate. Dropping beats emitting garbage.
							VCS_MEDIA_LOG_WARNING(
								"[H264] Unsupported NAL packing type %u: member=%s, size=%zu; dropped",
								static_cast<unsigned>(nalType), memberId.c_str(), size);
						}
						else {
							bool start = (fuHeader & 0x80) != 0;
							bool end = (fuHeader & 0x40) != 0;
							uint8_t originalNalType = fuHeader & 0x1F;
							size_t fuHeaderSize = (nalType == 29) ? 4 : 2;

							if (frag.started && sequenceNumber != static_cast<uint16_t>(frag.lastSeq + 1)) {
								VCS_MEDIA_LOG_WARNING("[H264] Packet loss detected! expected seq=%u, got=%u. Dropping current frame.",
									frag.lastSeq + 1, sequenceNumber);
								frag.started = false;
								frag.data.clear();
								frag.frameBuffer.clear(); // 丢弃这个已经损坏的完整帧
								frag.frameHasKey = false;

								// 如果不是新的一帧的起始包，直接丢弃该包
								if (!start) return r;
							}

							if (start) {
								frag.data.clear();
								frag.nalType = originalNalType;
								frag.started = true;
								frag.lastSeq = sequenceNumber;
								uint8_t reconstructedHeader = static_cast<uint8_t>((fuIndicator & 0xE0) | originalNalType);
								frag.data.push_back(reconstructedHeader);
							}

							if (frag.started) {
								size_t payloadSize = (size > fuHeaderSize) ? (size - fuHeaderSize) : 0;
								if (payloadSize > 0) {
									frag.data.insert(frag.data.end(), data + fuHeaderSize, data + size);
								}
								frag.lastSeq = sequenceNumber;

								if (end) {
									appendToFrame(kSc, 4);
									appendToFrame(frag.data.data(), frag.data.size());
									if (frag.nalType == 5) frag.frameHasKey = true;
									frag.started = false;
									frag.data.clear();
								}
							}
						}

						// Marker = end of the AU (RFC 6184 / RFC 3550 M bit). This is
						// the primary delimiter; the timestamp-change branch above is
						// only the fallback for a lost marker. When both fire on the
						// same packet (a single-packet AU right after a boundary) the
						// previous AU is already in `r`, so the freshly built one stays
						// buffered and is returned by the next call instead of being
						// dropped.
						if (!flushedPrevious && markerBit && !frag.frameBuffer.empty()) {
							r.nalu = std::move(frag.frameBuffer);
							r.isKeyFrame = frag.frameHasKey;
							r.complete = true;
							frag.frameBuffer.clear(); frag.frameHasKey = false; frag.frameTimestamp = 0;
						}
						return r;
					}

					inline void ClearMember(std::unordered_map<std::string, FragmentBuffer>& memberBuffers,
						std::mutex& mu,
						const std::string& memberId) {
						std::lock_guard<std::mutex> lock(mu);
						memberBuffers.erase(memberId);
					}

					inline void ClearAll(std::unordered_map<std::string, FragmentBuffer>& memberBuffers,
						std::mutex& mu) {
						std::lock_guard<std::mutex> lock(mu);
						memberBuffers.clear();
					}

				} // namespace H264
			} // namespace Ingress
		} // namespace NativeRtp


		// =====================================================================
		//  Anonymous namespace: NativeRtp::Egress
		//  Send path: build the 12-byte RTP header around the caller-supplied
		//  payload and hand the resulting datagram to IRtpTransport.
		// =====================================================================
		namespace NativeRtp {
			namespace Egress {

				// Build a 12-byte RTP header around the payload. Returned vector owns
				// the bytes; the gateway transport reads them synchronously.
				inline std::vector<uint8_t> BuildRtpDatagram(uint8_t  pt,
					uint32_t ssrc,
					uint64_t ts,
					uint16_t seq,
					const uint8_t* payload,
					size_t      payloadLen) {
					std::vector<uint8_t> out(12 + payloadLen);
					// V=2, P=0, X=0, CC=0
					out[0] = 0x80;
					// M=0, PT
					out[1] = static_cast<uint8_t>(pt & 0x7F);
					out[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
					out[3] = static_cast<uint8_t>(seq & 0xFF);
					out[4] = static_cast<uint8_t>((ts >> 24) & 0xFF);
					out[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
					out[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
					out[7] = static_cast<uint8_t>(ts & 0xFF);
					out[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
					out[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
					out[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
					out[11] = static_cast<uint8_t>(ssrc & 0xFF);
					if (payload && payloadLen > 0) {
						std::memcpy(out.data() + 12, payload, payloadLen);
					}
					return out;
				}

			} // namespace Egress
		} // namespace NativeRtp


		// =====================================================================
		//  Anonymous namespace: NativeRtp::Fragment
		//  H.264 NALU fragmentation (RFC 6184). Migrated from
		//  RTPEngine::SendH264WithFragmentation as part of Q5 step 5d-video.
		//  The caller hands us a (possibly multi-NALU) Annex-B byte stream and
		//  we ship it as a series of RTP packets (single NAL or FU-A) through
		//  the supplied sender callback.
		// =====================================================================
		namespace NativeRtp {
			namespace Fragment {

				// H.264 NALU fragmentation parameters (RFC 6184).
				// MTU = 1500, IP header = 20, UDP header = 8, RTP header = 12
				// Max RTP payload ~= 1500 - 20 - 8 - 12 = 1460. We use 1400 as a
				// conservative threshold to leave room for VPN / PPPoE / IP options.
				constexpr size_t MAX_RTP_PAYLOAD_SIZE = 1400;
				constexpr size_t RTP_HEADER_SIZE = 12;
				constexpr size_t FU_A_HEADER_SIZE = 2;

				// Detect an H.264 start code (00 00 01 or 00 00 00 01) at `offset`.
				inline bool IsH264StartCode(const uint8_t* data, size_t offset, size_t size) {
					if (offset + 4 > size) return false;
					if (data[offset] == 0x00 && data[offset + 1] == 0x00) {
						if (data[offset + 2] == 0x01) return true;
						if (offset + 4 <= size && data[offset + 2] == 0x00 && data[offset + 3] == 0x01) return true;
					}
					return false;
				}

				// Extract the 5-bit NAL unit type from the first byte of a NALU.
				inline uint8_t GetH264NaluType(const uint8_t* naluData) {
					return naluData[0] & 0x1F;
				}

				// Send a complete H.264 NALU byte stream, fragmenting as needed.
				//
				// `sendRtpFragment` is called once per RTP packet (single NAL or FU-A
				// fragment). It receives the FULLY ASSEMBLED RTP packet (12-byte
				// header + payload). The caller is responsible for the actual
				// transport send.
				//
				// `data` may contain multiple NAL units in Annex-B format
				// (00 00 00 01 / 00 00 01 prefixed). Each NAL unit is sent
				// independently.
				//
				// Returns the last sequence number used. If `data` is empty, returns
				// `initialSeq` unchanged. On the first fragment the transport
				// rejects, fragmentation stops and `*outError` (when provided)
				// is set to kSendFailed; the caller must not treat the rest of
				// the frame as delivered.
				inline uint16_t FragmentAndSendH264(
					const std::function<vcs::ErrorCode(const uint8_t*, size_t,
						uint16_t, uint64_t, bool)>& sendRtpFragment,
					const uint8_t* data,
					size_t size,
					uint64_t timestamp,
					uint16_t initialSeq,
					uint32_t ssrc,
					uint8_t payloadType,
					vcs::ErrorCode* outError = nullptr) {

					if (!data || size == 0) return initialSeq;

					size_t offset = 0;
					uint16_t currentSeq = initialSeq;
					const size_t maxFragmentSize = MAX_RTP_PAYLOAD_SIZE - RTP_HEADER_SIZE - FU_A_HEADER_SIZE;

					while (offset < size) {
						// Skip the start code that prefixes this NAL unit.
						if (offset == 0 || (offset >= 3 && IsH264StartCode(data, offset, size))) {
							if (data[offset + 2] == 0x01) {
								offset += 3;  // 3-byte start code
							}
							else {
								offset += 4;  // 4-byte start code
							}
							if (offset >= size) break;
						}

						// Find the boundary of the current NAL unit (next start code).
						size_t nalEnd = size;
						for (size_t i = offset + 1; i + 3 <= size; i++) {
							if (IsH264StartCode(data, i, size)) {
								nalEnd = i;
								break;
							}
						}
						size_t currentNalSize = nalEnd - offset;
						if (currentNalSize == 0) break;

						// Is there at least one more NAL unit after this one?
						bool hasMoreNal = false;
						for (size_t i = nalEnd; i + 3 <= size; i++) {
							if (IsH264StartCode(data, i, size)) {
								hasMoreNal = true;
								break;
							}
						}

						// Single-NAL or FU-A?
						bool needFuA = (currentNalSize > MAX_RTP_PAYLOAD_SIZE - RTP_HEADER_SIZE);

						if (!needFuA) {
							// ----- Case 1: single NAL packet (whole NALU fits in MTU).
							// Marker bit is set only on the LAST NAL of the frame
							// (RFC 6184 Section 5.3: "the marker bit ... indicates the last
							// RTP packet of a video frame").
							const bool setMarker = !hasMoreNal;

							std::vector<uint8_t> packet(RTP_HEADER_SIZE + currentNalSize);
							packet[0] = 0x80;                       // V=2, P=0, X=0, CC=0
							packet[1] = (setMarker ? 0x80 : 0x00) | (payloadType & 0x7F);
							packet[2] = (currentSeq >> 8) & 0xFF;
							packet[3] = currentSeq & 0xFF;
							packet[4] = (timestamp >> 24) & 0xFF;
							packet[5] = (timestamp >> 16) & 0xFF;
							packet[6] = (timestamp >> 8) & 0xFF;
							packet[7] = timestamp & 0xFF;
							packet[8] = (ssrc >> 24) & 0xFF;
							packet[9] = (ssrc >> 16) & 0xFF;
							packet[10] = (ssrc >> 8) & 0xFF;
							packet[11] = ssrc & 0xFF;
							std::memcpy(packet.data() + RTP_HEADER_SIZE, data + offset, currentNalSize);

							vcs::ErrorCode rc = sendRtpFragment(
								packet.data(), packet.size(), currentSeq, timestamp, setMarker);
							if (rc != vcs::ErrorCode::Success) {
								if (outError) *outError = rc;
								return static_cast<uint16_t>(currentSeq - 1);
							}
							currentSeq++;
						}
						else {
							// ----- Case 2: FU-A fragmentation.
							const uint8_t naluHeader = data[offset];
							const uint8_t naluType = GetH264NaluType(&naluHeader);
							size_t fuPayloadOffset = offset + 1;     // skip the NAL header byte

							bool startFragment = true;
							size_t fuOffset = fuPayloadOffset;
							while (fuOffset < nalEnd) {
								const size_t fragmentPayloadSize = std::min(maxFragmentSize, nalEnd - fuOffset);
								const bool endFragment = (fuOffset + fragmentPayloadSize >= nalEnd);
								// Marker bit is set only on the LAST fragment of the
								// LAST NAL of the frame (matches the single-NAL case).
								const bool setMarker = endFragment && !hasMoreNal;

								std::vector<uint8_t> fuPacket(RTP_HEADER_SIZE + FU_A_HEADER_SIZE + fragmentPayloadSize);
								fuPacket[0] = 0x80;
								fuPacket[1] = (setMarker ? 0x80 : 0x00) | (payloadType & 0x7F);
								fuPacket[2] = (currentSeq >> 8) & 0xFF;
								fuPacket[3] = currentSeq & 0xFF;
								fuPacket[4] = (timestamp >> 24) & 0xFF;
								fuPacket[5] = (timestamp >> 16) & 0xFF;
								fuPacket[6] = (timestamp >> 8) & 0xFF;
								fuPacket[7] = timestamp & 0xFF;
								fuPacket[8] = (ssrc >> 24) & 0xFF;
								fuPacket[9] = (ssrc >> 16) & 0xFF;
								fuPacket[10] = (ssrc >> 8) & 0xFF;
								fuPacket[11] = ssrc & 0xFF;

								// FU indicator: F(0) NRI(original) Type(28=FU-A)
								fuPacket[12] = static_cast<uint8_t>(0x1C | (naluHeader & 0x60));
								// FU header: S(1) E(1) R(1) Type(5)
								fuPacket[13] = 0;
								if (startFragment) fuPacket[13] |= 0x80;
								if (endFragment)   fuPacket[13] |= 0x40;
								fuPacket[13] |= (naluType & 0x1F);

								std::memcpy(fuPacket.data() + RTP_HEADER_SIZE + FU_A_HEADER_SIZE,
									data + fuOffset, fragmentPayloadSize);

								vcs::ErrorCode rc = sendRtpFragment(
									fuPacket.data(), fuPacket.size(), currentSeq, timestamp, setMarker);
								if (rc != vcs::ErrorCode::Success) {
									if (outError) *outError = rc;
									return static_cast<uint16_t>(currentSeq - 1);
								}

								fuOffset += fragmentPayloadSize;
								currentSeq++;
								startFragment = false;
							}
						}

						// Advance to the next NAL unit.
						offset = nalEnd;
					}

					return static_cast<uint16_t>(currentSeq - 1);
				}

			} // namespace Fragment
		} // namespace NativeRtp


		// =====================================================================
		//  Anonymous namespace: NativeRtp::Ssrc
		//  SSRC pool. The orchestrator pre-allocates SSRCs; this namespace is
		//  here so the future WebRTC backend can plug in a different allocator
		//  (e.g. SSRC tied to PeerConnection) without changing the interface.
		// =====================================================================
		namespace NativeRtp {
			namespace Ssrc {

				// Reserved range constants. The pool itself is owned by RTPEngine, but
				// NativeRtpBackend must reject any SSRC outside the negotiated range to
				// prevent a misbehaving caller from colliding with other conferences.
				inline bool InRange(uint32_t ssrc) {
					return ssrc != 0;  // 0 is reserved ("unassigned")
				}

			} // namespace Ssrc
		} // namespace NativeRtp


		// =====================================================================
		//  Anonymous namespace: NativeRtp::Rtcp
		//  RTCP compound packet builder.
		//
		//  [Q5 step 5h] This namespace was a skeleton in step 5d; it now
		//  contains a full RFC 3550-compliant compound report builder:
		//   - Sender Report        (PT=200, Section 6.4.1)
		//   - Receiver Report      (PT=201, Section 6.4.2)
		//   - SDES (CNAME)         (PT=202, Section 6.5)         -  mandatory per Section 6.1
		//   - Compound assembler   (Section 6.1)                 -  SR/RR + SDES + 32-bit pad
		//
		//  All builders are *pure functions*: they take typed input structs,
		//  allocate a std::vector<uint8_t>, and return it. They do not touch
		//  any per-member or backend state. The orchestrator (RTPEngine) is
		//  responsible for collecting the statistics that feed into the SR/RR
		//  bodies and for triggering a build every 5 seconds (RFC 3550 Section 6.2
		//  minimum interval). The backend is responsible for shipping the
		//  resulting bytes over the wire.
		//
		//  References:
		//   - RFC 3550 Section 5      RTP/RTCP packet format
		//   - RFC 3550 Section 6.1    RTCP packet types (compound)
		//   - RFC 3550 Section 6.4.1  SR: Sender Report
		//   - RFC 3550 Section 6.4.2  RR: Receiver Report
		//   - RFC 3550 Section 6.5    SDES: Source Description
		//   - RFC 3550 Section 4      NTP timestamp format
		// =====================================================================
		namespace NativeRtp {
			namespace Rtcp {

				// --- RFC 3550 Section 6.4.1 common: 24-byte receiver report block -----------
				//
				// One report block accompanies an SR/RR packet. Up to 31 report blocks
				// may be packed into a single SR or RR (RC field is 5 bits). We use
				// the standard 24-byte layout: SSRC of the source being reported on,
				// the 8-bit fraction lost, the 24-bit cumulative number of packets
				// lost, the extended highest sequence number, the interarrival jitter,
				// the middle 32 bits of the NTP timestamp from the most recent SR,
				// and the delay (1/65536-second units) since that SR.
				struct ReportBlock {
					uint32_t ssrc = 0;   // SSRC of the source
					uint8_t  fractionLost = 0;   // 0..255 (RFC 3550 Section A.3)
					uint32_t cumulativeLost = 0;   // 24-bit unsigned (wrap to 24)
					uint32_t extendedHighestSeq = 0;   // high 16 = seq cycles, low 16 = max seq
					uint32_t jitter = 0;   // in RTP timestamp units
					uint32_t lastSrNtp = 0;   // middle 32 bits of last SR's NTP ts
					uint32_t delaySinceLastSr = 0;   // 1/65536 s units since last SR
				};

				// --- RFC 3550 Section 6.4.1 Sender Report -----------------------------------
				//
				// Returned by an endpoint that has *recently* sent RTP data (within
				// the last 5 s). Carries the sender's 64-bit NTP wall clock at the
				// moment the report was generated, the corresponding 32-bit RTP
				// timestamp (so receivers can do clock-skew estimation), and the
				// cumulative count of packets and octets this SSRC has sent.
				// Followed by 0..31 report blocks describing *other* sources this
				// endpoint is listening to.
				struct SenderReportInput {
					uint32_t ssrcSender = 0;   // SSRC of *this* sender
					uint64_t ntpTimestamp = 0;   // 64-bit NTP (RFC 3550 Section 4)
					uint32_t rtpTimestamp = 0;   // RTP clock at the same instant
					uint32_t packetCount = 0;   // packets sent since session start
					uint32_t octetCount = 0;   // octets sent since session start
					std::vector<ReportBlock> reportBlocks;  // 0..31 entries
				};

				// --- RFC 3550 Section 6.4.2 Receiver Report ---------------------------------
				//
				// Returned by an endpoint that has NOT sent any RTP data recently
				// but is still receiving. Same header layout as SR but the 20-byte
				// "sender info" body is empty (just the SSRC of the receiver).
				struct ReceiverReportInput {
					uint32_t ssrcSender = 0;   // SSRC of *this* receiver
					std::vector<ReportBlock> reportBlocks;  // 0..31 entries
				};

				// --- RFC 3550 Section 6.5 SDES chunk ----------------------------------------
				//
				// One SDES "chunk" = 4-byte SSRC/CSRC + 1..N items. We only support
				// the CNAME item (item type 1), which RFC 3550 Section 6.5 marks as
				// mandatory and the only item that every participant must include.
				// The `cname` is a UTF-8 string of arbitrary length, padded to a
				// 32-bit boundary by the SDES builder.
				struct SdesChunk {
					uint32_t ssrc = 0;
					std::string cname;                  // UTF-8, no embedded NULs
				};

				// --- RFC 3550 Section 6.1 compound report input -----------------------------
				//
				// Exactly one of `sender` or `receiver` must be non-null. The SDES
				// chunk is required (CNAME is mandatory). The orchestrator picks
				// SR vs RR based on whether this endpoint has been an active sender
				// in the last reporting interval.
				struct CompoundReportInput {
					vcs::Optional<SenderReportInput>   sender;   // PT=200
					vcs::Optional<ReceiverReportInput> receiver; // PT=201
					SdesChunk                          sdes;     // PT=202, CNAME
				};

				// --- Constants -------------------------------------------------------

				// RFC 3550 Section A.1 / Section A.3
				static constexpr uint8_t  kRtcpVersion = 2;
				static constexpr uint8_t  kRtcpPtSender = 200;  // SR
				static constexpr uint8_t  kRtcpPtReceiver = 201;  // RR
				static constexpr uint8_t  kRtcpPtSdes = 202;  // SDES
				static constexpr uint8_t  kRtcpPtBye = 203;  // BYE (not built here)
				static constexpr uint8_t  kRtcpPtApp = 204;  // APP (not built here)

				// RFC 3550 Section 6.5 SDES item types
				static constexpr uint8_t  kSdesItemCname = 1;

				// RFC 3550 Section 6.1: each compound RTCP packet *must* be sent as a single
				// UDP datagram. The MTU ceiling is 1500 - 20 (IP) - 8 (UDP) = 1472 for
				// Ethernet, so the upper bound on a compound report size is one MTU.
				// We do not enforce that here  -  the orchestrator should rate-limit per
				// Section 6.2 (5 s minimum) which keeps the per-packet body small.
				static constexpr size_t   kRtcpHeaderSize = 4;     // V/P/SC|PT|length
				static constexpr size_t   kRtcpReportBlockSize = 24;   // one report block
				static constexpr size_t   kRtcpSrSenderInfoSize = 24;  // SSRC(4)+NTP(8)+RTP(4)+pkts(4)+octets(4)
				static constexpr size_t   kRtcpRrSenderInfoSize = 4;   // SSRC only

				// --- Network byte order helpers --------------------------------------
				//
				// RTCP uses big-endian (network byte order) on the wire. We write a
				// few trivial inline functions instead of pulling in htonl()/ntohl()
				// so the file stays self-contained on platforms where winsock is not
				// in scope.
				inline void WriteU8(std::vector<uint8_t>& v, uint8_t  x) { v.push_back(x); }
				inline void WriteU16(std::vector<uint8_t>& v, uint16_t x) {
					v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
					v.push_back(static_cast<uint8_t>(x & 0xFF));
				}
				inline void WriteU24(std::vector<uint8_t>& v, uint32_t x) {
					// x is masked to 24 bits by the caller. We write MSB first.
					v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
					v.push_back(static_cast<uint8_t>(x & 0xFF));
				}
				inline void WriteU32(std::vector<uint8_t>& v, uint32_t x) {
					v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
					v.push_back(static_cast<uint8_t>(x & 0xFF));
				}
				inline void WriteU64(std::vector<uint8_t>& v, uint64_t x) {
					v.push_back(static_cast<uint8_t>((x >> 56) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 48) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 40) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 32) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
					v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
					v.push_back(static_cast<uint8_t>(x & 0xFF));
				}

				// RFC 3550 Section 5.1: length field is "the length of the packet in 32-bit
				// words, minus one, including the header". The on-the-wire length
				// field thus stores (totalBytes/4) - 1. We write the header inline
				// in each builder (the deferred-write version was harder to read).

				// Pad the buffer to a 4-byte boundary with zero bytes (RFC 3550 Section 6.1:
				// "the last RTCP packet in a compound packet MUST be padded with
				// zero bits to a 32-bit boundary").
				inline void PadTo32BitBoundary(std::vector<uint8_t>& v) {
					while (v.size() % 4 != 0) v.push_back(0);
				}

				// --- Section 6.4.1 / Section 6.4.2 shared: encode a 24-byte report block -----------
				inline void WriteReportBlock(std::vector<uint8_t>& v, const ReportBlock& rb) {
					WriteU32(v, rb.ssrc);
					WriteU8(v, rb.fractionLost);
					// cumulativeLost is "24-bit signed" per RFC 3550 Section 6.4.1, but the
					// count is always non-negative in practice (we never subtract
					// recoveries). We mask to 24 bits to stay within the wire layout.
					WriteU24(v, rb.cumulativeLost & 0x00FFFFFFu);
					WriteU32(v, rb.extendedHighestSeq);
					WriteU32(v, rb.jitter);
					WriteU32(v, rb.lastSrNtp);
					WriteU32(v, rb.delaySinceLastSr);
				}

				// --- Section 6.4.1 Sender Report --------------------------------------------
				//
				// Layout: header(4) + SSRC(4) + NTP ts(8) + RTP ts(4) + pkts(4) +
				// octets(4) + 0..31 report blocks of 24 bytes each. Total min 28 B.
				inline std::vector<uint8_t> BuildSenderReport(const SenderReportInput& in) {
					// Build the body first so we know its length when we write the
					// header's length field.
					std::vector<uint8_t> body;
					body.reserve(kRtcpSrSenderInfoSize + in.reportBlocks.size() * kRtcpReportBlockSize);
					WriteU32(body, in.ssrcSender);
					WriteU64(body, in.ntpTimestamp);
					WriteU32(body, in.rtpTimestamp);
					WriteU32(body, in.packetCount);
					WriteU32(body, in.octetCount);
					for (const auto& rb : in.reportBlocks) {
						WriteReportBlock(body, rb);
					}
					// SR sender-info is 20 bytes (always 4-byte aligned), so no pad
					// needed for the body itself.

					// RC = min(reportBlocks.size(), 31). The field is 5 bits; 31 is
					// the max allowed by RFC 3550 Section 6.1.
					uint8_t rc = static_cast<uint8_t>(
						in.reportBlocks.size() > 31 ? 31 : in.reportBlocks.size());

					std::vector<uint8_t> out;
					out.reserve(kRtcpHeaderSize + body.size());
					out.push_back(static_cast<uint8_t>((kRtcpVersion << 6) | (0 << 5) | (rc & 0x1F)));
					out.push_back(kRtcpPtSender);
					uint16_t lengthWords = static_cast<uint16_t>((kRtcpHeaderSize + body.size()) / 4 - 1);
					WriteU16(out, lengthWords);
					out.insert(out.end(), body.begin(), body.end());
					return out;
				}

				// --- Section 6.4.2 Receiver Report ------------------------------------------
				//
				// Layout: header(4) + SSRC(4) + 0..31 report blocks of 24 bytes each.
				// Total min 8 B.
				inline std::vector<uint8_t> BuildReceiverReport(const ReceiverReportInput& in) {
					std::vector<uint8_t> body;
					body.reserve(4 + in.reportBlocks.size() * kRtcpReportBlockSize);
					WriteU32(body, in.ssrcSender);
					for (const auto& rb : in.reportBlocks) {
						WriteReportBlock(body, rb);
					}
					// Pad body to a 4-byte boundary if necessary (the report blocks
					// are already 24 bytes each, and the SSRC is 4 bytes, so the
					// body is always 4-byte aligned). We still pad for safety.
					PadTo32BitBoundary(body);

					std::vector<uint8_t> out;
					out.reserve(kRtcpHeaderSize + body.size());
					uint8_t rc = static_cast<uint8_t>(
						in.reportBlocks.size() > 31 ? 31 : in.reportBlocks.size());
					out.push_back(static_cast<uint8_t>((kRtcpVersion << 6) | (0 << 5) | (rc & 0x1F)));
					out.push_back(kRtcpPtReceiver);
					uint16_t lengthWords = static_cast<uint16_t>((kRtcpHeaderSize + body.size()) / 4 - 1);
					WriteU16(out, lengthWords);
					out.insert(out.end(), body.begin(), body.end());
					return out;
				}

				// --- Section 6.5 SDES packet (one chunk) ------------------------------------
				//
				// Layout: header(4) + SSRC(4) + item header(2) + item text + 0..3 pad
				// bytes. The item header is {type(8), length(8)}. We always emit
				// exactly one chunk (RFC 3550 Section 6.5: every participant must include
				// at least the CNAME item, and SC=1 is the most common shape).
				inline std::vector<uint8_t> BuildSdesItem(const SdesChunk& chunk) {
					std::vector<uint8_t> out;
					out.reserve(kRtcpHeaderSize + 4 + 2 + chunk.cname.size() + 3);

					// CNAME item header + text.
					WriteU32(out, chunk.ssrc);
					WriteU8(out, kSdesItemCname);
					// RFC 3550 Section 6.5: SDES item text length is 8 bits. CNAMEs longer
					// than 255 bytes are not representable in a single SDES item; we
					// truncate the tail with a defensive limit (this matches the
					// legacy RTPEngine behaviour).
					if (!chunk.cname.empty()) {
						uint8_t textLen = static_cast<uint8_t>(
							chunk.cname.size() > 255 ? 255 : chunk.cname.size());
						WriteU8(out, textLen);
						for (size_t i = 0; i < textLen; ++i) {
							out.push_back(static_cast<uint8_t>(chunk.cname[i]));
						}
					}
					else {
						// Empty CNAME is unusual but legal; emit type=1, length=0
						// followed by a 1-byte zero terminator (the SDES item list
						// ends with a 4-byte zero  -  we let the pad below produce it).
						WriteU8(out, 0);
					}
					// SDES item list ends with a 4-byte zero word (4 octets of 0).
					// We have already emitted type=1 + length. The next item would
					// be {type=0, length=0, ...} but the simpler RFC-compliant way
					// is to pad to a 32-bit boundary and let the empty terminator
					// be the zero pad bytes. The chunk must end with 0..3 zero bytes
					// to align the next packet on a 32-bit boundary.
					PadTo32BitBoundary(out);

					// Now prepend the SDES header (V=2, P=0, SC=1, PT=202).
					std::vector<uint8_t> result;
					result.reserve(kRtcpHeaderSize + out.size());
					result.push_back(static_cast<uint8_t>((kRtcpVersion << 6) | (0 << 5) | 0x01 /*SC=1*/));
					result.push_back(kRtcpPtSdes);
					uint16_t lengthWords = static_cast<uint16_t>((kRtcpHeaderSize + out.size()) / 4 - 1);
					WriteU16(result, lengthWords);
					result.insert(result.end(), out.begin(), out.end());
					return result;
				}

				// --- Section 6.1 Compound RTCP packet ---------------------------------------
				//
				// Per RFC 3550 Section 6.1, a "compound RTCP packet" consists of:
				//
				//   1. An SR or RR packet (always first, mandatory).
				//   2. Zero or more additional RR packets (we don't emit any).
				//   3. An SDES packet containing a CNAME chunk (always).
				//   4. Optionally a BYE packet (we don't emit one here  -  it is
				//      generated by RemoveMember on the orchestrator side).
				//
				// The whole compound is one UDP datagram. The last packet (SDES) is
				// padded to a 32-bit boundary so the receiver can scan for the next
				// packet by looking for the V=2 bit pattern. We do not enforce an
				// MTU here; the orchestrator is expected to respect RFC 3550 Section 6.2
				// rate limits (5 s minimum) which keeps the report well under 1 KB.
				inline std::vector<uint8_t> BuildCompoundReport(const CompoundReportInput& in) {
					if (!in.sender && !in.receiver) {
						// No SR and no RR: this is not a valid compound per Section 6.1.
						// Return an empty buffer; the caller will surface kInvalidParams.
						return {};
					}
					if (in.sdes.cname.empty()) {
						// RFC 3550 Section 6.1: every compound must contain an SDES with a
						// CNAME chunk. An empty CNAME is allowed by the wire format
						// (we emit type=1, length=0 above) but most stacks will drop
						// the report. The orchestrator is expected to provide a real
						// CNAME; this is a defensive check.
						return {};
					}

					std::vector<uint8_t> srOrRr;
					if (in.sender) {
						srOrRr = BuildSenderReport(*in.sender);
					}
					else {
						srOrRr = BuildReceiverReport(*in.receiver);
					}
					auto sdes = BuildSdesItem(in.sdes);

					std::vector<uint8_t> compound;
					compound.reserve(srOrRr.size() + sdes.size());
					compound.insert(compound.end(), srOrRr.begin(), srOrRr.end());
					compound.insert(compound.end(), sdes.begin(), sdes.end());
					// Per Section 6.1, the LAST packet in a compound is padded to a 32-bit
					// boundary. Our SDES builder already pads its payload, but if the
					// input chunk ended exactly on a 32-bit boundary we need at least
					// one more word so the on-the-wire length field is correct
					// (otherwise the receiver cannot parse the SDES item list  -  the
					// 4-byte terminator would be the next packet's header).
					if (compound.size() % 4 != 0) {
						PadTo32BitBoundary(compound);
					}
					return compound;
				}

				// --- Section 6.1.3 NTP timestamp helper -------------------------------------
				//
				// RFC 3550 Section 4: NTP timestamps are 64-bit values counting seconds since
				// 1900-01-01 00:00:00 UTC. Unix time_t counts seconds since 1970-01-01,
				// so we add the offset (2208988800 s) at the high 32 bits. The low 32
				// bits are the fractional-second part in 1/(2^32) units. This helper
				// is exposed so the orchestrator can call it once per report interval
				// and pass the result into SenderReportInput::ntpTimestamp.
				inline uint64_t UnixSecondsToNtp(uint64_t unixSeconds, uint32_t ntpFraction) {
					return (unixSeconds + 2208988800ULL) << 32 | static_cast<uint64_t>(ntpFraction);
				}

			} // namespace Rtcp
		} // namespace NativeRtp


		// =====================================================================
		//  NativeRtpBackend class implementation
		// =====================================================================

		NativeRtpBackend::NativeRtpBackend()
			: h264_state_(std::make_unique<H264State>()) {}

		// [Q5 step 5f-receive] The concrete H264DepacketizeResult lives in
		// IRtpBackend (the interface owner) so that NativeRtpBackend's override
		// has the same return type as the virtual declaration. NativeRtpBackend.h
		// re-exports it via `using H264DepacketizeResult = IRtpBackend::H264DepacketizeResult;`
		// so callers that prefer the backend-qualified name can still use it.

		// [Q5 step 5f-receive] H264State (pimpl) holds the per-member depacketizer
		// buffers AND the depacketizer's internal mutex. Both types are
		// defined inside the anonymous namespace NativeRtp::Ingress::H264 above;
		// the layout is private to the .cpp.
		struct NativeRtpBackend::H264State {
			std::unordered_map<std::string,
				NativeRtp::Ingress::H264::FragmentBuffer> buffers;
			std::mutex mu;
		};

		NativeRtpBackend::~NativeRtpBackend() {
			Shutdown();
		}

		vcs::ErrorCode NativeRtpBackend::Initialize(const BackendMediaConfig& config) {
			// [Yealink no-video fix] Initialize() is now a pure "remember the
			// config" step. We do NOT touch the `running_` flag here, and we
			// do NOT start any worker threads. The backend stays in the
			// default-deny posture (AddMember returns MediaTransportNotReady)
			// until Start() is called by the orchestrator (RTPEngine).
			//
			// The previous implementation set `running_ = true` here, which
			// assumed that every code path that called AddMember had also
			// called Initialize(). In practice RTPEngine::Initialize(config)
			// only stored the MediaConfig on its own Impl and never forwarded
			// it to backend_->Initialize(), so the flag stayed false forever
			// and every AddMember silently failed with MediaTransportNotReady.
			// The far-end never registered as a transport endpoint, and the
			// call ended with a "video timeout" because no media could be
			// sent.
			//
			// The new contract: a backend is created (constructor), configured
			// (Initialize), and only then started (Start). The orchestrator is
			// responsible for calling Start() once both steps are done.
			cfg_ = config;

			// TODO(step-4): open the UDP socket here, using cfg_.bindAddress
			//               and the per-member localPort from the first
			//               AddMember(). The legacy code lives in
			//               RTPEngine.cpp's Initialize.
			return vcs::ErrorCode::Success;
		}

		void NativeRtpBackend::Start() {
			// [Yealink no-video fix] Flip the running_ flag and spin up the
			// background workers. Idempotent: if Start() is called twice, the
			// second call is a no-op. We use `exchange` (not `store`) to make
			// the no-op detection atomic  -  a non-atomic read-then-set
			// pattern would race with a concurrent Shutdown().
			//
			// We deliberately do NOT touch any per-member state here. AddMember
			// may have been called before Start() in theory, but in practice
			// the orchestrator only calls AddMember after Start(), and even if
			// it didn't, AddMember's `if (!running_)` check at the top would
			// reject those calls with MediaTransportNotReady, so we never end
			// up with a member registered while running_ is false.
			if (running_.exchange(true)) {
				// Already running  -  nothing to do.
				return;
			}

			// Stats emitter thread: ticks every cfg_.statisticsIntervalMs and
			// fires on_stats_() for every registered member. The thread
			// watches `running_` and exits cleanly when Shutdown() flips it
			// back to false.
			//
			// NB: the thread is created with default attributes; if
			// cfg_.statisticsIntervalMs is 0 the StatsLoop floors it to 50 ms
			// so we never busy-loop.
			stats_thread_ = std::thread([this] { StatsLoop(running_); });
		}

		void NativeRtpBackend::Shutdown() noexcept {
			if (!running_.exchange(false)) return;
			if (stats_thread_.joinable()) stats_thread_.join();
			if (io_thread_.joinable())    io_thread_.join();
			{
				std::lock_guard<std::mutex> lk(members_mu_);
				members_.clear();
			}
			{
				std::lock_guard<std::mutex> lk(cb_mu_);
				on_rtp_ = nullptr;
				on_rtcp_ = nullptr;
				on_stats_ = nullptr;
				on_net_event_ = nullptr;
			}
		}

		vcs::ErrorCode NativeRtpBackend::AddMember(
			const std::string& memberId,
			const BackendMemberConfig& config) {
			if (!running_) return vcs::ErrorCode::MediaTransportNotReady;
			if (memberId.empty()) return vcs::ErrorCode::InvalidParams;
    if (config.ssrcAudio != 0 && !NativeRtp::Ssrc::InRange(config.ssrcAudio)) {
        return vcs::ErrorCode::InvalidParams;
    }
    if (config.ssrcVideo != 0 && !NativeRtp::Ssrc::InRange(config.ssrcVideo)) {
        return vcs::ErrorCode::InvalidParams;
    }
    std::lock_guard<std::mutex> lk(members_mu_);
    if (members_.count(memberId)) return vcs::ErrorCode::MemberAlreadyExists;
    auto entry = std::make_unique<MemberEntry>();
    entry->config = config;
    entry->registered = true;
    members_.emplace(memberId, std::move(entry));

			// [Q5 step 5c] Register the SSRCs in the routing index. The audio
			// and video SSRCs both belong to the same member (the orchestrator
			// pre-allocated them and handed them in), so we record both under
			// the same memberId. We do not validate uniqueness here  -  if the
			// orchestrator accidentally passes the same SSRC for two members,
			// the second AddMember will overwrite the first; this is the
			// orchestrator's bug to catch, and matches the legacy
			// RTPEngine::Impl::ssrcToMember semantics 1:1.
			if (config.ssrcAudio != 0) ssrcToMember_[config.ssrcAudio] = memberId;
			if (config.ssrcVideo != 0 && config.ssrcVideo != config.ssrcAudio) {
				ssrcToMember_[config.ssrcVideo] = memberId;
			}
			return vcs::ErrorCode::Success;
		}

		void NativeRtpBackend::RemoveMember(const std::string& memberId) noexcept {
			std::lock_guard<std::mutex> lk(members_mu_);

			// [Q5 step 5c] Tear down the SSRC routing entries that belong to
			// this member. We look the member up first so we can remove the
			// correct (ssrcAudio, ssrcVideo) pair from the index even if the
			// MemberEntry itself is gone (e.g. Shutdown() already cleared
			// members_). The lookup is O(1).
			auto it = members_.find(memberId);
			if (it != members_.end() && it->second) {
				const auto& cfg = it->second->config;
				if (cfg.ssrcAudio != 0) ssrcToMember_.erase(cfg.ssrcAudio);
				if (cfg.ssrcVideo != 0 && cfg.ssrcVideo != cfg.ssrcAudio) {
					ssrcToMember_.erase(cfg.ssrcVideo);
				}
			}
			else {
				// Member already gone from members_. As a defensive measure,
				// scan ssrcToMember_ for any entry whose value is this
				// memberId and remove it. This keeps the index consistent
				// even if Shutdown() / a partial init / a duplicate remove
				// left dangling entries.
				for (auto sit = ssrcToMember_.begin(); sit != ssrcToMember_.end(); ) {
					if (sit->second == memberId) sit = ssrcToMember_.erase(sit);
					else ++sit;
				}
			}

			members_.erase(memberId);
		}

		void NativeRtpBackend::UpdateMemberSsrc(
			const std::string& memberId,
			uint32_t audioSsrc,
			uint32_t videoSsrc,
			uint32_t h239Ssrc) noexcept {
			// [Q5 step 5g] SDP renegotiation can change audioSsrc / videoSsrc /
			// h239Ssrc on a live member. We must:
			//   1. Remove the OLD SSRCs from ssrcToMember_ (otherwise a stale
			//      mapping for a different member would be returned if the new
			//      audio/video SSRC collides with something we previously
			//      handed out).
			//   2. Update the MemberEntry's config (so subsequent SendRtp /
			//      FindMemberBySsrc see the latest SSRCs).
			//   3. Register the NEW SSRCs in ssrcToMember_ under the same
			//      memberId.
			//
			// Zero is treated as "do not change that slot"  -  matches the
			// legacy RTPEngine::UpdateMemberSsrc behaviour where zero values
			// were ignored.
			if (memberId.empty()) return;
			std::lock_guard<std::mutex> lk(members_mu_);

    auto it = members_.find(memberId);
    if (it == members_.end() || !it->second) {
        // [Fix 2026-08-12] Auto-recover: the orchestrator can call
        // UpdateMemberSsrc with a learned SSRC for a member that
        // never made it into members_ (typically because the
        // preceding AddMember was rejected upstream for some
        // transient reason, or the orchestrator learns the remote
        // SSRC before AddMember reaches this backend). Previously
        // we silently bailed out, leaving ssrcToMember_ empty for
        // that member  -  FindMemberBySsrc would return "" and the
        // incoming RTP would be dropped. Create a placeholder
        // MemberEntry so the SSRC index gets populated.
        auto newEntry = std::make_unique<MemberEntry>();
        newEntry->config.memberId  = memberId;
        newEntry->config.ssrcAudio = audioSsrc;  // best-effort seed
        newEntry->config.ssrcVideo = videoSsrc;
        newEntry->registered = true;
        members_.emplace(memberId, std::move(newEntry));
        it = members_.find(memberId);
    }

			auto& cfg = it->second->config;

			// --- 1. Remove the OLD audio/video/h239 SSRCs from the index.
			// Skip the case where the old and new value are equal (the caller
			// is just re-asserting the same SSRC).
			if (audioSsrc != 0 && audioSsrc != cfg.ssrcAudio) {
				ssrcToMember_.erase(cfg.ssrcAudio);
			}
			if (videoSsrc != 0 && videoSsrc != cfg.ssrcVideo) {
				ssrcToMember_.erase(cfg.ssrcVideo);
			}
			// h239Ssrc is optional (0 = not used). Only the orchestrator's
			// UpdateMemberSsrc knows the previous h239Ssrc, but we only
			// remember audio+video in MemberEntry. For the common case of
			// h239Ssrc changing from X to Y we will not erase the old X; if
			// X collides with a future member's ssrcAudio, that future
			// member's AddMember will overwrite the mapping (the legacy code
			// did the same  -  h239Ssrc was never in the routing index on
			// RTPEngine either). This is a documented limitation, not a bug.

			// --- 2. Update MemberEntry config (in place).
			if (audioSsrc != 0) cfg.ssrcAudio = audioSsrc;
			if (videoSsrc != 0) cfg.ssrcVideo = videoSsrc;
			// h239Ssrc is not stored in BackendMemberConfig (the native
			// backend only sends the two primary streams). The orchestrator
			// keeps h239Ssrc on its side and routes through the same
			// MemberMediaContext.

			// --- 3. Register the NEW SSRCs in ssrcToMember_.
			if (audioSsrc != 0) ssrcToMember_[audioSsrc] = memberId;
			if (videoSsrc != 0 && videoSsrc != audioSsrc) {
				ssrcToMember_[videoSsrc] = memberId;
			}
		}

		bool NativeRtpBackend::FindMemberBySsrc(
			uint32_t ssrc, std::string& outMemberId) const {
			if (ssrc == 0) return false;
			std::lock_guard<std::mutex> lk(members_mu_);
			auto it = ssrcToMember_.find(ssrc);
			if (it == ssrcToMember_.end()) return false;
			outMemberId = it->second;
			return true;
		}

		void NativeRtpBackend::SetTransports(
			const std::string& memberId,
			const MemberMediaTransports& transports) noexcept {
			// [V7.0] Replace the legacy single-transport `SetTransport` with a
			// per-kind map. The map may legally be empty (called before SDP is
			// fully negotiated); we still store the empty map so subsequent
			// SetTransports calls can add kinds incrementally.
			//
			// Thread-safety: same model as the old SetTransport: take the
			// members_ lock, look the member up, and either copy the map in or
			// drop the call if the member is unknown. The caller (RTPEngine)
			// discovers the mismatch later via kMemberNotFound on Send*.
    std::lock_guard<std::mutex> lk(members_mu_);
    auto it = members_.find(memberId);
    if (it == members_.end() || !it->second) {
        auto newEntry = std::make_unique<MemberEntry>();
        newEntry->config.memberId = memberId;
        newEntry->registered = true;
        members_.emplace(memberId, std::move(newEntry));
        it = members_.find(memberId);
    }
    it->second->transports = transports;
}

		vcs::ErrorCode NativeRtpBackend::SendRtp(
			const std::string& memberId,
			::vcs::MediaStreamKind kind,
			uint8_t  payloadType,
			const uint8_t* data, size_t len,
			uint32_t ssrc, uint64_t timestamp, uint16_t sequenceNumber) {
			// [V7.0] Look up the per-kind transport. The `kind` argument is the
			// single piece of information that disambiguates audio from video
			// traffic on the wire; the lookup either finds the matching
			// transport or returns MediaTransportNotReady. The audio/video cross-port
			// bug is structurally impossible here because there is no longer
			// any payload-type-based routing  -  the caller (RTPEngine) decided
			// which kind this packet is BEFORE it called us.
			std::shared_ptr<IRtpTransport> tp;
			{
				std::lock_guard<std::mutex> lk(members_mu_);
				auto it = members_.find(memberId);
				if (it == members_.end() || !it->second) {
					return vcs::ErrorCode::MemberNotFound;
				}
				auto tit = it->second->transports.find(kind);
				if (tit == it->second->transports.end() || !tit->second) {
					return vcs::ErrorCode::MediaTransportNotReady;
				}
				tp = tit->second;
			}
			auto datagram = NativeRtp::Egress::BuildRtpDatagram(
				payloadType, ssrc, timestamp, sequenceNumber, data, len);
			::vcs::RtpPacket pkt;
			pkt.data = datagram.data();
			pkt.length = datagram.size();
			try {
				// The transport returns false when the datagram never left
				// the socket (not connected / no transport / send failed).
				// Reporting Success here made RTPEngine count dropped
				// packets as sent, so a dead transport looked healthy.
				if (!tp->SendRtp(pkt)) {
					return vcs::ErrorCode::MediaSendFailed;
				}
				return vcs::ErrorCode::Success;
			}
			catch (...) {
				return vcs::ErrorCode::MediaSendFailed;
			}
		}

		vcs::ErrorCode NativeRtpBackend::SendH264Nalu(
			const std::string& memberId,
			::vcs::MediaStreamKind kind,
			uint8_t  payloadType,
			const uint8_t* naluData, size_t naluLen,
			uint32_t ssrc, uint64_t timestamp, uint16_t sequenceNumber,
			uint16_t* outLastSeq) {
			// [Q5 step 5d-video] The whole H.264 fragmentation logic now lives
			// in the backend. The orchestrator (RTPEngine) hands us a raw
			// Annex-B byte stream (possibly multiple NAL units), and we:
			//
			//   1. Look up the per-kind transport for `memberId` (once, outside
			//      the fragment loop  -  the lock would otherwise be acquired
			//      once per fragment).
			//   2. Run RFC 6184 single-NAL vs FU-A fragmentation.
			//   3. For each fragment, build a 12-byte RTP header and hand the
			//      full datagram to the gateway transport.
			//
			// Sequence numbers roll forward inside the fragmentation loop.
			// The first fragment uses the caller-supplied `sequenceNumber`;
			// subsequent fragments use ++seq.
			std::shared_ptr<IRtpTransport> tp;
			{
				std::lock_guard<std::mutex> lk(members_mu_);
				auto it = members_.find(memberId);
				if (it == members_.end() || !it->second) {
					return vcs::ErrorCode::MemberNotFound;
				}
				auto tit = it->second->transports.find(kind);
				if (tit == it->second->transports.end() || !tit->second) {
					return vcs::ErrorCode::MediaTransportNotReady;
				}
				tp = tit->second;
			}

			// Per-fragment sender: wrap a complete RTP datagram and call the
			// gateway transport. We catch the throw to surface kSendFailed,
			// and honour the transport's false return (packet dropped).
			auto sendFragment = [tp](const uint8_t* rtpData, size_t rtpSize,
				uint16_t /*seq*/, uint64_t /*ts*/, bool /*marker*/) -> vcs::ErrorCode {
				::vcs::RtpPacket pkt;
				pkt.data = const_cast<uint8_t*>(rtpData);
				pkt.length = rtpSize;
				try {
					if (!tp->SendRtp(pkt)) {
						return vcs::ErrorCode::MediaSendFailed;
					}
					return vcs::ErrorCode::Success;
				}
				catch (...) {
					return vcs::ErrorCode::MediaSendFailed;
				}
			};

			vcs::ErrorCode sendError = vcs::ErrorCode::Success;
			uint16_t lastSeq = NativeRtp::Fragment::FragmentAndSendH264(
				sendFragment,
				naluData, naluLen,
				timestamp, sequenceNumber,
				ssrc, payloadType, &sendError);

			if (outLastSeq) *outLastSeq = lastSeq;

			// A frame whose fragments failed is NOT delivered. Returning
			// Success unconditionally hid dead transports from RTPEngine.
			return sendError;
		}

		vcs::ErrorCode NativeRtpBackend::SendRtcp(
			const std::string& memberId,
			::vcs::MediaStreamKind kind,
			const uint8_t* data, size_t len) {
			// [V7.0] Same per-kind lookup as SendRtp. RTCP is normally co-located
			// with RTP on the same per-kind transport, but a backend that ships
			// RTCP on a separate socket can register a second transport for the
			// same kind; the lookup handles that correctly because the map
			// keys on `kind` (the same key for both RTP and RTCP on the same
			// stream).
			std::shared_ptr<IRtpTransport> tp;
			{
				std::lock_guard<std::mutex> lk(members_mu_);
				auto it = members_.find(memberId);
				if (it == members_.end() || !it->second) {
					return vcs::ErrorCode::MemberNotFound;
				}
				auto tit = it->second->transports.find(kind);
				if (tit == it->second->transports.end() || !tit->second) {
					return vcs::ErrorCode::MediaTransportNotReady;
				}
				tp = tit->second;
			}
			if (!data || len == 0) return vcs::ErrorCode::InvalidParams;
			::vcs::RtcpPacket pkt;
			// RtcpPacket::data is uint8_t* (mutable). The IRtpBackend contract
			// passes const uint8_t* because the backend is not supposed to mutate
			// the payload, but the gateway's socket layer is non-const. Cast
			// away for the crossing. (Same wart as the legacy RTPEngine::SendRtcp.)
			pkt.data = const_cast<uint8_t*>(data);
			pkt.length = len;
			try {
				// 传输层返回 false 说明包根本没发出去（未连接/无 transport/
				// send_rtcp 失败）。之前忽略返回值会让上层把 FIR 失败记成
				// 成功，现场无法从日志判断关键帧请求到底有没有离开本机。
				if (!tp->SendRtcp(pkt)) {
					return vcs::ErrorCode::MediaSendFailed;
				}
				return vcs::ErrorCode::Success;
			}
			catch (...) {
				return vcs::ErrorCode::MediaSendFailed;
			}
		}

		std::vector<uint8_t> NativeRtpBackend::BuildRtcpReport(
			const std::string& /*memberId*/,
			const RtcpReportRequest& req) {
			// [Q5 step 5h] Pure bridge between the IRtpBackend contract and
			// the NativeRtp::Rtcp builder. This function holds NO per-member
			// state and is therefore safe to call concurrently with anything
			// else. The orchestrator (RTPEngine) is expected to:
			//
			//   1. Snapshot the MediaStatisticsCollector for `memberId`.
			//   2. Convert the cumulative counters into ReportBlock fields
			//      (fraction_lost, cumulative_lost, jitter, etc.).
			//   3. Call BuildRtcpReport with a stable CNAME.
			//   4. Hand the returned bytes to SendRtcp() for shipping.
			//
			// The memberId parameter is currently unused; the report content
			// is fully described by `req`. We keep it in the signature so
			// a future variant that needs per-member state (e.g. cached
			// CNAME) can be added without breaking the interface.

			// ---- Defensive checks ----
			if (req.ssrcSender == 0) {
				// RFC 3550 Section 6.1: every compound report must identify a
				// sender. A zero SSRC is reserved ("unassigned") and is not
				// a valid report subject. Return an empty vector so the
				// orchestrator can detect the failure (and so that SendRtcp
				// is never called with an empty payload).
				return {};
			}
			if (req.cname.empty()) {
				// CNAME is mandatory per Section 6.1. An empty CNAME would still
				// produce a valid wire format, but most stacks (WebRTC
				// especially) drop reports without a CNAME. Treat as a
				// configuration error and return empty.
				return {};
			}
			if (req.reportBlocks.size() > 31) {
				// RC field is 5 bits; max 31 report blocks. Truncate the
				// tail to keep the on-the-wire RC field consistent. The
				// orchestrator should rate-limit to a small number of
				// report blocks anyway (RFC 3550 Section 6.2: even at 200 pps
				// a single receiver rarely has > 5 sources to report on).
				// We truncate rather than fail to keep the call site simple.
				// (Returning the truncated report is a deliberate design
				// choice  -  the alternative would be to return empty and
				// force the orchestrator to re-try, which is worse for
				// jitter-sensitive RTCP timing.)
			}

			// ---- Translate IRtpBackend::RtcpReportBlock -> internal
			//      NativeRtp::Rtcp::ReportBlock ----
			//
			// The two types are structurally identical. We could add a
			// free conversion, but a one-liner loop keeps the boundary
			// explicit and lets us evolve the two independently.
			NativeRtp::Rtcp::CompoundReportInput compound;
			compound.sdes.ssrc = req.ssrcSender;
			compound.sdes.cname = req.cname;

			if (req.generateSenderReport) {
				NativeRtp::Rtcp::SenderReportInput sr;
				sr.ssrcSender = req.ssrcSender;
				sr.ntpTimestamp = req.ntpTimestamp;
				sr.rtpTimestamp = req.rtpTimestamp;
				sr.packetCount = req.packetCount;
				sr.octetCount = req.octetCount;
				sr.reportBlocks.reserve(req.reportBlocks.size());
				for (const auto& rb : req.reportBlocks) {
					NativeRtp::Rtcp::ReportBlock out;
					out.ssrc = rb.ssrc;
					out.fractionLost = rb.fractionLost;
					out.cumulativeLost = rb.cumulativeLost;
					out.extendedHighestSeq = rb.extendedHighestSeq;
					out.jitter = rb.jitter;
					out.lastSrNtp = rb.lastSrNtp;
					out.delaySinceLastSr = rb.delaySinceLastSr;
					sr.reportBlocks.push_back(out);
				}
				// Truncate to 31 if necessary (RC field is 5 bits).
				if (sr.reportBlocks.size() > 31) {
					sr.reportBlocks.resize(31);
				}
				compound.sender = std::move(sr);
			}
			else {
				NativeRtp::Rtcp::ReceiverReportInput rr;
				rr.ssrcSender = req.ssrcSender;
				rr.reportBlocks.reserve(req.reportBlocks.size());
				for (const auto& rb : req.reportBlocks) {
					NativeRtp::Rtcp::ReportBlock out;
					out.ssrc = rb.ssrc;
					out.fractionLost = rb.fractionLost;
					out.cumulativeLost = rb.cumulativeLost;
					out.extendedHighestSeq = rb.extendedHighestSeq;
					out.jitter = rb.jitter;
					out.lastSrNtp = rb.lastSrNtp;
					out.delaySinceLastSr = rb.delaySinceLastSr;
					rr.reportBlocks.push_back(out);
				}
				if (rr.reportBlocks.size() > 31) {
					rr.reportBlocks.resize(31);
				}
				compound.receiver = std::move(rr);
			}

			return NativeRtp::Rtcp::BuildCompoundReport(compound);
		}

		void NativeRtpBackend::SetRtpReceiveCallback(RtpReceiveCallback cb) {
			std::lock_guard<std::mutex> lk(cb_mu_); on_rtp_ = std::move(cb);
		}
		void NativeRtpBackend::SetRtcpReceiveCallback(RtcpReceiveCallback cb) {
			std::lock_guard<std::mutex> lk(cb_mu_); on_rtcp_ = std::move(cb);
		}
		void NativeRtpBackend::SetStatisticsCallback(BackendStatisticsCallback cb) {
			std::lock_guard<std::mutex> lk(cb_mu_); on_stats_ = std::move(cb);
		}
		void NativeRtpBackend::SetNetworkEventCallback(NetworkEventCallback cb) {
			std::lock_guard<std::mutex> lk(cb_mu_); on_net_event_ = std::move(cb);
		}

		void NativeRtpBackend::OnRtpReceived(const std::string& memberId,
			const uint8_t* data, size_t len) {
			// [Q5 step 5d-receive] External ingress entry point. The orchestrator
			// (RTPEngine::OnRtpReceived) hands us the raw UDP datagram that the
			// gateway transport just read. We:
			//   1. Snapshot the registered callbacks under the callback lock.
			//   2. Delegate to NativeRtp::Ingress::HandleIncoming, which parses
			//      the RTP header (or detects RTCP) and fires the appropriate
			//      callback with the parsed fields.
			//
			// We pass the callbacks by value (copies of the std::function) so
			// that the implementation has no private-member access and is
			// re-entrant  -  a callback that re-enters OnRtpReceived will see
			// the latest registered callback, not the one we are about to fire.
			if (!data || len == 0) return;
			RtpReceiveCallback  rtp_cb;
			RtcpReceiveCallback rtcp_cb;
			{
				std::lock_guard<std::mutex> lk(cb_mu_);
				rtp_cb = on_rtp_;
				rtcp_cb = on_rtcp_;
			}
			NativeRtp::Ingress::HandleIncoming(
				std::move(rtp_cb), std::move(rtcp_cb), memberId, data, len);
		}

		void NativeRtpBackend::OnRtcpReceived(const std::string& memberId,
			const uint8_t* data, size_t len) {
			// [Q5 step 5d-receive] RTCP counterpart of OnRtpReceived. The
			// orchestrator's MediaControl layer splits RTP and RTCP at the
			// gateway transport level, so by the time we get here we know the
			// bytes are an RTCP compound packet. We hand them straight to the
			// registered RtcpReceiveCallback.
			//
			// (The RTCP path is intentionally not demuxed the way OnRtpReceived
			// is  -  the RTCP callback in RTPEngine just forwards raw bytes to
			// impl_->rtcpCallback, so there is no value in re-detecting RTCP
			// here. If a future backend needs SR/RR/SDES/... extraction at the
			// transport level, the right place is a separate RtcpParse step
			// inside NativeRtp::Rtcp, not this entry point.)
			if (!data || len == 0) return;
			RtcpReceiveCallback rtcp_cb;
			{
				std::lock_guard<std::mutex> lk(cb_mu_);
				rtcp_cb = on_rtcp_;
			}
			if (rtcp_cb) rtcp_cb(memberId, data, len);
		}

		void NativeRtpBackend::OnRtpReceivedParsed(
			const std::string& memberId, uint8_t payloadType, const uint8_t* data, size_t len,
			uint32_t ssrc, uint64_t timestamp, uint16_t sequenceNumber, bool markerBit) {
			if (!data || len == 0) return;
			RtpReceiveCallback cb;
			{
				std::lock_guard<std::mutex> lk(cb_mu_);
				cb = on_rtp_;
			}
			if (cb) {
				cb(memberId, payloadType, data, len, ssrc, timestamp, sequenceNumber, markerBit);
			}
		}

		vcs::ErrorCode NativeRtpBackend::SetSendBandwidth(
			const std::string& memberId, uint32_t bps) {
			std::lock_guard<std::mutex> lk(members_mu_);
			if (!members_.count(memberId)) return vcs::ErrorCode::MemberNotFound;
			members_[memberId]->config.remotePort = static_cast<uint16_t>(
				(bps > 0 && bps < 65536) ? bps : members_[memberId]->config.remotePort);
			return vcs::ErrorCode::Success;
		}

		vcs::ErrorCode NativeRtpBackend::SetFecEnabled(
			const std::string& /*memberId*/, bool /*enable*/) {
			// Native UDP backend does not implement FEC. The orchestrator should
			// route FEC requests to a different backend.
			return vcs::ErrorCode::NotImplemented;
		}

		std::shared_ptr<IRtpTransport> NativeRtpBackend::GetTransport(
			const std::string& memberId,
			::vcs::MediaStreamKind kind) {
			// [V7.0] Per-kind accessor. Returns nullptr if either the member
			// is unknown or the kind was never registered. Mirrors the old
			// single-transport GetTransport() but with an explicit kind
			// selector, so legacy callers that used to ask for "the" transport
			// now have to pick a kind.
			std::lock_guard<std::mutex> lk(members_mu_);
			auto it = members_.find(memberId);
			if (it == members_.end() || !it->second) return nullptr;
			auto tit = it->second->transports.find(kind);
			if (tit == it->second->transports.end()) return nullptr;
			return tit->second;
		}

		// ---- internal helpers -------------------------------------------------

		bool NativeRtpBackend::ParseRtpHeader(const uint8_t* data, size_t len,
			uint8_t& outPt, uint32_t& outSsrc, uint64_t& outTs, uint16_t& outSeq, bool& outMarkerBit) const {
			return NativeRtp::Ingress::ParseRtpHeader(data, len, outPt, outSsrc, outTs, outSeq, outMarkerBit);
		}

		bool NativeRtpBackend::IsRtcp(const uint8_t* data, size_t len) const {
			return NativeRtp::Ingress::IsRtcp(data, len);
		}

		void NativeRtpBackend::StatsLoop(std::atomic<bool>& running) {
			// C++11: use std::chrono::duration<int, std::milli> directly (no chrono_literals)
			auto period = std::max(50, cfg_.statisticsIntervalMs);
			while (running.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(period));
				if (!running.load()) break;
				// Snapshot the member list under the lock, then call back outside
				// the lock to avoid deadlocks if the callback re-enters the backend.
				std::vector<std::string> ids;
				{
					std::lock_guard<std::mutex> lk(members_mu_);
					ids.reserve(members_.size());
					for (auto& kv : members_) ids.push_back(kv.first);
				}
				BackendStatisticsCallback cb;
				{
					std::lock_guard<std::mutex> lk(cb_mu_);
					cb = on_stats_;
				}
				if (!cb) continue;
				for (const auto& id : ids) {
					BackendStatistics s;   // TODO(step-4): fill from MediaStatistics
					cb(id, s);
				}
			}
		}

		// =====================================================================
		//  H.264 FU-A depacketization (Q5 step 5f-receive)
		//
		//  These three methods used to live on RTPEngine::Impl::fuaDepacketizer.
		//  They are now part of the backend because the depacketizer is a
		//  protocol-layer concern: it knows about RFC 6184 NAL unit packing
		//  modes, FU-A/FU-B FU headers, sequence-number wraparound, etc. The
		//  orchestrator just calls DepacketizeH264() and gets a complete NAL
		//  unit back (or nothing, if the depacketizer is still waiting on
		//  fragments).
		// =====================================================================

		IRtpBackend::H264DepacketizeResult NativeRtpBackend::DepacketizeH264(
			const std::string& memberId, const uint8_t* payload, size_t payloadSize,
			uint16_t sequenceNumber, uint64_t timestamp, bool markerBit) {
			if (!h264_state_) {
				h264_state_ = std::make_unique<H264State>();
			}
			return NativeRtp::Ingress::H264::Process(
				h264_state_->buffers, h264_state_->mu,
				memberId, payload, payloadSize, sequenceNumber, timestamp, markerBit);
		}

		void NativeRtpBackend::ClearH264Depacketizer(const std::string& memberId) {
			if (!h264_state_) return;
			NativeRtp::Ingress::H264::ClearMember(
				h264_state_->buffers, h264_state_->mu, memberId);
		}

		void NativeRtpBackend::ClearAllH264Depacketizers() {
			if (!h264_state_) return;
			NativeRtp::Ingress::H264::ClearAll(
				h264_state_->buffers, h264_state_->mu);
		}

	} // namespace media
} // namespace vcs
