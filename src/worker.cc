#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include "worker.h"
#include "ssl_ctx.h"
#include "file_cache.h"

const int EPOLL_SIZE = 1024;
const int BUF_SIZE = 2048;


Worker::Worker(const Router* router, bool use_tls = true) :
	server_sock(-1),
	use_tls(use_tls), router(router),
	epfd(-1),
	ssl_ctx(nullptr),
	update_events(nullptr), fill_input_buffer(nullptr), flush_output_buffer(nullptr)
{
	bind_callbacks_for_mode(use_tls);
}

Worker::Worker(Worker&& worker)
{
	router = std::exchange(worker.router, nullptr);
	use_tls = worker.use_tls;
	epfd = std::exchange(worker.epfd, -1);
	server_sock = std::exchange(worker.server_sock, -1);
	server_addr = worker.server_addr;
	ssl_ctx = std::exchange(worker.ssl_ctx, nullptr);

	session_map.swap(worker.session_map);

	update_events = worker.update_events;
	fill_input_buffer = worker.fill_input_buffer;
	flush_output_buffer = worker.flush_output_buffer;
}

Worker::~Worker()
{
	if (server_sock > 0) {
		close(server_sock);
	}

	if (epfd > 0) {
		close(epfd);
	}

	if (ssl_ctx) {
		SSL_CTX_free(ssl_ctx);
	}

	session_map.clear(); // 세션 맵 반납을 명시적으로 수행 (하지 않아도 됨)
}

void Worker::set_nonblocking_socket(int& sock) const
{
	int flag = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flag|O_NONBLOCK);
}

void Worker::enable_address_and_port_reuse(int& sock) const
{
	if (sock > 0) {
		int opt = true;
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt)) < 0) {
			std::cout << "setsockopt(REUSEADDR) error: " << strerror(errno) << std::endl;
		}

		if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
			std::cerr << "setsockopt(SO_REUSEPORT) error: " << strerror(errno) << std::endl;
		}
	}
}

int Worker::create_listening_socket(const uint16_t port)
{
	int server_sock = socket(PF_INET, SOCK_STREAM, 0);
	if (server_sock > 0) {
		memset(&server_addr, 0, sizeof(struct sockaddr_in));
		server_addr.sin_family = AF_INET;
		server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		server_addr.sin_port = htons(port);

		enable_address_and_port_reuse(server_sock);
		set_nonblocking_socket(server_sock);

		if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)) == -1) {
			std::cout << "bind() error: " << strerror(errno) << std::endl;
			throw -1;
		}
	}
	return server_sock;
}

int Worker::create_epoll(size_t epoll_size)
{
	int epfd = epoll_create(epoll_size);
	if (epfd <= 0) {
		std::cout << "epoll_create() error: " << strerror(errno) << std::endl;
		throw -1;
	}
	return epfd;
}

void Worker::bind_callbacks_for_mode(bool use_tls)
{
	if (use_tls) {
		update_events = [this](int sock, std::shared_ptr<SessionData> session_data) {
			return this->update_events_tls(sock, session_data);
		};
		fill_input_buffer = [this](int sock, std::shared_ptr<SessionData> session_data) {
			return this->fill_input_buffer_tls(sock, session_data);
		};
		flush_output_buffer = [this](int sock, std::shared_ptr<SessionData> session_data) {
			return this->flush_output_buffer_tls(sock, session_data);
		};
	} else {
		update_events = [this](int sock, std::shared_ptr<SessionData> session_data) {
			return this->update_events_h2c(sock, session_data);
		};
		fill_input_buffer = [this](int sock, std::shared_ptr<SessionData> session_data) {
			return this->fill_input_buffer_h2c(sock, session_data);
		};
		flush_output_buffer = [this](int sock, std::shared_ptr<SessionData> session_data) {
			return this->flush_output_buffer_h2c(sock, session_data);
		};
	}

	this->use_tls = use_tls;
}

