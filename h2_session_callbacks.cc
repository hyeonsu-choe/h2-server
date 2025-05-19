#include <iostream>
#include <utility>
#include <memory>

#include "h2_session.h"
#include "h2_session_callbacks.h"

#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

#define MAKE_NV(NAME, VALUE)                                                   \
  {                                                                            \
    (uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

// nghttp2_seesion_mem_send2 호출 하여 프레임들을 큐에서 꺼내서 시리얼라이즈 할 때
// data 프레임이 필요한 경우 이 함수가 호출되어 데이터를 채움 (nghttp2_submit_response2 호출 때 호출되는 것이 아님에 유의)
static nghttp2_ssize file_read_callback(nghttp2_session* session, int32_t stream_id,
                                        uint8_t* buf, size_t length,
                                        uint32_t* data_flags, nghttp2_data_source* source,
                                        void* user_data)
{
	file_context_t* ctx = static_cast<file_context_t*>(source->ptr);
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

static int send_response(nghttp2_session* session, int32_t stream_id,
                         nghttp2_nv *nva, size_t nvlen, http2_stream_data_t* stream_data)
{
	nghttp2_data_provider2 data_prd;
	data_prd.source.ptr = &stream_data->file_ctx;
	data_prd.read_callback = file_read_callback;

	int rv = nghttp2_submit_response2(session, stream_id, nva, nvlen, &data_prd);
	if (rv != 0) {
		std::cout << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}
	return 0;
}

static const char ERROR_HTML[] = "<html><head><title>404</title></head>"
                                  "<body><h1>404 Not Found</h1></body></html>";
static nghttp2_ssize error_read_callback(nghttp2_session* session, int32_t stream_id,
                                        uint8_t* buf, size_t length,
                                        uint32_t* data_flags, nghttp2_data_source* source,
                                        void* user_data )
{
	size_t copy_len = std::min(length, strlen(ERROR_HTML));
	memcpy(buf, ERROR_HTML, std::min(length, copy_len));
	*data_flags |= NGHTTP2_DATA_FLAG_EOF;
	return copy_len;
}

static int error_reply(nghttp2_session* session, http2_stream_data_t* stream_data)
{
    nghttp2_nv hdrs[] = {MAKE_NV(":status", "404")};
	nghttp2_data_provider2 data_prd;
	data_prd.read_callback = error_read_callback;

	int rv = nghttp2_submit_response2(session, stream_data->stream_id, hdrs, ARRLEN(hdrs), &data_prd);
	if (rv != 0) {
		std::cout << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}
    return 0;
}

static int send_error_response(nghttp2_session* session, http2_stream_data_t* stream_data)
{
	if (error_reply(session, stream_data) != 0) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
	return 0;
}

static std::string extract_rel_path(const std::string& path)
{
	size_t pos = path.find_first_not_of('/');
	if (pos == std::string::npos) {
		return "";
	}
	return path.substr(pos);
}

static std::shared_ptr<MappedFile> load_file_from_filecache(const std::string& path)
{
	// 검색 및 삽입 실행
	std::shared_ptr<MappedFile> file = find_file_from_filecache(path);
	if (!file) {
		file = insert_file_into_filecache(path);
		if (!file || !file->get_data()) {
			std::cerr << "failed to load file \"" << path << "\" from file cache" << std::endl;
		}
	}
	return file;
}

static int on_request_recv(nghttp2_session* session, http2_session_data_t* session_data, http2_stream_data_t* stream_data)
{
	std::string rel_path = extract_rel_path(stream_data->request_path);
	if (rel_path.empty()) {
		return send_error_response(session, stream_data);
	}

	auto file = load_file_from_filecache(rel_path);
	if (!file) {
		return send_error_response(session, stream_data);
	}

	stream_data->file_ctx.data = file->get_data();
	stream_data->file_ctx.size = file->get_data_len();

	nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};
	if (send_response(session, stream_data->stream_id, hdrs, ARRLEN(hdrs), stream_data) < 0) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
	return 0;
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

				stream_data->request_path = reinterpret_cast<const char*>(value);
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
