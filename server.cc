#include "server.h"
#include "ssl_ctx.h"
#include "file_cache.h"


const int EPOLL_SIZE = 1024;
const int BUF_SIZE = 2048;

Server::Server(bool use_tls = true)
	: epfd(-1), server_sock(-1), ep_events(nullptr),
	check_rd_hup(nullptr), update_events(nullptr),
	fill_input_buffer(nullptr), flush_output_buffer(nullptr),
	ssl_ctx(nullptr), use_tls(use_tls)
{
	memset(&addr , 0, sizeof(struct sockaddr_in));
	set_mode(use_tls);
}

Server::~Server()
{
	if (server_sock > 0) {
		close(server_sock);
	}

	if (epfd > 0) {
		close(epfd);
	}

	if (ep_events) {
		delete []ep_events;
	}

	if (ssl_ctx) {
		SSL_CTX_free(ssl_ctx);
	}

	session_map.clear(); // 세션 맵 반납을 명시적으로 수행 (하지 않아도 됨)
}

void Server::set_mode(bool use_tls)
{
	if (use_tls) {
		check_rd_hup = [](uint32_t ev) {
			return ev & EPOLLRDHUP;
		};
		update_events = [this](int sock, std::shared_ptr<http2_session_data_t> session_data) {
			return this->update_events_tls(sock, session_data);
		};
		fill_input_buffer = [this](int sock, std::shared_ptr<http2_session_data_t> session_data) {
			return this->fill_input_buffer_tls(sock, session_data);
		};
		flush_output_buffer = [this](int sock, std::shared_ptr<http2_session_data_t> session_data) {
			return this->flush_output_buffer_tls(sock, session_data);
		};
	} else {
		check_rd_hup = [](uint32_t ev) {
			return false;
		};
		update_events = [this](int sock, std::shared_ptr<http2_session_data_t> session_data) {
			return this->update_events_h2c(sock, session_data);
		};
		fill_input_buffer = [this](int sock, std::shared_ptr<http2_session_data_t> session_data) {
			return this->fill_input_buffer_h2c(sock, session_data);
		};
		flush_output_buffer = [this](int sock, std::shared_ptr<http2_session_data_t> session_data) {
			return this->flush_output_buffer_h2c(sock, session_data);
		};
	}

	this->use_tls = use_tls;
}

void Server::setReuseSocket(int& server_sock) const
{
	if (server_sock > 0) {
		int opt = true;
		if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt)) < 0) {
			std::cout << "setsockopt() error: " << strerror(errno) << std::endl;
		}
	}
}

void Server::setNonBlockingSocket(int& sock) const
{
	int flag = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flag|O_NONBLOCK);
}

int Server::createListeningSocket(struct sockaddr_in& server_addr, uint16_t server_port)
{
	int server_sock = socket(PF_INET, SOCK_STREAM, 0);
	if (server_sock > 0) {
		memset(&server_addr, 0, sizeof(struct sockaddr_in));
		server_addr.sin_family = AF_INET;
		server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		server_addr.sin_port = htons(server_port);

		setReuseSocket(server_sock);
		setNonBlockingSocket(server_sock);

		if (bind(server_sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) == -1) {
			std::cout << "bind() error: " << strerror(errno) << std::endl;
			throw -1;
		}
	}

	return server_sock;
}

int Server::createEPOLL(int server_sock, size_t epoll_size)
{
	int epfd = epoll_create(EPOLL_SIZE);
	if (epfd <= 0) {
		std::cout << "epoll_create() error: " << strerror(errno) << std::endl;
		throw -1;
	}

	return epfd;
}

struct epoll_event* Server::createEventBucket(size_t size)
{
	struct epoll_event* ep_events = new struct epoll_event[EPOLL_SIZE];
	if (!ep_events) {
		std::cout << "allocation to ep_events error" << std::endl;
		throw -1;
	}

	return ep_events;
}

void Server::startUp(uint16_t port)
{
	try {
		server_sock = createListeningSocket(addr, port);
		if (listen(server_sock, 100)==-1) {
			std::cout << "listen() error" << std::endl;
			throw -1;
		}

		epfd = createEPOLL(server_sock, EPOLL_SIZE);
		set_event(server_sock, EPOLLIN);
		ep_events = createEventBucket(EPOLL_SIZE);
	} catch (int execept_code) {
		std::cout << "program abort due to exception" << std::endl;
		exit(0);
	}
}


void Server::set_event(int sock, uint32_t events)
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

void Server::update_event(int sock, uint32_t events, std::shared_ptr<http2_session_data_t> session_data)
{
	if (session_data->events == events) {
		return;
	}

	session_data->events = events;
	set_event(sock, events);
}

void Server::update_events_h2c(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	uint32_t events = 0;

	if (nghttp2_session_want_read(session_data->session)) {
		events |= EPOLLIN;
	}

	if (nghttp2_session_want_write(session_data->session) ||
		!session_data->output_buffer.empty()) {
		events |= EPOLLOUT;
	}

	if (!events) events = EPOLLIN; // default로 하지 않는 것은 EPOLLIN만 무조건 등록이 유지되어 부하가 생기는 것을 방지

	update_event(sock, events, session_data);
}

