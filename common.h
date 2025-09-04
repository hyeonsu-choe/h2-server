#pragma once

#include "h2_session.h"

#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

#define MAKE_NV(NAME, VALUE)                                                   \
  {                                                                            \
    (uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

int send_error_response(nghttp2_session* session, http2_stream_data_t* stream_data);
int send_error_response_with_file(request_t& request);
int send_error_response(request_t& request);
int send_ok_response_with_file(request_t& request);
int send_ok_response(request_t& request);
