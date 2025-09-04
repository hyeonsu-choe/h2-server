#pragma once

#include <nghttp2/nghttp2.h>
#include "h2_session.h"

int downloader(request_t& request);
int uploader(request_t& request);