bool Worker::check_close_event(uint32_t events)
{
	return events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

void Worker::set_event(int sock, uint32_t events)
{
	struct epoll_event ev;
	ev.events = events | EPOLLET | EPOLLRDHUP;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev) < 0) {
		if (errno == ENOENT) {
			epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
		}
	}
}

void Worker::update_event(int sock, uint32_t events, std::shared_ptr<SessionData> session_data)
{
	if (session_data->events == events) {
		return;
	}

	session_data->events = events;
	set_event(sock, events);
}

void Worker::update_events_h2c(int sock, std::shared_ptr<SessionData> session_data)
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

	update_event(sock, events, session_data);
}

void Worker::update_events_tls(int sock, std::shared_ptr<SessionData> session_data)
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

	update_event(sock, events, session_data);
}

void Worker::update_ssl_handshake_events(int sock, std::shared_ptr<SessionData> session_data)
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

	update_event(sock, events, session_data);
}

bool Worker::should_disconnect(SessionState state)
{
	return state == SessionState::DISCONNECTING;
}

bool Worker::should_close_after_disconnect(const std::shared_ptr<SessionData>& session_data)
{
	if (session_data->state != SessionState::DISCONNECTING)  {
		return false;
	}

	return !nghttp2_session_want_write(session_data->session) && // DISCONNECTING 단계에 진입 했으면 read는 더이상 중요치 않음, write 만 신경 쓰면 됨
			session_data->output_buffer.empty();
}

void Worker::disconnect_from_client(int sock, std::shared_ptr<SessionData> session_data)
{
	if (session_data) {
		if (use_tls && session_data->ssl) {
			SSL_set_quiet_shutdown(session_data->ssl, 1);
			session_data->close_session();
		}

		epoll_ctl(epfd, EPOLL_CTL_DEL, sock, NULL);
		close(sock);
		session_map.erase(sock);
	}
}

int Worker::send_server_connection_header(std::shared_ptr<SessionData> session_data)
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

SessionState Worker::establish_connection(int sock, std::shared_ptr<SessionData> session_data)
{
	if (send_server_connection_header(session_data) != 0) {
		std::cerr << "send_server_connection() error" << std::endl;
		return SessionState::DISCONNECTING;
	}
	return SessionState::ESTABLISHED;
}

SessionState Worker::do_tls_handshake(int sock, std::shared_ptr<SessionData> session_data)
{
	SSL* ssl = session_data->ssl;

	int ret = SSL_accept(ssl);
	if (ret > 0) {
		if (validate_alpn(session_data)) {

#if 0 // Session Resumption 동작 여부 확인을 위한 구간
			int reused = SSL_session_reused(ssl);
			const char* ver = SSL_get_version(ssl);
			std::cout << "TLS ver: " << ver << ", session_reused=" << reused << std::endl;
#endif
			return SessionState::ESTABLISHING;
		}
		return SessionState::DISCONNECTING;
	} else {
		int ssl_err = SSL_get_error(ssl, ret);
		if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
			update_ssl_handshake_events(sock, session_data);
			return SessionState::TLS_HANDSHAKING;
		} else {
			std::cerr << "unexpected SSL error: " << ERR_error_string(ssl_err, nullptr) << std::endl;
			return SessionState::DISCONNECTING;
		}
	}
}

void Worker::handle_tls_handshake(int sock, std::shared_ptr<SessionData> session_data)
{
	SessionState session_state = do_tls_handshake(sock, session_data);
	if (should_disconnect(session_state)) {
		disconnect_from_client(sock, session_data);
		return;
	}

	if (session_state == SessionState::TLS_HANDSHAKING) {
		return;
	}

	session_state = establish_connection(sock, session_data);
	if (should_disconnect(session_state)) {
		disconnect_from_client(sock, session_data);
		return;
	}

	session_data->state = session_state;
	update_events(sock, session_data);
}

