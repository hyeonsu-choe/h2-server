#include <iostream>

#include "ssl_ctx.h"

const unsigned char alpn_proto_list[] = {
	0x02, 'h', '2',
	0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'
};

static int select_next_proto(const unsigned char** out, unsigned char *outlen,
								const unsigned char* in, unsigned int inlen,
								const unsigned char* key, unsigned int keylen)
{
	for (unsigned int i = 0; i + keylen <= inlen; i += (unsigned int)(in[i] + 1)) {
		if (memcmp(&in[i], key, keylen) == 0) {
			*out = (unsigned char*)&in[i + 1];
			*outlen = in[i];

			return 0;
		}
	}

	return -1;
}

// 서버 측에서 사용 할 프로토콜 선택 : h2만 지원
static int alpn_select_proto_cb(SSL *ssl,
		const unsigned char **out, unsigned char* outlen,
		const unsigned char *in, unsigned int inlen,
		void *arg)
{
	if (select_next_proto(out, outlen, in , inlen, alpn_proto_list, 3) == 0) {
		return SSL_TLSEXT_ERR_OK;
	}

//	if (select_next_proto(out, outlen, in , inlen, alpn_proto_list + 3, 9) == 0) {
//		return SSL_TLSEXT_ERR_OK;
//	}

	return SSL_TLSEXT_ERR_NOACK;
}

SSL_CTX *create_ssl_ctx(const char *key_path, const char *cert_path)
{
	SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
	if (!ssl_ctx) {
		std::cerr << "SSL_CTX_new() error: " << ERR_error_string(ERR_get_error(), NULL) << std::endl;
		return nullptr;
	}

	SSL_CTX_set_options(ssl_ctx,
						SSL_OP_ALL |
						SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
						SSL_OP_NO_COMPRESSION);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	if (SSL_CTX_set1_curves_list(ssl_ctx, "P-256") != 1) {
		std::cerr << "SSL_CTX_set1_curve_list() error: " << ERR_error_string(ERR_get_error(), NULL) << std::endl;
		return nullptr;
	}
#else
	EC_KEY* ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!ecdh) {
		std::cerr << "EC_KEY_new_by_curve_name() error: " << ERR_error_string(ERR_get_error(), NULL) << std::endl;
		return nullptr;
	}
	SSL_CTX_set_tmp_ecdh(ssl_ctx, ecdh);
	EC_KEY_free(ecdh);
#endif

	if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) != 1) {
		std::cerr << "SSL_CTX_use_PrivateKey_file() error: " << ERR_error_string(ERR_get_error(), NULL) << std::endl;
		return nullptr;
	}

	if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path)!= 1) {
		std::cerr << "SSL_CTX_use_certificate_chain_file() error: " << ERR_error_string(ERR_get_error(), NULL) << std::endl;
		return nullptr;
	}

	SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_proto_cb, NULL);

	return ssl_ctx;
}

bool is_alpn_h2_selected(SSL* ssl)
{
	const unsigned char* alpn = NULL;
	unsigned int alpn_len = 0;

    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);

    if (alpn == NULL || alpn_len != 2 || memcmp("h2", alpn, 2) != 0) {
		return false;
	}

	return true;
}

