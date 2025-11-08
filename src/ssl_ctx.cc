#include <iostream>
#include "ssl_ctx.h"

const unsigned char id_ctx[] = {
	"h2_server"
};

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

SSL_CTX *create_ssl_ctx(const std::string& key_path, const std::string& cert_path)
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

	if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
		std::cerr << "SSL_CTX_use_PrivateKey_file() error: " << ERR_error_string(ERR_get_error(), NULL) << std::endl;
		return nullptr;
	}

	if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path.c_str())!= 1) {
		std::cerr << "SSL_CTX_use_certificate_chain_file() error: " << ERR_error_string(ERR_get_error(), NULL) << std::endl;
		return nullptr;
	}

#if 0 // Session Resumption : h2load 테스트 결과 성능 하락이 관측 되어서 주석 처리함
	// Session Resumption TLS 1.3
	SSL_CTX_set_num_tickets(ssl_ctx, 2); // 세션 티켓 발급 개수
	SSL_CTX_set_timeout(ssl_ctx, 300); // SSL_CTX 객체가 아닌 세션 캐시에 있는 SSL_SESSION 객체들의 만료 시간 관리

	// Session Resumption TLS 1.2
	SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER); // TLS1.2의 세션 ID 재개 켜기
	SSL_CTX_set_session_id_context(ssl_ctx, id_ctx, sizeof(id_ctx) - 1);
#endif

	// ALPN 설정 콜백 함수 등록
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

