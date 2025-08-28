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
