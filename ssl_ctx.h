#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
//#include <openssl/ec.h>
//#include <openssl/evp.h>

SSL_CTX* create_ssl_ctx(const char* key_path, const char* cert_path);
bool is_alpn_h2_selected(SSL* ssl);
