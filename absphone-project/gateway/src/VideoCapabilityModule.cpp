/**
 * @file VideoCapabilityModule.cpp
 * @brief SIP Video Capability Declaration Module Implementation
 *
 * Intercepts SDP messages via a pjsip module to ensure the video port is not 0.
 * Supports multiple video codecs: H264, H265, VP8, VP9, etc.
 *
 * Core principles:
 * 1. Register a pjsip module that intercepts messages before sending
 * 2. Check whether the SIP response contains an SDP body
 * 3. If m=video 0 is present, change the port to a non-zero value
 * 4. If no video media line is present, add a complete video media line
 * 5. Add the required video codec attributes (rtpmap, fmtp)
 *
 * Actual video encoding/decoding is handled by the FFmpeg in the Media module.
 * This module simply "tricks" the PJSIP negotiator into believing we support video.
 */

#include "VideoCapabilityModule.h"
#include "ILogger.h"
#include <pjsip.h>
#include <pjlib.h>
#include <pjsip/sip_module.h>
#include <pjsua-lib/pjsua.h>
#include <pjsua-lib/pjsua_internal.h>
#include <cstring>
#include <cstdio>

#define VIDEO_MODULE_LOG    "[VideoCap] "

// ============================================================================
// Configuration Variables
// ============================================================================

// Video port configuration (default 4002)
static int g_video_port = 4002;

// Video codec to declare
static const char* g_video_codec_name = "H264";
static const char* g_video_payload_type = "97";
static const char* g_video_clock_rate = "90000";
static const char* g_video_fmtp = "profile-level-id=42e01f;packetization-mode=1";

// Module instance
static pjsip_module* g_video_module = NULL;

// ============================================================================
// Module Definition
// ============================================================================

/**
 * @brief Module name
 */
static const char* VIDEO_MODULE_NAME = "video-capability";

/**
 * @brief Fix the video port in SDP
 *
 * @param sdp_text SDP text
 * @param sdp_len SDP length
 * @param port New port number
 * @return Number of modifications (0=no change, 1=modified existing line, 2=added new line)
 */
static int fix_or_add_video_in_sdp(char* sdp_text, int sdp_len, int port) {
    // Check whether m=video already exists
    char* m_video = strstr(sdp_text, "m=video");

    if (m_video) {
        // Video line already exists, check whether the port is 0
        char* port_start = m_video + 8;
        while (*port_start == ' ') port_start++;

        // Check whether the port is 0
        if (*port_start == '0' && (*(port_start+1) == '\r' || *(port_start+1) == '\n' ||
                                   *(port_start+1) == ' ' || *(port_start+1) == '\0')) {
            // Found video port 0, needs modification
            VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Found m=video 0, fixing port to %d", port);

            // Replace the port number
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%d", port);

            char* end = port_start;
            while (*end >= '0' && *end <= '9') end++;
            size_t old_len = end - port_start;
            size_t new_len = strlen(port_str);

            // If the lengths differ, the string must be moved
            if (old_len < new_len) {
                memmove(port_start + new_len, end, strlen(end) + 1);
            } else if (old_len > new_len) {
                memcpy(port_start, port_str, new_len);
                memmove(port_start + new_len, end, strlen(end) + 1);
                for (size_t i = new_len; i < old_len; i++) {
                    port_start[i] = ' ';
                }
            } else {
                memcpy(port_start, port_str, new_len);
            }
            return 1;  // Modified successfully
        } else {
            // Port is already non-zero
            VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Video port already non-zero: %c", *port_start);
            return 0;
        }
    }

    // No video line present, need to add one
    VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "No video media line found, adding one with port %d", port);

    // Build the video media line
    char video_line[512];
    snprintf(video_line, sizeof(video_line),
             "m=video %d RTP/AVP %s\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "b=AS:4096\r\n"
             "a=rtpmap:%s %s/%s\r\n"
             "a=fmtp:%s %s\r\n"
             "a=sendrecv\r\n",
             port,
             g_video_payload_type,
             g_video_payload_type, g_video_codec_name, g_video_clock_rate,
             g_video_payload_type, g_video_fmtp);

    // Find the last m= line (before any a= lines)
    char* last_m_line = NULL;
    char* p = sdp_text;
    while ((p = strstr(p, "\nm=")) != NULL) {
        last_m_line = p;
        p++;
    }

    // Find the insertion point (first a= line after last m=)
    char* insert_pos = NULL;
    if (last_m_line) {
        // Find the first a= line after the last m= line
        insert_pos = last_m_line + 1;
        while (*insert_pos && *insert_pos != '\n') insert_pos++;
        if (*insert_pos == '\n') insert_pos++;
    } else {
        // No m= line found, insert before the first a= line
        insert_pos = strstr(sdp_text, "\na=");
        if (!insert_pos) {
            insert_pos = strstr(sdp_text, "a=");
        }
    }

    if (insert_pos) {
        // Move remaining content, insert the video line
        size_t video_len = strlen(video_line);
        size_t remaining_len = strlen(insert_pos);
        memmove(insert_pos + video_len, insert_pos, remaining_len + 1);
        memcpy(insert_pos, video_line, video_len);
        VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Video media line added successfully");
        return 2;  // Added successfully
    } else {
        VCS_GATEWAY_LOG_ERROR(VIDEO_MODULE_LOG "Failed to find insertion point for video line");
        return -1;
    }
}

