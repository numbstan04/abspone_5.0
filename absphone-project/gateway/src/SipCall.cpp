#include "SipCall.h"
#include "ExternalSdpBuilder.h"
#include "SdpCodecUtil.h"
#include "ILogger.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>

// PJSIP always enabled
#include <pjsua.h>
#include <pjsua-lib/pjsua_internal.h>
#include <pjsip.h>
#include <pjsip/sip_dialog.h>
#include <pjmedia.h>
#include <pjmedia/stream.h>     // for pjmedia_stream_pause
#include <pjmedia/vid_stream.h>
#include <pjmedia/sdp.h>        // for pjmedia_sdp_session, pjmedia_sdp_media, pjmedia_sdp_attr
#include <pjmedia/sdp_neg.h>   // for SDP negotiation
#include <pjlib.h>
#include <cstring>

namespace vcs { namespace gateway {

// RTP payload type constants
namespace {
    enum : uint8_t {
        PAYLOAD_PCMU = 0,
        PAYLOAD_PCMA = 8,
        PAYLOAD_G722 = 9,
        PAYLOAD_H264 = 97,
        PAYLOAD_VP8 = 100,
        PAYLOAD_VP9 = 101,
        PAYLOAD_OPUS = 111,
        PAYLOAD_H265 = 102,
    };
}

SipCall::SipCall(pj::Account& account, int callId, ISipSignalingCallback* callback)
    : pj::Call(account, callId)
    , callback_(callback)
    , isOriginated_(false)
    , localSdpForAnswer_(nullptr)
    , localAudioPort_(4004)    // Default value, can be modified via SetLocalMediaPorts()
    , localVideoPort_(4002)     // Default value, can be modified via SetLocalMediaPorts()
{
    callInfo_.callId = callId;
    UpdateCallInfo();
}

SipCall::SipCall(pj::Account& account, ISipSignalingCallback* callback)
    : pj::Call(account, -1)
    , callback_(callback)
    , isOriginated_(true)
    , localSdpForAnswer_(nullptr)
    , localAudioPort_(4004)    // Default value, can be modified via SetLocalMediaPorts()
    , localVideoPort_(4002)     // Default value, can be modified via SetLocalMediaPorts()
{
    callInfo_.callId = getId();
}

SipCall::~SipCall() {
    if (incomingRdata_) {
        pjsip_rx_data_free_cloned(incomingRdata_);
        incomingRdata_ = nullptr;
    }

    // Ensure call is disconnected
    try {
        pj::CallInfo info = getInfo();
        if (info.state != PJSIP_INV_STATE_DISCONNECTED) {
            pj::CallOpParam param;
            param.statusCode = PJSIP_SC_OK;
            hangup(param);
        }
    } catch (const pj::Error&) {
        // Ignore errors
    }
}

bool SipCall::CloneIncomingRequest(pjsip_rx_data* src) {
    if (!src) return false;

    if (incomingRdata_) {
        pjsip_rx_data_free_cloned(incomingRdata_);
        incomingRdata_ = nullptr;
    }

    pjsip_rx_data* clone = nullptr;
    pj_status_t status = pjsip_rx_data_clone(src, 0, &clone);
    if (status != PJ_SUCCESS || !clone) {
        char errbuf[256];
        pj_strerror(status, errbuf, sizeof(errbuf));
        VCS_GATEWAY_LOG_ERROR("[ExternalSdp] pjsip_rx_data_clone failed: %s", errbuf);
        return false;
    }

    incomingRdata_ = clone;
    VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] INVITE request cloned for delayed answer");
    return true;
}

bool SipCall::MakeCall(const std::string& target, int audioCount, int videoCount) {
	try {
		pj::CallOpParam param;

		// pjsua 只承担信令。mediaDir=NONE 让它自己生成的 m-line 保持 inactive；
		// 线上 SDP 由 onCallSdpCreated() 用 "真实绑定端口" 重写，仍是
		// sendrecv（应用要收发）。
		// 出呼时 pjsua 不会应用这里的 mediaDir（我方是 offerer，answer 来自
		// 对端，apply_med_update 里那条分支被跳过），所以协议栈侧不发送 RTP
		// 由 onStreamCreated() 压住编码器 / 账户关闭视频自动发送来保证。
		// audioCount/videoCount 必须与 ExternalSdpBuilder 的 m-line 数一致
		// （固定 1 音频 + 1 视频），设为 0 会让 pjsua 以 488 拒绝建立媒体通道。
		param.opt.flag = PJSUA_CALL_SET_MEDIA_DIR;
		param.opt.audioCount = 1;
		param.opt.videoCount = 1;
		param.opt.mediaDir = { PJMEDIA_DIR_NONE, PJMEDIA_DIR_NONE };

		std::string targetUri = target;
		if (targetUri.find("sip:") == std::string::npos) targetUri = "sip:" + targetUri;

		VCS_GATEWAY_LOG_INFO("Making call to: %s (pjsua media dir=NONE, SDP in onCallSdpCreated)",
		                     targetUri.c_str());
		makeCall(targetUri, param);

		callInfo_.callId = getId();
		callInfo_.remoteUri = targetUri;
		UpdateCallInfo();
		return true;
	}
	catch (const pj::Error& err) {
		VCS_GATEWAY_LOG_ERROR("Failed to make call: %s", err.info().c_str());
		return false;
	}
}

bool SipCall::Answer(int code) {
    try {
        // ============================================================
        // [Ultimate Solution] Completely bypass PJSUA/PJSIP media subsystem
        //
        // Goal: Directly send raw SIP response containing external SDP
        // - Do not use pjsip_inv_answer (it triggers media negotiation crash)
        // - Use pjsip_endpt_create_response directly to create response
        // - Set SDP body directly into message
        //
        // Steps:
        // 1. Get call and invite session
        // 2. Generate external SDP
        // 3. Use pjsip_endpt_create_response to create 200 OK
        // 4. Set external SDP directly as message body
        // 5. Use pjsip_inv_send_msg to send
        // ============================================================

        pjsua_call_id callId = getId();
        if (callId < 0 || callId >= (int)pjsua_var.ua_cfg.max_calls) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] Answer: Invalid callId: %d", callId);
            return false;
        }

