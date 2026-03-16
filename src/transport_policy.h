#pragma once

#include <nghttp2/nghttp2.h>

#include "session.h"
#include "ssl_ctx.h"

enum class IOResult {
	SUCCESS,
	AGAIN,
	SHUTDOWN,
};

inline constexpr int EPOLL_SIZE = 1024;
inline constexpr int BUF_SIZE = 2048;

class H2cTransportPolicy {
	public:
		static constexpr bool use_tls = false;

		class Context {
			public:
		};

		static bool init_context(Context& ctx, std::string_view key_path, std::string_view cert_path)
		{
			return true;
		}

		static void deinit_context(Context& ctx)
		{

		}

		static bool on_accept(Context& ctx, int sock, const std::shared_ptr<SessionData>& session_data)
		{
			return true;
		}

		static void update_event(int epfd, int sock, uint32_t events, const std::shared_ptr<SessionData>& session_data) 
		{
			struct epoll_event ev;
			ev.events = events | EPOLLET | EPOLLRDHUP;
			ev.data.fd = sock;
			if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev) < 0) {
				if (errno == ENOENT) {
					epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
				}
			}
			session_data->events = events;
		}

		static void update_events(int epfd, int sock, const std::shared_ptr<SessionData>& session_data)
		{
			uint32_t events = 0;

			// 종료 상태일때 EPOLLIN 등록 배제 : h2load 연속 반복 테스트시 read 무한 대기 하며 종료가 제대로 안 됨
			if (session_data->state == SessionState::DISCONNECTING) {
				if (nghttp2_session_want_write(session_data->session) ||
						!session_data->output_buffer.empty()) {
					events |= EPOLLOUT;
				}
			} else {
				if (nghttp2_session_want_read(session_data->session)) {
					events |= EPOLLIN;
				}

				if (nghttp2_session_want_write(session_data->session) ||
						!session_data->output_buffer.empty()) {
					events |= EPOLLOUT;
				}

				if (!events) events = EPOLLIN; // default로 하지 않는 것은 EPOLLIN만 무조건 등록이 유지되어 부하가 생기는 것을 방지
			}

			update_event(epfd, sock, events, session_data);
		}

		static IOResult fill_input_buffer(int sock, const std::shared_ptr<SessionData>& session_data)
		{
			unsigned char buffer[BUF_SIZE];

			while (true) {
				ssize_t read_len = read(sock, buffer, BUF_SIZE);
				if (read_len == 0) {
					return IOResult::SHUTDOWN;
				} else if (read_len < 0) {
					if (errno == EINTR) {
						continue;
					} else if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
						return IOResult::AGAIN;
					}

					return IOResult::SHUTDOWN;
				}
				session_data->append_to_input_buffer(buffer, read_len);
			}
		}

		static IOResult flush_output_buffer(int sock, const std::shared_ptr<SessionData>& session_data)
		{
			while (!session_data->output_buffer.empty()) {
				ssize_t written_len = write(sock, session_data->output_buffer.data(), session_data->output_buffer.size());
				if (written_len < 0) {
					if (errno == EINTR) {
						continue;
					} else if (errno == EAGAIN || errno == EWOULDBLOCK) { // 현재 처리 할 데이터가 없으므로 다음 EPOLLOUT 때 처리
						return IOResult::AGAIN;
					}
					return IOResult::SHUTDOWN; // ex: EPIPE
				}
				session_data->consume_output_buffer(written_len);
			}
			return IOResult::SUCCESS;
		}

		static void disconnect(Context& ctx, const std::shared_ptr<SessionData>& session_data)
		{
			if (session_data) {
				session_data->close_session();
			}
		}
};

class TlsTransportPolicy {
	public:
		static constexpr bool use_tls = true;

		class Context {
			public:
				SSL_CTX* ssl_ctx = nullptr;
		};

		static bool init_context(Context& ctx, std::string_view key_path, std::string_view cert_path)
		{
			SSL_load_error_strings();
			OpenSSL_add_ssl_algorithms();

			ctx.ssl_ctx = create_ssl_ctx(key_path, cert_path);
			return  ctx.ssl_ctx != nullptr;
		}

		static void deinit_context(Context& ctx)
		{
			if (ctx.ssl_ctx) {
				SSL_CTX_free(ctx.ssl_ctx);
				ctx.ssl_ctx = nullptr;
			}
		}

		static bool validate_alpn(const std::shared_ptr<SessionData>& session_data)
		{
			const unsigned char* alpn_proto = nullptr;
			unsigned int alpn_proto_len = 0;
			SSL_get0_alpn_selected(session_data->ssl, &alpn_proto, &alpn_proto_len);

			return alpn_proto && alpn_proto_len == 2 && memcmp(alpn_proto, "h2", 2) == 0;
		}

		static bool on_accept(Context& ctx, int sock, const std::shared_ptr<SessionData>& session_data)
		{
			SSL* ssl = SSL_new(ctx.ssl_ctx);
			if (!ssl) {
				return false;
			}

			SSL_set_fd(ssl, sock);
			session_data->ssl = ssl;
			return true;
		}

