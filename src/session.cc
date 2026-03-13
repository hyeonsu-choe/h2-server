#include <iostream>
#include <utility>
#include <memory>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include "session.h"
#include "router.h"

namespace {
constexpr const char ERROR_HTML[] = "<html><head><title>404</title></head>"
                                  "<body><h1>404 Not Found</h1></body></html>";

#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

#define MAKE_NV(NAME, VALUE)                                                   \
  {                                                                            \
    (uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

// nghttp2_seesion_mem_send2 호출 하여 프레임들을 큐에서 꺼내서 시리얼라이즈 할 때
// data 프레임이 필요한 경우 이 함수가 호출되어 데이터를 채움 (nghttp2_submit_response2 호출 때 호출되는 것이 아님에 유의)
nghttp2_ssize file_read_callback(nghttp2_session* session, int32_t stream_id,
                                        uint8_t* buf, size_t length,
                                        uint32_t* data_flags, nghttp2_data_source* source,
                                        void* user_data)
{
	FileContext* ctx = static_cast<FileContext*>(source->ptr);
	if (!ctx) {
		return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
	}

	size_t remain_len = ctx->size - ctx->offset;
	size_t copy_len = std::min(length, remain_len);

	memcpy(buf, ctx->data + ctx->offset, copy_len);
	ctx->offset += copy_len;

	if (ctx->offset == ctx->size) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
	}
    return (nghttp2_ssize)copy_len;
}

int send_response(nghttp2_session* session, nghttp2_nv *nva, size_t nvlen, StreamData* stream_data)
{
	nghttp2_data_provider2 data_prd;
	data_prd.source.ptr = &stream_data->file_ctx;
	data_prd.read_callback = file_read_callback;

	int rv = nghttp2_submit_response2(session, stream_data->stream_id, nva, nvlen, &data_prd);
	if (rv != 0) {
		std::cout << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}
	return 0;
}

nghttp2_ssize error_read_callback(nghttp2_session* session, int32_t stream_id,
                                        uint8_t* buf, size_t length,
                                        uint32_t* data_flags, nghttp2_data_source* source,
                                        void* user_data )
{
	size_t copy_len = std::min(length, strlen(ERROR_HTML));
	memcpy(buf, ERROR_HTML, std::min(length, copy_len));
	*data_flags |= NGHTTP2_DATA_FLAG_EOF;
	return copy_len;
}

int error_reply(nghttp2_session* session, StreamData* stream_data)
{
    nghttp2_nv hdrs[] = {MAKE_NV(":status", "404")};
	nghttp2_data_provider2 data_prd;
	data_prd.read_callback = error_read_callback;

	int rv = nghttp2_submit_response2(session, stream_data->stream_id, hdrs, ARRLEN(hdrs), &data_prd);
	if (rv != 0) {
		std::cerr << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}
    return 0;
}

int on_request_recv(nghttp2_session* session, SessionData* session_data, StreamData* stream_data)
{
	auto router = session_data->router;
	if (router) {
		auto result = router->resolve(stream_data->method, stream_data->request_path);
		if (result.handler) {
			Request request(session, session_data, stream_data, result.param);
			return (*result.handler)(request);
		}

		Request request(session, session_data, stream_data, "");
		return request.reply_404();
	}

	return NGHTTP2_ERR_CALLBACK_FAILURE;
}

// nghttp2_session_mem_recv2 호출 시, 프레임이 모두 도착 되었다고 판정 될 경우 호출
int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame *frame, void *user_data)
{
	SessionData* session_data = static_cast<SessionData*>(user_data);
	StreamData* stream_data = nullptr;

	switch (frame->hd.type) {
		case NGHTTP2_HEADERS:
		case NGHTTP2_DATA:
			if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
				stream_data = static_cast<StreamData*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
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
	SessionData* session_data = (SessionData*)user_data;
	StreamData* stream_data = nullptr;
	(void)error_code;

	stream_data = static_cast<StreamData*>(nghttp2_session_get_stream_user_data(session, stream_id));
	if (!stream_data) {
		return 0;
	}

	delete_stream_data(session_data, stream_data);
	return 0;
}

// on_begin_headers_callback 호출된 뒤 부터, 각 http 헤더, 필드 쌍 해석 때마다 호출
int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
							const uint8_t* name, size_t namelen,
							const uint8_t *value, size_t valuelen,
							uint8_t flags, void* user_data)
{
	StreamData* stream_data;
	const char path[] = ":path";
	const char method[] = ":method";
	const char content_type[] = "content-type";

	switch (frame->hd.type) {
		case NGHTTP2_HEADERS:
			if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
				break;
			}
			if ((namelen == sizeof(path) - 1) && (memcmp(path, name, namelen) == 0)) {
				stream_data = static_cast<StreamData*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					break;
				}
				stream_data->request_path.assign(reinterpret_cast<const char*>(value), valuelen);
			} else if ((namelen == sizeof(method) - 1) && (memcmp(method, name, namelen) == 0)) {
				stream_data = static_cast<StreamData*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					break;
				}
		
