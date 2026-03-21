#pragma once

#include <iostream>
#include <memory>
#include <nghttp2/nghttp2.h>

#include "session.h"
#include "transport_policy.h"

class SessionEngine {
	public:
		static bool should_close_after_disconnect(const std::shared_ptr<SessionData>& session_data)
		{
			if (session_data->state != SessionState::DISCONNECTING) {
				return false;
			}

			// DISCONNECTING 단계에 진입 했으면 read는 더이상 중요치 않음, write 만 신경 쓰면 됨
			return !nghttp2_session_want_write(session_data->session) && session_data->output_buffer.empty();
		}

		static int send_server_connection_header(std::shared_ptr<SessionData> session_data)
		{
			nghttp2_settings_entry iv[1] = {
				{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
			};

			int rv = nghttp2_submit_settings(session_data->session, NGHTTP2_FLAG_NONE, iv, sizeof(iv) / sizeof(iv[0]));
			if (rv != 0) {
				std::cerr << "nghttp2_submit_settings() error: " << nghttp2_strerror(rv) << std::endl;
				return -1;
			}

			return 0;
		}

		static IOResult feed_input_buffer(const std::shared_ptr<SessionData>& session_data)
		{
			while (!session_data->input_buffer.empty()) {
				nghttp2_ssize fed_len = nghttp2_session_mem_recv2(
						session_data->session,
						session_data->input_buffer.data(),
						session_data->input_buffer.size());
				if (fed_len < 0) {
					std::cerr << "nghttp2_session_mem_recv2() error: " << nghttp2_strerror((int)fed_len) << std::endl;
					return IOResult::SHUTDOWN;
				}
				session_data->consume_input_buffer(fed_len);
			}
			return IOResult::SUCCESS;
		}

		static void fill_output_buffer(std::shared_ptr<SessionData> session_data)
		{
			while (nghttp2_session_want_write(session_data->session)) {
				const uint8_t* data;
				size_t length = nghttp2_session_mem_send2(session_data->session, &data);
				if (length <= 0) {
					break;
				}
				session_data->append_to_output_buffer(data, length);
			}
		}
};