		static void update_event(int epfd, int sock, uint32_t events, const std::shared_ptr<SessionData>& session_data) 
		{
			struct epoll_event ev;
			ev.events = events | EPOLLET | EPOLLRDHUP;
			ev.data.fd = sock;
			if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev) < 0) {
				if (errno == ENOENT) {
					epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
				}
			}
			session_data->events = events;
		}

		static void update_ssl_handshake_events(int epfd, int sock, const std::shared_ptr<SessionData>& session_data)
		{
			uint32_t events = 0;

			if (SSL_want_read(session_data->ssl)) {
				events |= EPOLLIN;
			}

			if (SSL_want_write(session_data->ssl)) {
				events |= EPOLLOUT;
			}

			if (events == 0) {
				events = EPOLLIN;
			}

			update_event(epfd, sock, events, session_data);
		}

		static bool do_handshake(int epfd, int sock, std::shared_ptr<SessionData> session_data)
		{
			SSL* ssl = session_data->ssl;

			int ret = SSL_accept(ssl);
			if (ret > 0) {
				if (!validate_alpn(session_data)) {
					return false;
				}
#if 0 // Session Resumption 동작 여부 확인을 위한 구간
					int reused = SSL_session_reused(ssl);
					const char* ver = SSL_get_version(ssl);
					std::cout << "TLS ver: " << ver << ", session_reused=" << reused << std::endl;
#endif
				return true;
			} 

			int ssl_err = SSL_get_error(ssl, ret);
			if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
				update_ssl_handshake_events(epfd, sock, session_data);
				session_data->state = SessionState::TLS_HANDSHAKING;
				return false;
			}

			uint32_t openssl_err = ERR_get_error();
			if (openssl_err != 0) {
				std::cerr << "SSL_accept() failed: ssl_err=" << ssl_err
					<< ", openssl_err=" << ERR_error_string(openssl_err, nullptr)
					<< std::endl;
			} else {
				std::cerr << "SSL_accept() failed: ssl_err=" << ssl_err
					<< ", errno=" << errno << " (" << strerror(errno) << ")"
					<< std::endl;
			}

			return false;
		}

		static void update_events(int epfd, int sock, const std::shared_ptr<SessionData>& session_data)
		{
			uint32_t events = 0;

			if (session_data->state == SessionState::DISCONNECTING) {
				if (SSL_want_write(session_data->ssl) ||
						nghttp2_session_want_write(session_data->session) ||
						!session_data->output_buffer.empty()) {
					events |= EPOLLOUT;
				}
			} else {
				if (SSL_want_read(session_data->ssl) ||
						nghttp2_session_want_read(session_data->session)) {
					events |= EPOLLIN;
				}

				if (SSL_want_write(session_data->ssl) ||
						nghttp2_session_want_write(session_data->session) ||
						!session_data->output_buffer.empty()) {
					events |= EPOLLOUT;
				}

				if (!events) events = EPOLLIN; // default로 하지 않는 것은 EPOLLIN만 무조건 등록이 유지되어 부하가 생기는 것을 방지
			}

			update_event(epfd, sock, events, session_data);
		}

		static IOResult fill_input_buffer(int sock, const std::shared_ptr<SessionData>& session_data)
		{
			unsigned char buffer[BUF_SIZE];
			SSL* ssl = session_data->ssl;

			while (true) {
				int ret = SSL_read(ssl, buffer, BUF_SIZE);
				if (ret <= 0) {
					int err = SSL_get_error(ssl, ret);
					switch (err) {
						case SSL_ERROR_WANT_READ:
						case SSL_ERROR_WANT_WRITE:
							return IOResult::AGAIN; // read 할 데이터가 더이상 없음
						case SSL_ERROR_SYSCALL:
							if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
								return IOResult::AGAIN;
							} else if (errno == EINTR) {
								continue;
							}
							return IOResult::SHUTDOWN;
						case SSL_ERROR_ZERO_RETURN: // ret 값이 0 일때 ZERO 값이 리턴 됨 : if문 비교를 덜 하기위해 여기서 한 번에 처리토록 함
							return IOResult::SHUTDOWN;
						default:
							std::cerr << "SSL_read() error: " << strerror(errno) << std::endl;
							return IOResult::SHUTDOWN;
					}
				}
				session_data->append_to_input_buffer(buffer, ret);
			}
		}

		static IOResult flush_output_buffer(int sock, const std::shared_ptr<SessionData>& session_data)
		{
			SSL* ssl = session_data->ssl;

			while (!session_data->output_buffer.empty()) {
				int ret = SSL_write(ssl, session_data->output_buffer.data(), session_data->output_buffer.size());
				if (ret <= 0) {
					int err = SSL_get_error(ssl, ret);
					switch (err) {
						case SSL_ERROR_WANT_READ:
						case SSL_ERROR_WANT_WRITE:
							return IOResult::AGAIN;
						case SSL_ERROR_SYSCALL:
							if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
								return IOResult::AGAIN;
							} else if (errno == EINTR) {
								continue;
							}
							return IOResult::SHUTDOWN; // ex: EPIPE
						case SSL_ERROR_ZERO_RETURN: { // ret 값이 0 일때 ZERO 값이 리턴 됨 : if문 비교를 덜 하기위해 여기서 한 번에 처리토록 함
							return IOResult::SHUTDOWN;
						}
						default:
							std::cerr << "SSL_write() error: " << strerror(errno) << std::endl;
							return IOResult::SHUTDOWN;
					}
				}
				session_data->consume_output_buffer(ret);
			}
			return IOResult::SUCCESS;
		}

		static void disconnect(Context& ctx, const std::shared_ptr<SessionData>& session_data)
		{
			if (session_data && session_data->ssl) {
				SSL_set_quiet_shutdown(session_data->ssl, 1);
				session_data->close_session();
			}
		}
};