				if (valuelen == 3 && memcmp("GET", value, valuelen) == 0) {
					stream_data->method = GET;
				} else if (valuelen == 4 && memcmp("POST", value, valuelen) == 0) {
					stream_data->method = POST;
					stream_data->mime_parser = std::make_unique<MultipartFormParser>();
				}
			} else if ((namelen == sizeof(content_type) - 1) && (memcmp(content_type, name, namelen) == 0)) {
				stream_data = static_cast<StreamData*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					break;
				}

				if (stream_data->mime_parser) {
					std::string_view content_type_view(reinterpret_cast<const char*>(value), valuelen);
					stream_data->mime_parser->extract_boundary(content_type_view);
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
	SessionData* session_data = (SessionData*)user_data;

	if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
		return 0;
	}

	std::unique_ptr<StreamData> stream_data = create_stream_data(session_data, frame->hd.stream_id);
	add_stream_data_to_session(session_data, std::move(stream_data));
	return 0;
}

// data 프레임의 body 부를 읽어들이는 동안 읽혀진 chunk 단위로 호출
// on_frame_recv_callback은 data 프레임이 바디부 까지 모두 전송 완료 되면 1회 호출 됨
int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data)
{
	SessionData* session_data = (SessionData*)user_data;
	StreamData* stream_data = nullptr;

	stream_data = static_cast<StreamData*>(nghttp2_session_get_stream_user_data(session, stream_id));
	if (!stream_data) {
		return 0;
	}

	if (len > 0) {
		stream_data->upload_file_buffer.insert(stream_data->upload_file_buffer.end(), data, data + len);
	}

//	for (size_t i = 0; i < len; i++) {
//		std::cout << *(data + i);
//	}
//	std::cout << std::endl;

	return 0;
}


}


StreamData::StreamData(uint32_t stream_id)
	: stream_id(stream_id)
{

}

StreamData::~StreamData()
{

}

SessionData::SessionData()
	: router(nullptr), session(nullptr), events(0), state(SessionState::CONNECTING), ssl(nullptr), is_closed(false)
{

}

SessionData::~SessionData()
{
	if (session) {
		nghttp2_session_del(session);
		session = nullptr;
	}

	if (ssl) {
		SSL_free(ssl);
		ssl = nullptr;
	}
}

void SessionData::close_session()
{
	if (is_closed) return;

	if (ssl) {
		SSL_shutdown(ssl);
	}
	is_closed = true;
}

void SessionData::append_to_output_buffer(const uint8_t* data, size_t length)
{
	output_buffer.insert(output_buffer.end(), data, data + length);
}

void SessionData::consume_output_buffer(size_t length)
{
	output_buffer.erase(output_buffer.begin(), output_buffer.begin() + length);
}