        // Use configured local IP and port
        std::string localIp = localIpAddress_;
        if (localIp.empty()) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] localIp is NOT configured!");
            return false;
        }

        // 通告端口取实际绑定值，配置端口只在查询失败时兜底。
        int boundAudioPort = 0;
        int boundVideoPort = 0;
        if (!GetLocalMediaPorts(boundAudioPort, boundVideoPort)) {
            boundAudioPort = static_cast<int>(localAudioPort_);
            boundVideoPort = static_cast<int>(localVideoPort_);
            VCS_GATEWAY_LOG_WARNING("[ExternalSdp] Call %d: media transports not bound yet, "
                                    "falling back to configured ports", callId);
        }

        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] ========== RAW SIP RESPONSE ==========");
        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] Call %d: Bypassing PJSUA media subsystem", callId);
        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] Local IP: %s, Audio: %d, Video: %d",
                            localIp.c_str(), boundAudioPort, boundVideoPort);

        // Get underlying pjsua_call and invite session
        pjsua_call* call = &pjsua_var.calls[callId];
        pjsip_inv_session* inv = call->inv;
        if (!inv) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] pjsip_inv_session is NULL!");
            return false;
        }

        // Original INVITE rx_data. Our own clone survives past the
        // on_incoming_call callback; pjsua's incoming_data only lives
        // during that callback and is a last-resort fallback (prompt answer).
        pjsip_rx_data* rdata = incomingRdata_ ? incomingRdata_ : call->incoming_data;
        if (!rdata) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] rdata is NULL (no cloned INVITE; "
                                  "answered outside the incoming-call callback?)");
            return false;
        }

        // Generate external SDP (use pjsua_var.pool to ensure memory is valid)
        pj::SdpSession answerSdp = ExternalSdpBuilder::buildOfferSdp(
            localIp, static_cast<uint16_t>(boundAudioPort),
            static_cast<uint16_t>(boundVideoPort));
        pjmedia_sdp_session* sdp = static_cast<pjmedia_sdp_session*>(answerSdp.pjSdpSession);

        if (!sdp) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] External SDP is NULL!");
            return false;
        }

        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] External SDP prepared: media_count=%u", sdp->media_count);
        for (unsigned i = 0; i < sdp->media_count; i++) {
            pjmedia_sdp_media* m = sdp->media[i];
            if (m) {
                char media_name[32];
                pj_str_t media_str = m->desc.media;
                snprintf(media_name, sizeof(media_name), "%.*s", (int)media_str.slen, media_str.ptr);
                VCS_GATEWAY_LOG_DEBUG("[ExternalSdp]   Media[%u]: %s, port=%u",
                                    i, media_name, m->desc.port);
            }
        }

        pj_status_t status;
        pjsip_tx_data* tdata = nullptr;

        // ============================================================
        // [Key] Create raw SIP response directly, skip INVITE session
        // Use pjsip_endpt_create_response instead of pjsip_inv_answer
        // ============================================================
        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] Creating raw 200 OK response...");

        // Get endpoint
        pjsip_endpoint* endpt = pjsua_var.endpt;
        if (!endpt) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] pjsip_endpoint is NULL!");
            return false;
        }

        // Create 200 OK response (using original rdata, not through inv)
        status = pjsip_endpt_create_response(endpt, rdata, 200, nullptr, &tdata);
        if (status != PJ_SUCCESS) {
            char errbuf[256];
            pj_strerror(status, errbuf, sizeof(errbuf));
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] pjsip_endpt_create_response failed: %s", errbuf);
            return false;
        }

        // ============================================================
        // [Core] Set SDP body directly into message
        // Use pjsip_create_sdp_body API to set SDP
        // ============================================================
        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] Setting external SDP body...");

        // Use pjsip_create_sdp_body to create message body from SDP structure
        status = pjsip_create_sdp_body(tdata->pool, sdp, &tdata->msg->body);
        if (status != PJ_SUCCESS || !tdata->msg->body) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] pjsip_create_sdp_body failed!");
            pjsip_tx_data_dec_ref(tdata);
            return false;
        }

        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] SDP body created successfully");

        // Contact must point at the socket this endpoint actually listens on.
        // Prefer the configured signaling port; fall back to the port of the
        // transport that received the INVITE (handles port=0 / auto-bind).
        uint16_t contactPort = localSignalingPort_;
        if (contactPort == 0 && rdata->tp_info.transport) {
            contactPort = static_cast<uint16_t>(rdata->tp_info.transport->local_name.port);
        }
        if (contactPort == 0) {
            contactPort = 5060;
        }

        char contact_uri[256];
        snprintf(contact_uri, sizeof(contact_uri), "<sip:%s:%u>", localIp.c_str(),
                 static_cast<unsigned>(contactPort));

        // pjsip_parse_uri() parses in place and keeps pointers into the
        // buffer, so the text must live in the tx-data pool, not on stack.
        const size_t contactLen = strlen(contact_uri);
        char* uriBuf = static_cast<char*>(pj_pool_alloc(tdata->pool, contactLen + 1));
        memcpy(uriBuf, contact_uri, contactLen + 1);

        pjsip_contact_hdr* contact = pjsip_contact_hdr_create(tdata->pool);
        contact->uri = pjsip_parse_uri(tdata->pool, uriBuf, contactLen, 0);
        if (!contact->uri) {
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] Failed to parse Contact URI '%s'", contact_uri);
            pjsip_tx_data_dec_ref(tdata);
            return false;
        }
        pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)contact);
        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] Contact: %s", contact_uri);

        // Content-Length is automatically calculated by PJSIP, no manual setting needed

        // ============================================================
        // Use inv->dlg to send response
        // This sends through dialog layer, maintaining dialog state
        // ============================================================
        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] Sending 200 OK via dialog...");

        status = pjsip_dlg_send_response(inv->dlg, inv->invite_tsx, tdata);
        if (status != PJ_SUCCESS) {
            char errbuf[256];
            pj_strerror(status, errbuf, sizeof(errbuf));
            VCS_GATEWAY_LOG_ERROR("[ExternalSdp] pjsip_dlg_send_response failed: %s", errbuf);
            return false;
        }

        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] ========== RAW SIP SUCCESS ==========");
        VCS_GATEWAY_LOG_DEBUG("[ExternalSdp] 200 OK sent with external SDP (bypassed all PJSUA media)");
        return true;

    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("[ExternalSdp] Failed to answer call: %s", err.info().c_str());
        return false;
    }
}

