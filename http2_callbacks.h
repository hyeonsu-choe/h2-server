#pragma once

#include <nghttp2/nghttp2.h>

int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame *frame, void *user_data);
int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);
int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
							const uint8_t* name, size_t namelen,
							const uint8_t *value, size_t valuelen,
							uint8_t flags, void* user_data);
int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
