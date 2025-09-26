#pragma once
#include <list>
#include <vector>
#include <string>
#include <memory>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include "multipart_parser.h"
#include "file_cache.h"
#include "router.h"


enum class SessionState {
	CONNECTING,
	DISCONNECTING,
	TLS_HANDSHAKING,
	ESTABLISHING,
	ESTABLISHED
};

class http2_stream_data_t {
	public:
		uint32_t stream_id;
		METHOD method;
		std::string request_path;
		file_context_t file_ctx;
		std::unique_ptr<MultipartFormParser> mime_parser;
		std::vector<uint8_t> upload_file_buffer;

		http2_stream_data_t(uint32_t stream_id = 0);
		~http2_stream_data_t();
};

class http2_session_data_t {
	public:
		const Router* router;
		std::list<std::unique_ptr<http2_stream_data_t>> streams;
		nghttp2_session* session; // shared_ptr 로 바꿔 보기
		std::vector<uint8_t> output_buffer;
		std::vector<uint8_t> input_buffer;
		uint32_t events;
		SessionState state;
		SSL* ssl;

	public:
		http2_session_data_t();
		~http2_session_data_t();

		void append_to_output_buffer(const uint8_t* data, size_t length);
		void consume_output_buffer(size_t length);
		void append_to_input_buffer(const uint8_t* data, size_t length);
		void consume_input_buffer(size_t length);
};

class request_t {
	public:
		nghttp2_session* session;
		http2_session_data_t* session_data;
		http2_stream_data_t* stream_data;
		const std::string& rel_path;

		void clear_upload_file_buffer();
		request_t(nghttp2_session* session, http2_session_data_t* session_data, http2_stream_data_t* stream_data, const std::string& rel_path);
		~request_t();
};


std::unique_ptr<http2_stream_data_t> create_http2_stream_data(http2_session_data_t* session_data, uint32_t stream_id);
void delete_http2_stream_data(http2_session_data_t* session_data, http2_stream_data_t* stream_data);
void add_stream_to_session(http2_session_data_t* session_data, std::unique_ptr<http2_stream_data_t> stream_data);
int init_http2_session_data(std::shared_ptr<http2_session_data_t> session_data);