bool Worker::handle_tls_accept(int sock, std::shared_ptr<SessionData> session_data)
{
	SSL* ssl = SSL_new(ssl_ctx);
	SSL_set_fd(ssl, sock);
	session_data->ssl = ssl;

	SessionState session_state = do_tls_handshake(sock, session_data);
	if (session_state == SessionState::DISCONNECTING) {
		disconnect_from_client(sock, session_data);
		return false;
	} else if (session_state == SessionState::TLS_HANDSHAKING) {
		session_data->state = session_state;
		return false;
	}
	return true;
}

void Worker::handle_new_connections(int clnt_sock)
{
	// http2 session 생성
	std::shared_ptr<SessionData> session_data = std::make_shared<SessionData>();
	if (init_session_data(session_data) != 0) {
		close(clnt_sock);
		return;
	}
	session_data->router = router;
	session_map.emplace(clnt_sock, session_data);

	if (use_tls) {
		if (!handle_tls_accept(clnt_sock, session_data)) {
			return;
		}
	}

	SessionState session_state = establish_connection(clnt_sock, session_data);
	if (session_state == SessionState::DISCONNECTING) {
		disconnect_from_client(clnt_sock, session_data);
		return;
	}

	session_data->state = session_state;
	update_events(clnt_sock, session_data);
}

void Worker::handle_accept(int sock)
{
	while (true) { // 일시 실패 시 재시도 용도의 루프
		struct sockaddr_in client_addr;
		socklen_t client_addr_size = sizeof(client_addr);

		int clnt_sock = accept4(server_sock, (struct sockaddr*)&client_addr, &client_addr_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (clnt_sock < 0) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				//std::cerr << "accept() error : " << strerror(errno) << std::endl;
			} else if (errno == EMFILE || errno == ENFILE) {
				std::cerr << "accept() error : " << strerror(errno) << std::endl;
			}
			break;
		} else {
			//std::cout << "client[" << clnt_sock << "] connected ..." << std::endl;
			handle_new_connections(clnt_sock);
		}
	}
}

void Worker::handle_events(uint32_t ev, int sock, std::shared_ptr<SessionData> session_data)
{
	IOResult result = IOResult::SUCCESS;

	// FIN 만 수신된 경우(SSL_read로 확인이 어려운 FIN 만 오는 경우 EPOLLRDHUP 이벤트로 확인)
	bool has_close_event = check_close_event(ev);
	if ((ev & EPOLLIN) || has_close_event) {
		result = handle_read(sock, session_data); // 소켓 수신 버퍼에 있는 데이터 처리 : 데이터 유실 방지
	}

	if (result == IOResult::SUCCESS) {
		if ((ev & EPOLLOUT) || !session_data->output_buffer.empty() || nghttp2_session_want_write(session_data->session)) {
			result = handle_write(sock, session_data);
		}
	}

	if (result == IOResult::SHUTDOWN || has_close_event) {
		disconnect_from_client(sock, session_data);
	}
}

IOResult Worker::handle_read(int sock, std::shared_ptr<SessionData> session_data)
{
	IOResult input_result = fill_input_buffer(sock, session_data);
	if (input_result == IOResult::SHUTDOWN) {
		return IOResult::SHUTDOWN;
	}

	IOResult feed_result = feed_input_buffer(session_data);
	if (feed_result == IOResult::SHUTDOWN) {
		return IOResult::SHUTDOWN;
	}

	update_events(sock, session_data);
	if (should_close_after_disconnect(session_data)) {
		return IOResult::SHUTDOWN;
	}

	return IOResult::SUCCESS;
}

IOResult Worker::handle_write(int sock, std::shared_ptr<SessionData> session_data)
{
	fill_output_buffer(session_data);
	IOResult result = flush_output_buffer(sock, session_data);
	if (result == IOResult::SHUTDOWN) {
		return IOResult::SHUTDOWN;
	}

	update_events(sock, session_data);
	if (should_close_after_disconnect(session_data)) {
		return IOResult::SHUTDOWN;
	}

	return result;
}

IOResult Worker::fill_input_buffer_h2c(int sock, std::shared_ptr<SessionData> session_data)
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
#ifdef DEBUG
            std::cerr << "read() error: " << strerror(errno) << std::endl;
