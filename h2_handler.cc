#include "h2_handler.h"
#include "common.h"


static std::shared_ptr<MappedFile> load_file_from_filecache(const std::string& path)
{
	// 검색 및 삽입 실행
	std::shared_ptr<MappedFile> file = find_file_from_filecache(path);
	if (!file) {
		file = insert_file_into_filecache(path);
		if (!file || !file->is_mapped()) {
			std::cerr << "failed to load file \"" << path << "\" from file cache" << std::endl;
			return nullptr;
		}
	} else {
		// 파일 이름은 등록 되어 있으나 실제 파일과 매핑이 되어 있지 않은 경우 매핑 시도
		if (!file->is_mapped()) {
			if (!file->map(path.c_str())) {
				return nullptr;
			}
		}
	}
	return file;
}

int downloader(request_t& request)
{
	http2_stream_data_t* stream_data = request.stream_data;
	const std::string& rel_path = request.rel_path;

	if (rel_path.empty()) {
		return send_error_response(request);
	}

	auto file = load_file_from_filecache(rel_path);
	if (!file) {
		return send_error_response(request);
	}

	stream_data->file_ctx.data = file->get_data();
	stream_data->file_ctx.size = file->get_data_len();

	return send_ok_response_with_file(request);
}

int uploader(request_t& request)
{
	http2_stream_data_t* stream_data = request.stream_data;
	const std::string& rel_path = request.rel_path;

	if (stream_data->mime_parser) {
		auto is_completed = stream_data->mime_parser->write(stream_data->upload_file_buffer);
		if (is_completed) {
			auto file = load_file_from_filecache(stream_data->mime_parser->get_filename());
			if (!file) {
				return send_error_response(request);
			}
		}
	}

	return send_ok_response(request);
}
