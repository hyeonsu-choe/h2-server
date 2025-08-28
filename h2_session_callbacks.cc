#include <iostream>
#include <utility>
#include <memory>

#include "h2_session.h"
#include "h2_session_callbacks.h"
#include "h2_handler.h"
#include "common.h"

static int on_request_recv(nghttp2_session* session, http2_session_data_t* session_data, http2_stream_data_t* stream_data)
{
	auto router = session_data->router;
	if (router) {
		auto result = router->resolve(stream_data->method, stream_data->request_path);
		if (result.handler) {
			return (*result.handler)(session, session_data, stream_data, result.param);
		}

		return send_error_response(session, stream_data);
	}
		
	return NGHTTP2_ERR_CALLBACK_FAILURE;
}

// nghttp2_session_mem_recv2 호출 시, 프레임이 모두 도착 되었다고 판정 될 경우 호출
int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame *frame, void *user_data)
{
	http2_session_data_t* session_data = static_cast<http2_session_data_t*>(user_data);	
	http2_stream_data_t* stream_data;

	switch (frame->hd.type) {
		case NGHTTP2_HEADERS:
		case NGHTTP2_DATA:
			if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
				stream_data = static_cast<http2_stream_data_t*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					return 0;
				}
				return on_request_recv(session, session_data, stream_data);
			}
			break;
		case NGHTTP2_GOAWAY:
			{
				uint32_t last_stream_id = nghttp2_session_get_last_proc_stream_id(session);
				nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, last_stream_id, NGHTTP2_NO_ERROR, nullptr, 0);
				session_data->state = SessionState::DISCONNECTING;
			}
			break;
		default:
			break;
	}
	return 0;
}

// 스트림 닫히려고 할 때 호출
int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data)
{
	http2_session_data_t* session_data = (http2_session_data_t*)user_data;
	http2_stream_data_t* stream_data;
	(void)error_code;

	stream_data = static_cast<http2_stream_data_t*>(nghttp2_session_get_stream_user_data(session, stream_id));
	if (!stream_data) {
		return 0;
	}

	delete_http2_stream_data(session_data, stream_data);
	return 0;
}

// on_begin_headers_callback 호출된 뒤 부터, 각 http 헤더, 필드 쌍 해석 때마다 호출
int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
							const uint8_t* name, size_t namelen,
							const uint8_t *value, size_t valuelen,
							uint8_t flags, void* user_data)
{
	http2_stream_data_t* stream_data;
	const char PATH[] = ":path";
	const char METHOD[] = ":method";

	switch (frame->hd.type) {
		case NGHTTP2_HEADERS:
			if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
				break;
			}
			if ((namelen == sizeof(PATH) - 1) && (memcmp(PATH, name, namelen) == 0)) {
				stream_data = static_cast<http2_stream_data_t*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					break;
				}

				stream_data->request_path.assign(reinterpret_cast<const char*>(value), valuelen);
			} else if ((namelen == sizeof(METHOD) - 1) && (memcmp(METHOD, name, namelen) == 0)) {
				stream_data = static_cast<http2_stream_data_t*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					break;
				}
		
				if (valuelen == 3 && memcmp("GET", value, valuelen) == 0) {
					stream_data->method = GET;
				} else {
					stream_data->method = POST;
				}
			}

			// 헤더 출력
			//std::string name_str(reinterpret_cast<const char*>(name), namelen);
			//std::string value_str(reinterpret_cast<const char*>(value), valuelen);
			//std::cout << "name: " << name_str << std::endl;
			//std::cout << "value: " << value_str << std::endl;
			break;
	}
	return 0;
}

// nghttp2_session_mem_recv2 호출 시,
// HEADERS 프레임 또는 또는 PUSH_PROMISE 프레임 내 헤더 블록(HPACK으로 인코딩 된) 수신이 시작될 때 호출
int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
	http2_session_data_t* session_data = (http2_session_data_t*)user_data;

	if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
		return 0;
	}

	std::unique_ptr<http2_stream_data_t> stream_data = create_http2_stream_data(session_data, frame->hd.stream_id);
	add_stream_to_session(session_data, std::move(stream_data));
	return 0;
}