#endif
            return IOResult::SHUTDOWN;
        }
        session_data->append_to_input_buffer(buffer, read_len);
    }
}

IOResult Worker::fill_input_buffer_tls(int sock, std::shared_ptr<SessionData> session_data)
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

IOResult Worker::feed_input_buffer(std::shared_ptr<SessionData> session_data)
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

void Worker::fill_output_buffer(std::shared_ptr<SessionData> session_data)
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

IOResult Worker::flush_output_buffer_h2c(int sock, std::shared_ptr<SessionData> session_data)
{
    while (!session_data->output_buffer.empty()) {
        ssize_t written_len = write(sock, session_data->output_buffer.data(), session_data->output_buffer.size());
        if (written_len < 0) {
            if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) { // 현재 처리 할 데이터가 없으므로 다음 EPOLLOUT 때 처리
				return IOResult::AGAIN;
			}
#ifdef DEBUG
            std::cerr << "write() error: " << strerror(errno) << std::endl;
#endif
            return IOResult::SHUTDOWN; // ex: EPIPE
        }
        session_data->consume_output_buffer(written_len);
    }
    return IOResult::SUCCESS;
}

IOResult Worker::flush_output_buffer_tls(int sock, std::shared_ptr<SessionData> session_data)
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

bool Worker::validate_alpn(std::shared_ptr<SessionData> session_data)
{
	const unsigned char* alpn_proto = nullptr;
	unsigned int alpn_proto_len = 0;

	SSL_get0_alpn_selected(session_data->ssl, &alpn_proto, &alpn_proto_len);
	if (alpn_proto == NULL || alpn_proto_len != 2 || memcmp("h2", alpn_proto, 2) != 0) {
		std::cerr << "alpn protocol is not h2" << std::endl;
		return false;
	}
	return true;
}

bool Worker::startup(const uint16_t port)
{
	epfd = create_epoll(EPOLL_SIZE);
	if (epfd < 0) {
		std::cerr << "create_epoll failed" << std::endl;
		return false;
	}

	server_sock = create_listening_socket(port);
	if (server_sock < 0) {
		std::cerr << "create_listening_socket failed" << std::endl;
		return false;
	}

	if (listen(server_sock, SOMAXCONN) < 0) {
		std::cout << "listen failed" << std::endl;
		return false;
	}

	set_event(server_sock, EPOLLIN);
	return true;
}

void Worker::run(const uint16_t server_port, std::string_view key_path, std::string_view cert_path)
{
	if (!startup(server_port)) {
		std::cerr << "worker startup failed" << std::endl;
		return;
	}

	if (use_tls) {
		SSL_load_error_strings();
		OpenSSL_add_ssl_algorithms();
		ssl_ctx = create_ssl_ctx(key_path, cert_path);
	}

	struct epoll_event events[EPOLL_SIZE];
	while (true) {
		int event_count = epoll_wait(epfd, events, EPOLL_SIZE, -1);
		if (event_count < 0) {
			if (errno == EINTR) {
				continue;
			}
			std::cerr << "epoll_wait() error" << strerror(errno) << std::endl;
			break;
		}

		for (int i = 0; i < event_count; i++) {
			if (events[i].data.fd == server_sock) {
				handle_accept(events[i].data.fd);
			} else {
				uint32_t ev = events[i].events;
				int clnt_sock = events[i].data.fd;
				auto iter = session_map.find(clnt_sock);
				if (iter != session_map.end()) {
					std::shared_ptr<SessionData> session_data = iter->second;
					if (!iter->second) {
						std::cerr << "session_data has nullptr" << std::endl;
						continue;
					}

					// TLS handshake 진행 중인 경우
					if (session_data->state == SessionState::TLS_HANDSHAKING) {
						handle_tls_handshake(clnt_sock, session_data);
						continue;
					}

					handle_events(ev, clnt_sock, session_data);
				} else {
					std::cerr << "session_data doesn't exist in session map" << std::endl;
				}
			}
		}
	}
}