bool SipCall::Reject(int code) {
    try {
        pj::CallOpParam param;
        param.statusCode = static_cast<pjsip_status_code>(code);
        hangup(param);

        VCS_GATEWAY_LOG_INFO("Rejecting call %d with code %d", callInfo_.callId, code);
        return true;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to reject call: %s", err.info().c_str());
        return false;
    }
}

bool SipCall::Hangup(int code) {
    try {
        pj::CallOpParam param;
        param.statusCode = static_cast<pjsip_status_code>(code);
        hangup(param);

        VCS_GATEWAY_LOG_INFO("Hanging up call %d", callInfo_.callId);
        return true;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to hangup call: %s", err.info().c_str());
        return false;
    }
}

bool SipCall::Hold() {
    try {
        pj::CallOpParam param;
        param.opt.flag = PJSUA_CALL_UNHOLD;  // Clear UNHOLD flag to hold
        setHold(param);

        VCS_GATEWAY_LOG_INFO("Holding call %d", callInfo_.callId);
        return true;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to hold call: %s", err.info().c_str());
        return false;
    }
}

bool SipCall::Unhold() {
    try {
        pj::CallOpParam param;
        param.opt.flag = PJSUA_CALL_UNHOLD;
        reinvite(param);

        VCS_GATEWAY_LOG_INFO("Unholding call %d", callInfo_.callId);
        return true;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to unhold call: %s", err.info().c_str());
        return false;
    }
}

bool SipCall::SendDTMF(const std::string& digits) {
    try {
        // Use INFO method to send DTMF
        pj::CallSendDtmfParam param;
        param.digits = digits;
        sendDtmf(param);

        VCS_GATEWAY_LOG_INFO("Sent DTMF: %s", digits.c_str());
        return true;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to send DTMF: %s", err.info().c_str());
        return false;
    }
}

bool SipCall::RequestKeyFrame() {
    VCS_GATEWAY_LOG_INFO("Requesting key frame for call %d", callInfo_.callId);

// PJSIP always enabled
    try {
        pjsua_call_id callId = getId();

        if (callId < 0 || callId >= (int)pjsua_var.ua_cfg.max_calls) {
            VCS_GATEWAY_LOG_ERROR("RequestKeyFrame: Invalid callId: %d", callId);
            return false;
        }

        pjsua_call* call = &pjsua_var.calls[callId];

        // Traverse all media, find video stream and send PLI request
        for (unsigned i = 0; i < call->med_cnt; i++) {
            pjsua_call_media* call_media = &call->media[i];

            if (call_media->type == PJMEDIA_TYPE_VIDEO &&
                call_media->strm.v.stream != nullptr) {
                VCS_GATEWAY_LOG_INFO("Sending RTCP PLI to request keyframe for video stream %u", i);

                pj_status_t status = pjmedia_vid_stream_send_rtcp_pli(call_media->strm.v.stream);
                if (status == PJ_SUCCESS) {
                    VCS_GATEWAY_LOG_INFO("RTCP PLI sent successfully for media index %u", i);
                    return true;
                } else {
                    VCS_GATEWAY_LOG_ERROR("Failed to send RTCP PLI: status=%d", status);
                }
            }
        }

        VCS_GATEWAY_LOG_WARNING("No video stream found for call %d", callId);
        return false;

    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("RequestKeyFrame failed: %s", err.info().c_str());
        return false;
    }
}

