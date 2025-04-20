#include <nghttp2/nghttp2.h>
#include <unordered_map>
#include <list>
#include <vector> 

#include "server.h"
#include "http2_session.h"
#include "file_context.h"
//#include "mapped_file.h"
#include "ssl_ctx.h"

const int EPOLL_SIZE = 1024;
const int BUF_SIZE = 2048;

enum class IOResult {
	SUCCESS,
	AGAIN,
	CLOSED,
	ERROR
};


SSL_CTX* g_ssl_ctx;
std::unordered_map<int, std::shared_ptr<http2_session_data_t>> session_map; // 나중에 shared_ptr로 바꿀 것

void update_event2(int epfd, int sock, uint32_t events)
{
	struct epoll_event ev;
	ev.events = events | EPOLLET;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev) < 0) {
		if (errno == ENOENT) {
			epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
		}
	}
}

void update_event(int epfd, int sock, uint32_t events, std::shared_ptr<http2_session_data_t> session_data)
{
	if (session_data->events == events) {
		return;
	}

	session_data->events = events;

	struct epoll_event ev;
	ev.events = events | EPOLLET | EPOLLRDHUP;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev) < 0) {
		if (errno == ENOENT) {
			epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
		}
	}
}

static void update_events(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
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

	update_event(epfd, sock, events, session_data);
}

static void update_ssl_handshake_events(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
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


static void disconnect_from_client(int epfd, int sock)
{
	epoll_ctl(epfd, EPOLL_CTL_DEL, sock, NULL);
	close(sock);
	session_map.erase(sock);
}

static int send_server_connection_header(std::shared_ptr<http2_session_data_t> session_data)
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

static void disconnect_from_client_if_done(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	if (session_data->state == SessionState::DISCONNECTING &&
		!nghttp2_session_want_read(session_data->session) &&
		!nghttp2_session_want_write(session_data->session) &&
		session_data->output_buffer.empty()) {
		disconnect_from_client(epfd, sock);
	}
}

static IOResult fill_input_buffer(int sock, std::shared_ptr<http2_session_data_t> session_data)
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

static IOResult feed_input_buffer(std::shared_ptr<http2_session_data_t> session_data)
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

static void handle_read(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	IOResult input_result = fill_input_buffer(sock, session_data);
	if (input_result == IOResult::CLOSED || input_result == IOResult::ERROR) {
		disconnect_from_client(epfd, sock);
		return;
	}

	IOResult feed_result = feed_input_buffer(session_data);
	if (feed_result == IOResult::ERROR) {
		disconnect_from_client(epfd, sock);
		return;
	}

	update_events(epfd, sock, session_data);
	disconnect_from_client_if_done(epfd, sock, session_data);
}

static void fill_output_buffer(std::shared_ptr<http2_session_data_t> session_data)
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

static IOResult flush_output_buffer(int sock, std::shared_ptr<http2_session_data_t> session_data)
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

static void handle_write(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	fill_output_buffer(session_data);
	IOResult flush_result = flush_output_buffer(sock, session_data);
	if (flush_result == IOResult::ERROR) {
		disconnect_from_client(epfd, sock);
		return;
	}

	update_events(epfd, sock, session_data);
	disconnect_from_client_if_done(epfd, sock, session_data);
}

static bool validate_alpn(std::shared_ptr<http2_session_data_t> session_data)
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

SessionState do_tls_handshake(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
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
			update_ssl_handshake_events(epfd, sock, session_data);
			return SessionState::TLS_HANDSHAKING;
		} else {
			std::cerr << "unexpected SSL error: " << ERR_error_string(ssl_err, nullptr) << std::endl;
			return SessionState::DISCONNECTING;
		}
	}
}

SessionState establish_connection(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	if (send_server_connection_header(session_data) != 0) {
		std::cerr << "send_server_connection() error" << std::endl;
		return SessionState::DISCONNECTING;
	}

	return SessionState::ESTABLISHED;
}

void handle_tls_handshake(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	SessionState session_state = do_tls_handshake(epfd, sock, session_data);
	if (session_state == SessionState::DISCONNECTING) {
		disconnect_from_client(epfd, sock);
		return;
	} else if (session_state == SessionState::TLS_HANDSHAKING) {
		return;
	}

	session_state = establish_connection(epfd, sock, session_data);
	if (session_state == SessionState::DISCONNECTING) {
		disconnect_from_client(epfd, sock);
		return;
	}

	session_data->state = session_state;
	update_events(epfd, sock, session_data);
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

			SSL* ssl = SSL_new(g_ssl_ctx);
			SSL_set_fd(ssl, clnt_sock);
			session_data->ssl = ssl;

			SessionState session_state = do_tls_handshake(epfd, clnt_sock, session_data);
			if (session_state == SessionState::DISCONNECTING) {
				disconnect_from_client(epfd, clnt_sock);
				continue;
			} else if (session_state == SessionState::TLS_HANDSHAKING) {
				session_data->state = session_state;
				continue;
			}

			session_state = establish_connection(epfd, clnt_sock, session_data);
			if (session_state == SessionState::DISCONNECTING) {
				disconnect_from_client(epfd, clnt_sock);
				continue;
			}

			session_data->state = session_state;
			update_events(epfd, clnt_sock, session_data);
		}
	}
}

Server::Server() : epfd(-1), server_sock(-1), ep_events(nullptr)
{
	memset(&addr , 0, sizeof(struct sockaddr_in));
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
		update_event2(epfd, server_sock, EPOLLIN);
		ep_events = createEventBucket(EPOLL_SIZE);
	} catch (int execept_code) {
		std::cout << "program abort due to exception" << std::endl;
		exit(0);
	}
}

void Server::listen_and_serve(const uint16_t port, const char* key_path, const char* crt_path)
{
	startUp(port);

    SSL_load_error_strings();
	OpenSSL_add_ssl_algorithms();

	g_ssl_ctx = create_ssl_ctx(key_path, crt_path);

	struct epoll_event event;

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
						handle_tls_handshake(epfd, clnt_sock, session_data);
						continue;
					}

					if (ev & EPOLLRDHUP) { // SSL_read로 확인이 어려운 FIN만 오는 경우 확인
						disconnect_from_client(epfd, clnt_sock);
						continue;
					}

					if (ev & EPOLLIN) {
						handle_read(epfd, clnt_sock, session_data);
					}

					if (ev & EPOLLOUT) {
						handle_write(epfd, clnt_sock, session_data);
					}
				} else {
					std::cout << "session_data doesn't exist" << std::endl;
				}
			}
		}
	}

	session_map.clear(); // 세션 맵 반납을 명시적으로 수행 (하지 않아도 됨)
					
	if (g_ssl_ctx) {
		SSL_CTX_free(g_ssl_ctx);
	}
}