void SessionData::append_to_input_buffer(const uint8_t* data, size_t length)
{
	input_buffer.insert(input_buffer.end(), data, data + length);
}

void SessionData::consume_input_buffer(size_t length)
{
	input_buffer.erase(input_buffer.begin(), input_buffer.begin() + length);
}

Request::Request(nghttp2_session* session, SessionData* session_data, StreamData* stream_data, std::string_view rel_path)
	: session(session), session_data(session_data), stream_data(stream_data), rel_path(rel_path)
{

}

Request::~Request()
{
	clear_upload_file_buffer(); // 파일이 하나의 스트림 내에서 여러 DATA 프레임으로 나뉘어져 수신되는 경우를 위해, 다음 data 프레임의 chunk 데이터들을 처음 부터 읽기 위해 초기화 (메모리 공간 절약 목적)
}

void Request::clear_upload_file_buffer()
{
	if (stream_data && not stream_data->upload_file_buffer.empty()) {
		stream_data->upload_file_buffer.clear();
	}
}

int Request::reply_ok()
{
    nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};

	int rv = nghttp2_submit_response2(session, stream_data->stream_id, hdrs, ARRLEN(hdrs), nullptr);
	if (rv != 0) {
		std::cerr << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
    return 0;
}

int Request::reply_ok_with_file()
{
	nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};
	if (send_response(session, hdrs, ARRLEN(hdrs), stream_data) < 0) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
	return 0;
}

int Request::reply_404()
{
	if (error_reply(session, stream_data) != 0) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
	return 0;
}


std::unique_ptr<StreamData> create_stream_data(SessionData* session_data, uint32_t stream_id)
{
	return std::make_unique<StreamData>(stream_id);
}

void delete_stream_data(SessionData* session_data, StreamData* stream_data)
{
	auto iter = std::find_if(session_data->streams.begin(), session_data->streams.end(),
			[stream_data](const std::unique_ptr<StreamData>& ptr) {
				return ptr.get() == stream_data;
			});

	if (iter != session_data->streams.end()) {
		session_data->streams.erase(iter);
	}
}

void add_stream_data_to_session(SessionData* session_data, std::unique_ptr<StreamData> stream_data)
{
	uint32_t stream_id = stream_data->stream_id;

	nghttp2_session_set_stream_user_data(session_data->session, stream_id, stream_data.get());
	session_data->streams.push_back(std::move(stream_data));
}

// nghttp2 세션 객체에 콜백 함수들 등록
int init_session_data(std::shared_ptr<SessionData> session_data)
{
	nghttp2_session_callbacks *callbacks = nullptr;
	try {
		if (nghttp2_session_callbacks_new(&callbacks) != 0) {
			throw std::runtime_error("nghttp2_session_callbacks_new() error");
		}

		nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback); // 프레임 모두 도착 시 호출 (프레임 n개 도착 시, n번 호출)
		nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback); // 스트림 닫히려고 할 때 호출
		nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback); // 헤더의 name-value 쌍 확인 및 저장
		nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback); //HEADERS 또는 PUSH_PROMISE 프레임에서 HPACK으로 인코딩된 헤더 블록 수신 시작 시 호출
		nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);

		if (nghttp2_session_server_new(&session_data->session, callbacks, session_data.get()) != 0) {
			throw std::runtime_error("nghttp2_session_server_new() error");
		}

	} catch (const std::exception& ex) {
		std::cerr << "exception: " << ex.what() << std::endl;
		if (callbacks) {
			nghttp2_session_callbacks_del(callbacks);
		}
		return -1;
	}

	// callbacks 내용을 nghttp2_session_server_new 함수 호출을 통해
	// nghttp2 세션 객체에 복사하여 넘겨 필요가 없어졌으니, 메모리 누수 방지를 위해 반환
	nghttp2_session_callbacks_del(callbacks);
	return 0;
}