pjmedia_transport* SipCall::GetPjMediaTransport(unsigned mediaIndex) {
    try {
        pjsua_call_id callId = getId();

        // Validate callId validity
        if (callId < 0 || callId >= (int)pjsua_var.ua_cfg.max_calls) {
            VCS_GATEWAY_LOG_ERROR("Invalid callId: %d", callId);
            return nullptr;
        }

        // Directly access pjsua internal data to get transport
        pjsua_call* call = &pjsua_var.calls[callId];

        VCS_GATEWAY_LOG_INFO("GetPjMediaTransport: callId=%d, mediaIndex=%u, med_cnt=%u",
                           callId, mediaIndex, call->med_cnt);

        if (mediaIndex >= call->med_cnt) {
            VCS_GATEWAY_LOG_ERROR("Invalid mediaIndex: %u, max: %u", mediaIndex, call->med_cnt);
            return nullptr;
        }

        // [Key Fix] Check all media transport status
        for (unsigned i = 0; i < call->med_cnt; i++) {
            pjsua_call_media* m = &call->media[i];
            VCS_GATEWAY_LOG_INFO("  Media[%u]: status=%d, type=%d, dir=%d, tp=%p",
                               i, m->state, m->type, m->dir, (void*)m->tp);
        }

        // Get specified media transport
        pjsua_call_media* call_media = &call->media[mediaIndex];

        // [Key] Return this media transport directly
        // In RTP passthrough mode, SRTP transport selects correct crypto context based on payload type
        pjmedia_transport* tp = call_media->tp;

        if (!tp) {
            // No cross-stream fallback: each SipMediaTransport binds exactly one
            // pjmedia transport, and that transport carries a single external
            // callback slot. Handing media[0].tp to the video wrapper (or vice
            // versa) silently rebinds the slot and re-attaches the same
            // transport behind the owning stream's back.
            VCS_GATEWAY_LOG_ERROR(
                "GetPjMediaTransport: media[%u] has no transport "
                "(state=%d, type=%d, dir=%d); refusing cross-stream fallback",
                mediaIndex, call_media->state, call_media->type, call_media->dir);
            return nullptr;
        }

        VCS_GATEWAY_LOG_INFO("  GetPjMediaTransport: returning tp=%p for mediaIndex=%u",
                           (void*)tp, mediaIndex);
        return tp;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to get PJSIP media transport: %s", err.info().c_str());
        return nullptr;
    }
}

bool SipCall::SendRtpPacket(unsigned mediaIndex, const void* rtpData, size_t size) {
    try {
        // Get media transport info
        pj::MediaTransportInfo transportInfo = getMedTransportInfo(mediaIndex);

        // Directly send RTP packet, bypassing conference
        // Note: This requires raw pointer of pjmedia_transport
        // Here we just log first, actual sending needs to go through pjmedia API
        VCS_GATEWAY_LOG_INFO("SendRtpPacket: callId=%d, mediaIndex=%u, size=%zu",
                            callInfo_.callId, mediaIndex, size);
        return true;
    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to send RTP packet: %s", err.info().c_str());
        return false;
    }
}

pjsip_inv_session* SipCall::GetInvSession() const {
    try {
        pjsua_call_id callId = getId();
        if (callId < 0 || callId >= (int)pjsua_var.ua_cfg.max_calls) {
            return nullptr;
        }
        pjsua_call* pjCall = &pjsua_var.calls[callId];
        return pjCall ? pjCall->inv : nullptr;
    } catch (...) {
        return nullptr;
    }
}