void Server::update_events_tls(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	uint32_t events = 0;

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

	update_event(sock, events, session_data);
}

void Server::update_ssl_handshake_events(int sock, std::shared_ptr<http2_session_data_t> session_data)
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


void Server::disconnect_from_client(int sock)
{
	epoll_ctl(epfd, EPOLL_CTL_DEL, sock, NULL);
	close(sock);
	session_map.erase(sock);
}

void Server::disconnect_from_client_if_done(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	if (session_data->state == SessionState::DISCONNECTING &&
		!nghttp2_session_want_read(session_data->session) &&
		!nghttp2_session_want_write(session_data->session) &&
		session_data->output_buffer.empty()) {
		disconnect_from_client(sock);
	}
}

IOResult Server::fill_input_buffer_h2c(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
    unsigned char buffer[BUF_SIZE];

    while (1) {
        ssize_t read_len = read(sock, buffer, BUF_SIZE);
        if (read_len == 0) {
            return IOResult::CLOSED;
        } else if (read_len < 0) {
            if (errno == EINTR) { // 인터럽트 시그널로 인한 read 반환
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
                return IOResult::AGAIN;
            }

            std::cerr << "read() error: " << strerror(errno) << std::endl;
            return IOResult::ERROR;
        }

        session_data->append_to_input_buffer(buffer, read_len);
    }

    return IOResult::SUCCESS;
}

IOResult Server::fill_input_buffer_tls(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	unsigned char buffer[BUF_SIZE];
	SSL* ssl = session_data->ssl;

	while (true) {
		int ret = SSL_read(ssl, buffer, BUF_SIZE);
		if (ret < 0) {
			int err = SSL_get_error(ssl, ret);
			switch (err) {
				case SSL_ERROR_WANT_READ:
				case SSL_ERROR_WANT_WRITE:
					return IOResult::AGAIN;
				case SSL_ERROR_SYSCALL:
					if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
						return IOResult::AGAIN;
					} 
					return IOResult::ERROR;
				case SSL_ERROR_ZERO_RETURN:
					return IOResult::CLOSED;
				default:
					std::cerr << "SSL_read() error: " << strerror(errno) << std::endl;
					return IOResult::ERROR;
			}
		}

		session_data->append_to_input_buffer(buffer, ret);
		if (ret < BUF_SIZE) {
			break;
		}
	}

	return IOResult::SUCCESS;
}

IOResult Server::feed_input_buffer(std::shared_ptr<http2_session_data_t> session_data)
{
	while (!session_data->input_buffer.empty()) {
		nghttp2_ssize fed_len = nghttp2_session_mem_recv2(
				session_data->session,
				session_data->input_buffer.data(),
				session_data->input_buffer.size());
		if (fed_len < 0) {
			std::cerr << "nghttp2_session_mem_recv2() error: " << nghttp2_strerror((int)fed_len) << std::endl;
			return IOResult::ERROR;
		}

		session_data->consume_input_buffer(fed_len);
	}

	return IOResult::SUCCESS;
}

void Server::handle_read(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	IOResult input_result = fill_input_buffer(sock, session_data);
	if (input_result == IOResult::CLOSED || input_result == IOResult::ERROR) {
		disconnect_from_client(sock);
		return;
	}

	IOResult feed_result = feed_input_buffer(session_data);
	if (feed_result == IOResult::ERROR) {
		disconnect_from_client(sock);
		return;
	}

	update_events(sock, session_data);
	disconnect_from_client_if_done(sock, session_data);
}

void Server::fill_output_buffer(std::shared_ptr<http2_session_data_t> session_data)
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

IOResult Server::flush_output_buffer_h2c(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
    while (!session_data->output_buffer.empty()) {
        ssize_t written_len = write(sock, session_data->output_buffer.data(), session_data->output_buffer.size());
        if (written_len < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return IOResult::AGAIN; // 현재 처리 할 데이터가 없으므로 다음 EPOLLOUT 때 처리

            std::cerr << "write() error: " << strerror(errno) << std::endl;
            return IOResult::ERROR;
        }
        session_data->consume_output_buffer(written_len);
    }

    return IOResult::SUCCESS;
}

IOResult Server::flush_output_buffer_tls(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	SSL* ssl = session_data->ssl;

	while (!session_data->output_buffer.empty()) {
		int ret = SSL_write(ssl, session_data->output_buffer.data(), session_data->output_buffer.size());
		if (ret < 0) {
			int err = SSL_get_error(ssl, ret);
			switch (err) {
				case SSL_ERROR_WANT_READ:
				case SSL_ERROR_WANT_WRITE:
					return IOResult::AGAIN;
				case SSL_ERROR_SYSCALL:
					if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
						return IOResult::AGAIN;
					}
					return IOResult::ERROR;
				default:
					std::cerr << "SSL_write() error: " << strerror(errno) << std::endl;
					return IOResult::ERROR;
			}
		}
		session_data->consume_output_buffer(ret);
	}
	return IOResult::SUCCESS;
}

