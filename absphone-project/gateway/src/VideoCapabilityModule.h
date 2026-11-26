/**
 * @file VideoCapabilityModule.h
 * @brief SIP Video Capability Declaration Module
 *
 * Intercepts SDP messages via a pjsip module to ensure the video port is not 0.
 * Supports multiple video codecs: H264, H265, VP8, VP9, etc.
 *
 * Design principles:
 * 1. Does not depend on any actual video codec implementation
 * 2. Only modifies the port of the video media line in the SDP
 * 3. Actual video processing is done by the Media module via RTPEngine
 */

#ifndef VIDEO_CAPABILITY_MODULE_H
#define VIDEO_CAPABILITY_MODULE_H

#include <pj/pool.h>

/**
 * @brief Register the video capability declaration module
 *
 * Call this function after the SIP stack is initialized to register the module.
 * The module intercepts all outgoing SIP responses and checks whether they
 * contain an SDP. If so, it ensures the video port is not 0.
 *
 * @return PJ_SUCCESS on success
 */
pj_status_t register_video_capability_module(void);

/**
 * @brief Set the video port number
 * @param port Video RTP port number (e.g., 4002)
 */
void set_video_port(int port);

/**
 * @brief Set the list of video codecs to declare
 * @param codecs Comma-separated list of codec names (e.g., "H264,VP8")
 *
 * Note: Codec names must match the fmt fields in the SDP
 */
void set_video_codecs(const char* codecs);

/**
 * @brief Get the currently configured video port
 */
int get_video_port(void);

#endif // VIDEO_CAPABILITY_MODULE_H
