#pragma once
#include <list>
#include <vector>
#include <string>

#include "multipart_parser.h"
#include "file_cache.h"

class Router;
enum METHOD : uint8_t;
struct nghttp2_session;

struct ssl_st;
using SSL = ssl_st;

enum class SessionState {
	CONNECTING,
	DISCONNECTING,
	TLS_HANDSHAKING,
	ESTABLISHING,
	ESTABLISHED
};

class StreamData {
	public:
		uint32_t stream_id;
		METHOD method;
		std::string request_path;
		FileContext file_ctx;
		std::unique_ptr<MultipartFormParser> mime_parser;
		std::vector<uint8_t> upload_file_buffer;

		StreamData(uint32_t stream_id = 0);
		~StreamData();
};

class SessionData {
	public:
		const Router* router;
		std::list<std::unique_ptr<StreamData>> streams;
		nghttp2_session* session;
		std::vector<uint8_t> output_buffer;
		std::vector<uint8_t> input_buffer;
		uint32_t events;
		SessionState state;
		SSL* ssl;
		bool is_closed;

	public:
		SessionData();
		~SessionData();

		void close_session();
		void append_to_output_buffer(const uint8_t* data, size_t length);
		void consume_output_buffer(size_t length);
		void append_to_input_buffer(const uint8_t* data, size_t length);
		void consume_input_buffer(size_t length);
};

class Request {
	public:
		nghttp2_session* session;
		SessionData* session_data;
		StreamData* stream_data;
		std::string_view rel_path;

		Request(nghttp2_session* session, SessionData* session_data, StreamData* stream_data, std::string_view rel_path);
		~Request();

		void clear_upload_file_buffer();
		int reply_ok();
		int reply_ok_with_file();
		int reply_404();
};


std::unique_ptr<StreamData> create_stream_data(SessionData* session_data, uint32_t stream_id);
void delete_stream_data(SessionData* session_data, StreamData* stream_data);
void add_stream_data_to_session(SessionData* session_data, std::unique_ptr<StreamData> stream_data);
int init_session_data(std::shared_ptr<SessionData> session_data);