void SipCall::onCallState(pj::OnCallStateParam& /*prm*/) {
	VCS_GATEWAY_LOG_DEBUG("[SipCall] onCallState ENTER this=%p, callId=%d", this, callInfo_.callId);
	UpdateCallInfo();

	VCS_GATEWAY_LOG_DEBUG("[DIAG-1] onCallState ENTER: callId=%d, state=%u, stateText=%s, isOriginated=%d",
		callInfo_.callId, callInfo_.state, callInfo_.stateText.c_str(),
		(int)isOriginated_);

	VCS_GATEWAY_LOG_INFO("Call %d state changed: %s",
		callInfo_.callId, callInfo_.stateText.c_str());

	// ============================================================
	// 1. [原有逻辑保留] 解析原始 INVITE SDP (仅限被叫方在 EARLY 状态)
	// ============================================================
	if (callInfo_.state == PJSIP_INV_STATE_EARLY && !isOriginated_) {
		VCS_GATEWAY_LOG_INFO("[SDP-PARSE] Incoming call in EARLY state, parsing INVITE SDP...");

		try {
			pjsua_call_id callId = getId();
			pjsua_call* call = &pjsua_var.calls[callId];

			if (call && call->inv && call->inv->invite_req) {
				pjsip_tx_data* tdata = call->inv->invite_req;
				if (tdata->msg && tdata->msg->body) {
					char* sdpData = static_cast<char*>(tdata->msg->body->data);
					pj_ssize_t sdpLen = tdata->msg->body->len;

					if (sdpData && sdpLen > 0) {
						std::string sdpText(sdpData, sdpLen);
						VCS_GATEWAY_LOG_INFO("[SDP-PARSE] Remote SDP length: %d", (int)sdpLen);
						ParseAndStoreRemotePortsFromSdp(sdpText);
					}
					else {
						VCS_GATEWAY_LOG_WARNING("[SDP-PARSE] No SDP data in body");
					}
				}
				else {
					VCS_GATEWAY_LOG_WARNING("[SDP-PARSE] No body in INVITE request");
				}
			}
			else {
				VCS_GATEWAY_LOG_WARNING("[SDP-PARSE] Call or invite_req is NULL");
			}
		}
		catch (const pj::Error& err) {
			VCS_GATEWAY_LOG_ERROR("[SDP-PARSE] Failed to parse INVITE SDP: %s", err.info().c_str());
		}
	}

	// ============================================================
	// 2. [新增逻辑] 处理 CONNECTING (收到 200 OK)，手动发送 ACK
	// ============================================================
	if (callInfo_.state == PJSIP_INV_STATE_CONNECTING) {
		VCS_GATEWAY_LOG_INFO("[SipCall] Received 200 OK, manually sending ACK...");
		VCS_GATEWAY_LOG_DEBUG("[DIAG-9] CONNECTING: manual ACK code entering");

		pjsua_call_id callId = getId();
		pjsua_call* call = &pjsua_var.calls[callId];

		VCS_GATEWAY_LOG_DEBUG("[DIAG-9] callId=%d, call=%p, call->inv=%p, call->inv->invite_tsx=%p",
			callId, (void*)call, call ? (void*)call->inv : nullptr,
			(call && call->inv) ? (void*)call->inv->invite_tsx : nullptr);

		if (call && call->inv) {
			pjsip_tx_data* tdata = nullptr;
			int ackCseq = 0;
			if (call->inv->invite_tsx) {
				ackCseq = call->inv->invite_tsx->cseq;
			}

			VCS_GATEWAY_LOG_DEBUG("[DIAG-9] ackCseq=%d", ackCseq);

			// PJSIP 2.17 API: pjsip_inv_create_ack(inv, cseq, &tdata)
			pj_status_t status = pjsip_inv_create_ack(call->inv, ackCseq, &tdata);

			VCS_GATEWAY_LOG_DEBUG("[DIAG-9] pjsip_inv_create_ack returned status=%d, tdata=%p",
				status, (void*)tdata);

			if (status == PJ_SUCCESS && tdata) {
				pjsip_inv_send_msg(call->inv, tdata);
				VCS_GATEWAY_LOG_INFO("[SipCall] ACK sent successfully.");
				VCS_GATEWAY_LOG_DEBUG("[DIAG-9] ACK sent successfully");
			}
			else {
				VCS_GATEWAY_LOG_ERROR("[SipCall] Failed to create/send ACK, status=%d", status);
				VCS_GATEWAY_LOG_ERROR("[DIAG-9] Failed to create/send ACK, status=%d", status);
			}
		}
		else {
			VCS_GATEWAY_LOG_ERROR("[DIAG-9] call or call->inv is NULL, ACK NOT sent");
		}
	}

	// ============================================================
	// 3. [回调通知] 通知上层状态变化
	// ============================================================
	if (callback_) {
		//callback_->OnCallState(callInfo_.callId, callInfo_.state, callInfo_.stateText);

		VCS_GATEWAY_LOG_DEBUG("[DIAG-2] Before callback->OnCallState: callId=%d, state=%u",
			callInfo_.callId, callInfo_.state);
		callback_->OnCallState(callInfo_.callId, callInfo_.state, callInfo_.stateText);
		VCS_GATEWAY_LOG_DEBUG("[DIAG-2] After callback->OnCallState returned: callId=%d, state=%u",
			callInfo_.callId, callInfo_.state);

		if (callInfo_.state == PJSIP_INV_STATE_DISCONNECTED) {
			std::ostringstream oss;
			oss << "Call disconnected (code: " << callInfo_.state << ")";
			callback_->OnCallDisconnected(callInfo_.callId, callInfo_.state, oss.str());
		}
	}
	VCS_GATEWAY_LOG_DEBUG("[SipCall] onCallState EXIT this=%p, callId=%d", this, callInfo_.callId);
}


void SipCall::onCallMediaState(pj::OnCallMediaStateParam& /*prm*/) {
	// pjsua 媒体平面只用于维持 SIP 层 SDP 协商，真正的媒体由外部 RTPEngine
	// 承载（SDP 通告的 4004/4006）：协议栈侧不产生 RTP，RX 由应用挂在
	// pjmedia transport 上的外部回调负责。
	VCS_GATEWAY_LOG_INFO("onCallMediaState: callId=%d, external media plane (pjsua TX suppressed)",
	                     callInfo_.callId);

	if (!externalMediaPlane_) {
		return;
	}

	// 每次媒体更新（re-INVITE、hold/unhold）后重新压住编码器：新建的流会再
	// 触发 onStreamCreated，这里覆盖只动已有流的路径。
	if (audioStream_) {
		pjmedia_stream_pause(audioStream_, PJMEDIA_DIR_ENCODING);
	}

	// 视频没有 pjsua2 的 per-stream 回调。pjsua 在 start 后先暂停编码，
	// 只有 videoConfig.autoTransmitOutgoing 打开时才会定时恢复；账户侧已关闭。
	// 这里再压一次，保证 hold/unhold 不会把协议栈侧视频 TX 带回来。
	// 幂等；无活动视频流时返回 PJ_ENOTFOUND。
	pj_status_t vidStatus = pjsua_call_set_vid_strm(
		static_cast<pjsua_call_id>(getId()),
		PJSUA_CALL_VID_STRM_STOP_TRANSMIT, nullptr);
	VCS_GATEWAY_LOG_DEBUG(
		"onCallMediaState: callId=%d, video TX stop re-asserted (status=%d)",
		getId(), static_cast<int>(vidStatus));
}