void Server::handle_write(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	fill_output_buffer(session_data);
	IOResult flush_result = flush_output_buffer(sock, session_data);
	if (flush_result == IOResult::ERROR) {
		disconnect_from_client(sock);
		return;
	}

	update_events(sock, session_data);
	disconnect_from_client_if_done(sock, session_data);
}

bool Server::validate_alpn(std::shared_ptr<http2_session_data_t> session_data)
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

SessionState Server::do_tls_handshake(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	SSL* ssl = session_data->ssl;

	int ret = SSL_accept(ssl);
	if (ret > 0) {
		if (validate_alpn(session_data)) {
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

int Server::send_server_connection_header(std::shared_ptr<http2_session_data_t> session_data)
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

SessionState Server::establish_connection(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	if (send_server_connection_header(session_data) != 0) {
		std::cerr << "send_server_connection() error" << std::endl;
		return SessionState::DISCONNECTING;
	}

	return SessionState::ESTABLISHED;
}

void Server::handle_tls_handshake(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	SessionState session_state = do_tls_handshake(sock, session_data);
	if (session_state == SessionState::DISCONNECTING) {
		disconnect_from_client(sock);
		return;
	} else if (session_state == SessionState::TLS_HANDSHAKING) {
		return;
	}

	session_state = establish_connection(sock, session_data);
	if (session_state == SessionState::DISCONNECTING) {
		disconnect_from_client(sock);
		return;
	}

	session_data->state = session_state;
	update_events(sock, session_data);
}

bool Server::handle_tls_accept(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	SSL* ssl = SSL_new(ssl_ctx);
	SSL_set_fd(ssl, sock);
	session_data->ssl = ssl;

	SessionState session_state = do_tls_handshake(sock, session_data);
	if (session_state == SessionState::DISCONNECTING) {
		disconnect_from_client(sock);
		return false;
	} else if (session_state == SessionState::TLS_HANDSHAKING) {
		session_data->state = session_state;
		return false;
	}

	return true;
}

void Server::handle_accept()
{
	struct sockaddr_in client_addr;
	socklen_t client_addr_size = sizeof(client_addr);
	int clnt_sock = -1;

	while (true) {
		clnt_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_addr_size);
		if (clnt_sock < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				//std::cerr << "accept() :  EWOULDBLOCK()" << std::endl;
			} else {
				std::cout << "accept() error : " << strerror(errno) << std::endl;
			}
			break;
		} else {
			setNonBlockingSocket(clnt_sock);
			//std::cout << "client[" << clnt_sock << "] connected ..." << std::endl;

			// http2 session 생성
			std::shared_ptr<http2_session_data_t> session_data = std::make_shared<http2_session_data_t>();
			if (init_http2_session_data(session_data) != 0) {
				close(clnt_sock);
				continue;
			}
			session_map.emplace(clnt_sock, session_data);

			if (use_tls) {
				if (!handle_tls_accept(clnt_sock, session_data)) {
					continue;
				}
			}

			SessionState session_state = establish_connection(clnt_sock, session_data);
			if (session_state == SessionState::DISCONNECTING) {
				disconnect_from_client(clnt_sock);
				continue;
			}

			session_data->state = session_state;
			update_events(clnt_sock, session_data);
		}
	}
}
																			
void Server::listen_and_serve(const uint16_t port, const std::string& key_path, const std::string& cert_path)
{
	struct epoll_event event;

	if (use_tls) {
		SSL_load_error_strings();
		OpenSSL_add_ssl_algorithms();
		ssl_ctx = create_ssl_ctx(key_path, cert_path);
	}

	startUp(port);
	while (!isShutdown()) {
		uint32_t event_count = epoll_wait(epfd, ep_events, EPOLL_SIZE, -1);
		if (event_count < 0) {
			std::cout << "epoll_wait() error" << std::endl;
			break;
		}

		for (uint32_t i = 0; i < event_count; i++) {
			if (ep_events[i].data.fd == server_sock) {
				handle_accept();
			} else {
				uint32_t ev = ep_events[i].events;
				int clnt_sock = ep_events[i].data.fd;

				auto iter = session_map.find(clnt_sock);
				if (iter != session_map.end()) {
					std::shared_ptr<http2_session_data_t> session_data = iter->second;
					if (!iter->second) {
						std::cout << "session_data doesn't exist" << std::endl;
						continue;
					}

					if (session_data->state == SessionState::TLS_HANDSHAKING) {
						handle_tls_handshake(clnt_sock, session_data);
						continue;
					}

					if (check_rd_hup(ev)) { // SSL_read로 확인이 어려운 FIN만 오는 경우 EPOLLRDHUP 이벤트로 확인
						disconnect_from_client(clnt_sock);
						continue;
					}

					if (ev & EPOLLIN) {
						handle_read(clnt_sock, session_data);
					}

					if (ev & EPOLLOUT) {
						handle_write(clnt_sock, session_data);
					}
				} else {
					std::cout << "session_data doesn't exist" << std::endl;
				}
			}
		}
	}
}
