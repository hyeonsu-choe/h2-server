#include "http2_session.h"
#include "http2_callbacks.h"

http2_stream_data_t::http2_stream_data_t(uint32_t stream_id = 0)
	: stream_id(stream_id)
{

}

http2_stream_data_t::~http2_stream_data_t()
{

}

http2_session_data_t::http2_session_data_t()
	: session(nullptr), events(0), state(SessionState::CONNECTING), ssl(nullptr)
{

}

http2_session_data_t::~http2_session_data_t()
{
	if (session) {
		nghttp2_session_del(session);
	}

	if (ssl) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
	}
}

void http2_session_data_t::append_to_output_buffer(const uint8_t* data, size_t length)
{
	output_buffer.insert(output_buffer.end(), data, data + length);
}

void http2_session_data_t::consume_output_buffer(size_t length)
{
	output_buffer.erase(output_buffer.begin(), output_buffer.begin() + length);
}

void http2_session_data_t::append_to_input_buffer(const uint8_t* data, size_t length)
{
	input_buffer.insert(input_buffer.end(), data, data + length);
}

void http2_session_data_t::consume_input_buffer(size_t length)
{
	input_buffer.erase(input_buffer.begin(), input_buffer.begin() + length);
}


std::unique_ptr<http2_stream_data_t> create_http2_stream_data(http2_session_data_t* session_data, uint32_t stream_id)
{
	return std::make_unique<http2_stream_data_t>(stream_id);
}

void delete_http2_stream_data(http2_session_data_t* session_data, http2_stream_data_t* stream_data)
{
	auto iter = std::find_if(session_data->streams.begin(), session_data->streams.end(),
			[stream_data](const std::unique_ptr<http2_stream_data_t>& ptr) {
				return ptr.get() == stream_data;
			});

	if (iter != session_data->streams.end()) {
		session_data->streams.erase(iter);
	}
}

void add_stream_to_session(http2_session_data_t* session_data, std::unique_ptr<http2_stream_data_t> stream_data)
{
	uint32_t stream_id = stream_data->stream_id;

	nghttp2_session_set_stream_user_data(session_data->session, stream_id, stream_data.get());
	session_data->streams.push_back(std::move(stream_data));
}

int init_http2_session_data(std::shared_ptr<http2_session_data_t> session_data)
{
	nghttp2_session_callbacks *callbacks;
	try {
		if (nghttp2_session_callbacks_new(&callbacks) != 0) {
			throw std::runtime_error("nghttp2_session_callbacks_new() error");
		}

		nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback); // 프레임 모두 도착 시 호출 (프레임 n개 도착 시, n번 호출)
		nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback); // 스트림 닫히려고 할 때 호출
		nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback); // 헤더의 name-value 쌍 확인 및 저장
		nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback); //HEADERS 또는 PUSH_PROMISE 프레임에서 헤더 블록 수신 시작 시 호출

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

	nghttp2_session_callbacks_del(callbacks);
	return 0;
}