void SipCall::onStreamCreated(pj::OnStreamCreatedParam& prm) {
	if (!externalMediaPlane_) {
		return;
	}

	// 这是能用的最早公开钩子：pjsua 在回调返回后才把该流挂进会议桥
	// （pjmedia_conf_add_port），所以此刻还没人给编码器喂帧，压住编码器
	// 就保证协议栈一包 RTP 都不会发。onStreamPreCreate 不允许改方向，
	// 改本端 SDP 方向又会连带丢掉自己的 RX。
	audioStream_ = static_cast<pjmedia_stream*>(prm.stream);
	if (!audioStream_) {
		VCS_GATEWAY_LOG_WARNING(
			"onStreamCreated: callId=%d streamIdx=%u has no pjmedia stream; "
			"cannot pause the stack's audio encoder", getId(), prm.streamIdx);
		return;
	}

	pj_status_t status = pjmedia_stream_pause(audioStream_, PJMEDIA_DIR_ENCODING);
	if (status == PJ_SUCCESS) {
		VCS_GATEWAY_LOG_INFO(
			"onStreamCreated: callId=%d streamIdx=%u, pjmedia audio encoder paused "
			"(external media plane, stack does not TX)", getId(), prm.streamIdx);
	} else {
		VCS_GATEWAY_LOG_ERROR(
			"onStreamCreated: callId=%d streamIdx=%u, pjmedia_stream_pause failed: %d",
			getId(), prm.streamIdx, static_cast<int>(status));
	}
}

void SipCall::onCallMediaTransportState(pj::OnCallMediaTransportStateParam& prm) {
    VCS_GATEWAY_LOG_DEBUG("Media transport state: %d", prm.state);
}

void SipCall::onCallRxOffer(pj::OnCallRxOfferParam& prm) {
	VCS_GATEWAY_LOG_INFO("onCallRxOffer: callId=%d, manually crafting answer SDP to prevent crash", callInfo_.callId);
	
}

void SipCall::UpdateCallInfo() {
    try {
        pj::CallInfo info = getInfo();

        callInfo_.callId = info.id;
        callInfo_.state = info.state;
        callInfo_.stateText = GetStateText(info.state);
        callInfo_.localUri = info.localUri;
        callInfo_.remoteUri = info.remoteUri;

        // PJSIP 2.x uses totalDuration to calculate connection time
        callInfo_.connectTime = 0;
        callInfo_.duration = info.totalDuration.sec * 1000 + info.totalDuration.msec;

    } catch (const pj::Error& err) {
        VCS_GATEWAY_LOG_ERROR("Failed to update call info: %s", err.info().c_str());
    }
}

void SipCall::GetAllMediaAddresses(std::string& audioAddr, int& audioPort,
	std::string& videoAddr, int& videoPort) const {
	audioAddr.clear(); audioPort = 0;
	videoAddr.clear(); videoPort = 0;

	pjsua_call_id callId = getId();
	if (callId < 0 || callId >= (int)pjsua_var.ua_cfg.max_calls) return;
	pjsua_call* call = &pjsua_var.calls[callId];
	if (!call || !call->inv || !call->inv->neg) return;

	// 直接拿 SDP 协商后的远端 SDP
	const pjmedia_sdp_session* remoteSdp = nullptr;
	if (pjmedia_sdp_neg_get_active_remote(call->inv->neg, &remoteSdp) == PJ_SUCCESS && remoteSdp) {
		if (remoteSdp->conn) {
			char ipBuf[64];
			snprintf(ipBuf, sizeof(ipBuf), "%.*s", (int)remoteSdp->conn->addr.slen, remoteSdp->conn->addr.ptr);
			audioAddr = ipBuf;
			videoAddr = ipBuf;
		}

		for (unsigned i = 0; i < remoteSdp->media_count; ++i) {
			pjmedia_sdp_media* m = remoteSdp->media[i];
			if (pj_strcmp2(&m->desc.media, "audio") == 0) {
				audioPort = m->desc.port;
			}
			else if (pj_strcmp2(&m->desc.media, "video") == 0) {
				videoPort = m->desc.port;
			}
		}
	}
}