/**
 * @brief Callback that intercepts outgoing messages
 *
 * This function is invoked before a message is sent.
 * We use it to modify the video port in SDP or add a video line.
 */
static pj_status_t video_module_on_tx_message(pjsip_tx_data* tdata) {
    // Check whether the message is a response
    if (!tdata || !tdata->msg || tdata->msg->type != PJSIP_RESPONSE_MSG) {
        return PJ_SUCCESS;
    }

    // Check whether there is an SDP body
    pjsip_msg_body* body = tdata->msg->body;
    if (!body || !body->data) {
        return PJ_SUCCESS;
    }

    // Check whether this is a 200 OK response
    // (we check first_line.type to determine whether this is a response)
    if (tdata->msg->type == PJSIP_RESPONSE_MSG) {
        pjsip_status_line* status = &tdata->msg->line.status;
        if (status->code != 200) {
            return PJ_SUCCESS;  // Not a 200 response, skip
        }
    }

    // Modify the SDP text directly
    char* sdp_text = (char*)body->data;

    // Fix or add the video line
    int result = fix_or_add_video_in_sdp(sdp_text, body->len, g_video_port);

    if (result == 1) {
        VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Fixed video port in existing m=video line");
    } else if (result == 2) {
        VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Added complete video media line");
    } else if (result == 0) {
        VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Video already configured correctly");
    } else {
        VCS_GATEWAY_LOG_ERROR(VIDEO_MODULE_LOG "Failed to fix/add video media line");
    }

    return PJ_SUCCESS;
}

/**
 * @brief Module priority (after inv negotiator)
 *
 * Priority notes:
 * - PJSIP_MOD_PRIORITY_TRANSPORT_LAYER = 8
 * - PJSIP_MOD_PRIORITY_APPLICATION = PJSIP_MOD_PRIORITY_TRANSPORT_LAYER + 1
 * - We use PJSIP_MOD_PRIORITY_APPLICATION + 1
 *
 * [Key] Must run after the inv negotiator, otherwise the SDP will be overwritten by it!
 *
 * Processing order (high to low):
 * 1. INV session processing (generates final SDP)
 * 2. Our module (modifies m=video 0 to non-zero, or adds video line)
 * 3. Transport layer (sends)
 */
static pjsip_module video_capability_module = {
    NULL, NULL,                     // prev, next
    NULL,                           // name (will be set on registration)
    -1,                             // id (will be assigned on registration)
    PJSIP_MOD_PRIORITY_APPLICATION + 1,  // priority (after inv)
    NULL,                           // load
    NULL,                           // start
    NULL,                           // stop
    NULL,                           // unload
    NULL,                           // on_rx_request
    NULL,                           // on_rx_response
    NULL,                           // on_tx_request
    NULL,                           // on_tx_response
    &video_module_on_tx_message,    // on_tx_message
    NULL,                           // on_tsx_state
};

// ============================================================================
// Public Interface Implementation
// ============================================================================

pj_status_t register_video_capability_module(void) {
    pj_status_t status;

    // Check whether already registered
    if (g_video_module) {
        VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Module already registered");
        return PJ_SUCCESS;
    }

    // Set module name
    video_capability_module.name = pj_str((char*)VIDEO_MODULE_NAME);

    // Register the module using pjsua_var's endpoint
    status = pjsip_endpt_register_module(pjsua_var.endpt, &video_capability_module);
    if (status != PJ_SUCCESS) {
        VCS_GATEWAY_LOG_ERROR(VIDEO_MODULE_LOG "Failed to register module: %d", status);
        return status;
    }

    g_video_module = &video_capability_module;

    VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Video capability module registered");
    VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "  - Video port: %d", g_video_port);
    VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "  - Codec: %s (pt=%s)", g_video_codec_name, g_video_payload_type);

    return PJ_SUCCESS;
}

void set_video_port(int port) {
    g_video_port = port;
    VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Video port set to %d", port);
}

void set_video_codecs(const char* codecs) {
    g_video_codec_name = codecs;
    VCS_GATEWAY_LOG_INFO(VIDEO_MODULE_LOG "Video codecs set to: %s", codecs);
}

int get_video_port(void) {
    return g_video_port;
}
