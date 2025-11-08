#pragma once

#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>

SSL_CTX* create_ssl_ctx(const std::string& key_path, const std::string& cert_path);
bool is_alpn_h2_selected(SSL* ssl);