bool SipCall::GetLocalMediaPorts(int& audioPort, int& videoPort) const {
	audioPort = 0;
	videoPort = 0;

	pjsua_call_id callId = getId();
	if (callId < 0 || callId >= (int)pjsua_var.ua_cfg.max_calls) return false;

	const pjsua_call* call = &pjsua_var.calls[callId];

	// Offer 阶段媒体还在 media_prov；会话确认后才会搬到 media。
	const pjsua_call_media* meds = nullptr;
	unsigned count = 0;
	if (call->med_prov_cnt > 0) {
		meds = call->media_prov;
		count = call->med_prov_cnt;
	}
	else if (call->med_cnt > 0) {
		meds = call->media;
		count = call->med_cnt;
	}

	for (unsigned i = 0; i < count; ++i) {
		const pjsua_call_media& m = meds[i];
		// SRTP 包装层的 get_info 不向下透传，端口要从底层 UDP transport 取。
		pjmedia_transport* tp = m.tp_orig ? m.tp_orig : m.tp;
		if (!tp) continue;

		pjmedia_transport_info info;
		pjmedia_transport_info_init(&info);
		if (pjmedia_transport_get_info(tp, &info) != PJ_SUCCESS) continue;
		if (info.sock_info.rtp_sock == PJ_INVALID_SOCKET) continue;

		const int port = static_cast<int>(pj_sockaddr_get_port(&info.sock_info.rtp_addr_name));
		if (port <= 0) continue;

		if (m.type == PJMEDIA_TYPE_AUDIO && audioPort == 0) {
			audioPort = port;
		}
		else if (m.type == PJMEDIA_TYPE_VIDEO && videoPort == 0) {
			videoPort = port;
		}
	}

	VCS_GATEWAY_LOG_INFO("[SipCall] Local bound media ports: callId=%d, audio=%d, video=%d",
	                     callId, audioPort, videoPort);
	return audioPort > 0 || videoPort > 0;
}

bool SipCall::GetRemoteCodecs(vcs::NegotiatedCodec& audioCodec,
	vcs::NegotiatedCodec& videoCodec) const {
	pjsua_call_id callId = getId();
	if (callId < 0 || callId >= (int)pjsua_var.ua_cfg.max_calls) return false;
	pjsua_call* call = &pjsua_var.calls[callId];
	if (!call || !call->inv || !call->inv->neg) return false;

	const pjmedia_sdp_session* remoteSdp = nullptr;
	if (pjmedia_sdp_neg_get_active_remote(call->inv->neg, &remoteSdp) != PJ_SUCCESS ||
		!remoteSdp) {
		return false;
	}

	return sdp::ParseNegotiatedCodecs(remoteSdp, audioCodec, videoCodec);
}

bool SipCall::GetSecondaryVideoAddress(std::string& secondaryVideoAddr,
                                       int& secondaryVideoPort,
                                       uint8_t& secondaryVideoPayloadType) const {
    secondaryVideoAddr.clear();
    secondaryVideoPort = 0;
    secondaryVideoPayloadType = 0;

    // ============================================================
    // [V7.0 Phase-2 E] Secondary video stream extraction
    //
    // The secondary video stream is identified by SDP `a=content:slides`
    // or `a=content:sub` attribute on the m=video line.
    //
    // Walk all media in `mediaInfoList_`, find the first video media
    // whose `contentType` matches the secondary-stream marker, and
    // return its remote address/port/PT.
    // ============================================================
    for (const auto& media : mediaInfoList_) {
        if (!media.status) continue;
        if (media.type != "video") continue;

        const bool isSecondary =
            (media.contentType == "slides" || media.contentType == "sub");
        if (!isSecondary) continue;

        secondaryVideoAddr = media.remoteAddress;
        secondaryVideoPort = media.remotePort;
        secondaryVideoPayloadType = media.payloadType;
        VCS_GATEWAY_LOG_INFO("[V7.0] Secondary video: %s:%d (pt=%u, content=%s)",
                             secondaryVideoAddr.c_str(), secondaryVideoPort,
                             secondaryVideoPayloadType, media.contentType.c_str());
        return true;
    }

    VCS_GATEWAY_LOG_INFO("[V7.0] No secondary video stream present in this call");
    return false;
}

bool SipCall::GetMainVideoAddress(std::string& mainVideoAddr,
                                  int& mainVideoPort,
                                  uint8_t& mainVideoPayloadType) const {
    mainVideoAddr.clear();
    mainVideoPort = 0;
    mainVideoPayloadType = 0;

    // ============================================================
    // [V7.0 Phase-2 E] Main video stream extraction
    //
    // The main video stream is identified by either:
    //  1. an explicit `a=content:main` attribute, OR
    //  2. the first video stream that does NOT have a "slides"/"sub"
    //     contentType (covers legacy terminals that don't set content).
    //
    // If the call carries only one video stream it is considered main.
    // ============================================================
    for (const auto& media : mediaInfoList_) {
        if (!media.status) continue;
        if (media.type != "video") continue;

        const bool isMain =
            (media.contentType == "main") ||
            (media.contentType.empty()) ||
            (media.contentType != "slides" && media.contentType != "sub");
        if (!isMain) continue;

        mainVideoAddr = media.remoteAddress;
        mainVideoPort = media.remotePort;
        mainVideoPayloadType = media.payloadType;
        VCS_GATEWAY_LOG_INFO("[V7.0] Main video: %s:%d (pt=%u, content=%s)",
                             mainVideoAddr.c_str(), mainVideoPort,
                             mainVideoPayloadType,
                             media.contentType.empty() ? "(none)" : media.contentType.c_str());
        return true;
    }

    VCS_GATEWAY_LOG_WARNING("[V7.0] No main video stream found in this call");
    return false;
}

