#pragma once

#include <nghttp2/nghttp2.h>
#include "h2_session.h"

int download_handler(nghttp2_session* session, http2_session_data_t* session_data, http2_stream_data_t* stream_data, const std::string& rel_path);
