#include "h2_handler.h"
#include "common.h"

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

int download_handler(nghttp2_session* session, http2_session_data_t* session_data, http2_stream_data_t* stream_data, const std::string& rel_path)
{
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