void SipCall::ParseAndStoreRemotePortsFromSdp(const std::string& sdpText) {
    VCS_GATEWAY_LOG_INFO("[SDP-PARSE] Parsing remote ports from INVITE SDP");

    if (sdpText.empty()) {
        VCS_GATEWAY_LOG_WARNING("[SDP-PARSE] SDP text is empty");
        return;
    }

    // Parse m= line in SDP, format:
    //   m=<media> <port> <proto> <fmt> ...
    // Example: m=audio 50074 RTP/AVP 103 107 121 122 123 124 9 0 8 101
    //       m=video 50076 RTP/AVP 97 98 99 34 117

    std::istringstream iss(sdpText);
    std::string line;
    int lineNum = 0;

    while (std::getline(iss, line)) {
        lineNum++;

        // Remove trailing \r\n or \n
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Check if it is m= line
        if (line.rfind("m=", 0) == 0) {
            // Parse m=audio port ... or m=video port ...
            std::istringstream mLine(line.substr(2));  // Skip "m="
            std::string mediaType;
            int port = 0;

            mLine >> mediaType >> port;

            if (mediaType == "audio" && port > 0) {
                remoteAudioPortFromInvite_ = port;
                VCS_GATEWAY_LOG_INFO("[SDP-PARSE] Found audio port: %d", port);
            } else if (mediaType == "video" && port > 0) {
                remoteVideoPortFromInvite_ = port;
                VCS_GATEWAY_LOG_INFO("[SDP-PARSE] Found video port: %d", port);
            }
        }

        // Parse c= line to get remote IP address
        // Format: c=IN IP4 addr
        if (line.rfind("c=", 0) == 0 && remoteAddressFromInvite_.empty()) {
            std::istringstream cLine(line.substr(2));  // Skip "c="
            std::string networkType, addrType, addr;

            cLine >> networkType >> addrType >> addr;

            if (networkType == "IN" && addrType == "IP4" && !addr.empty()) {
                remoteAddressFromInvite_ = addr;
                VCS_GATEWAY_LOG_INFO("[SDP-PARSE] Found remote address: %s", addr.c_str());
            }
        }
    }

    VCS_GATEWAY_LOG_INFO("[SDP-PARSE] Result: addr=%s, audio=%d, video=%d",
                        remoteAddressFromInvite_.c_str(),
                        remoteAudioPortFromInvite_,
                        remoteVideoPortFromInvite_);
}

std::string SipCall::GetStateText(uint32_t state) {
    switch (state) {
        case PJSIP_INV_STATE_NULL:       return "Null";
        case PJSIP_INV_STATE_CALLING:    return "Calling";
        case PJSIP_INV_STATE_EARLY:      return "Early";
        case PJSIP_INV_STATE_CONNECTING: return "Connecting";
        case PJSIP_INV_STATE_CONFIRMED:  return "Confirmed";
        case PJSIP_INV_STATE_DISCONNECTED: return "Disconnected";
        default: return "Unknown";
    }
}


void SipCall::onCallSdpCreated(pj::OnCallSdpCreatedParam& prm) {
	// remSdp 非空说明这是应答，由 pjsua 自己的协商决定，不在这里改。
	if (!prm.remSdp.wholeSdp.empty()) {
		return;
	}

	const std::string localIp =
		localIpAddress_.empty() ? "127.0.0.1" : localIpAddress_;

	// 通告的端口必须是 pjmedia 实际绑定的端口：配置里的 4004/4006 只是
	// 首通呼叫的起点（见账户 rtp_cfg.port），之后 pjmedia 会顺延到
	// 4008/4010……，继续写死配置值会让对端把 RTP 发到没人监听的端口
	// （只有对称 RTP 会掩盖这个问题）。
	int audioPort = 0;
	int videoPort = 0;
	if (!GetLocalMediaPorts(audioPort, videoPort)) {
		audioPort = static_cast<int>(localAudioPort_);
		videoPort = static_cast<int>(localVideoPort_);
		VCS_GATEWAY_LOG_WARNING(
			"[SipCall] onCallSdpCreated: media transports not bound yet, "
			"falling back to configured ports audio=%d, video=%d (callId=%d)",
			audioPort, videoPort, callInfo_.callId);
	}
	if (audioPort <= 0 || videoPort <= 0) {
		VCS_GATEWAY_LOG_ERROR(
			"[SipCall] onCallSdpCreated: no usable local media ports "
			"(audio=%d, video=%d, callId=%d); leaving pjsua SDP untouched",
			audioPort, videoPort, callInfo_.callId);
		return;
	}

	pj::SdpSession offer = ExternalSdpBuilder::buildOfferSdp(
		localIp, static_cast<uint16_t>(audioPort), static_cast<uint16_t>(videoPort));
	if (offer.wholeSdp.empty()) {
		VCS_GATEWAY_LOG_ERROR("[SipCall] onCallSdpCreated: ExternalSdpBuilder returned empty SDP");
		return;
	}

	prm.sdp.wholeSdp = offer.wholeSdp;
	VCS_GATEWAY_LOG_INFO("[SipCall] onCallSdpCreated: offer rewritten with bound ports "
	                     "audio=%d, video=%d (callId=%d)",
	                     audioPort, videoPort, callInfo_.callId);
}

}} // namespace vcs::gateway
