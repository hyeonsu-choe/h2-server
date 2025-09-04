#include "common.h"

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
		std::cerr << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}
    return 0;
}

int send_error_response(nghttp2_session* session, http2_stream_data_t* stream_data)
{
	if (error_reply(session, stream_data) != 0) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
	return 0;
}

int send_error_response_with_file(request_t& request)
{
	return send_error_response(request.session, request.stream_data);
}

int send_error_response(request_t& request)
{
    nghttp2_nv hdrs[] = {MAKE_NV(":status", "404")};

	int rv = nghttp2_submit_response2(request.session, request.stream_data->stream_id, hdrs, ARRLEN(hdrs), nullptr);
	if (rv != 0) {
		std::cerr << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
    return 0;
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

static int send_response(nghttp2_session* session, nghttp2_nv *nva, size_t nvlen, http2_stream_data_t* stream_data)
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

int send_ok_response_with_file(request_t& request)
{
	nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};
	if (send_response(request.session, hdrs, ARRLEN(hdrs), request.stream_data) < 0) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}

	return 0;
}

int send_ok_response(request_t& request)
{
    nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};

	int rv = nghttp2_submit_response2(request.session, request.stream_data->stream_id, hdrs, ARRLEN(hdrs), nullptr);
	if (rv != 0) {
		std::cerr << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
    return 0;
}
