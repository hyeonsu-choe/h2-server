#pragma once

#include <iostream>
#include <queue>
#include <functional>

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
#include <openssl/ssl.h>

#include "session_engine.h"

struct ssl_ctx_st;
using SSL_CTX = ssl_ctx_st;

class Router;

template <typename TransportPolicy>
class WorkerBase {
	private:
		const Router* router;
		int server_sock;
		struct sockaddr_in server_addr;
		int epfd;

		typename TransportPolicy::Context worker_ctx;
		std::unordered_map<int, std::shared_ptr<SessionData>> session_map;

	private:
		void set_nonblocking_socket(int& sock) const
		{
			int flag = fcntl(sock, F_GETFL, 0);
			fcntl(sock, F_SETFL, flag|O_NONBLOCK);
		}

		void enable_address_and_port_reuse(int& sock) const
		{
			if (sock > 0) {
				int opt = true;
				if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt)) < 0) {
					std::cerr << "setsockopt(REUSEADDR) error: " << strerror(errno) << std::endl;
				}

				if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
					std::cerr << "setsockopt(SO_REUSEPORT) error: " << strerror(errno) << std::endl;
				}
			}
		}

		int create_epoll(size_t epoll_size)
		{
			int epfd = epoll_create(epoll_size);
			if (epfd <= 0) {
				std::cerr << "epoll_create() error: " << strerror(errno) << std::endl;
				throw -1;
			}

			return epfd;
		}

		int create_listening_socket(const uint16_t port)
		{
			int sock = socket(PF_INET, SOCK_STREAM, 0);
			if (sock < 0) {
				std::cerr << "socket() error: " << strerror(errno) << std::endl;
				return -1;
			}

			memset(&server_addr, 0, sizeof(struct sockaddr_in));
			server_addr.sin_family = AF_INET;
			server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
			server_addr.sin_port = htons(port);

			enable_address_and_port_reuse(sock);
			set_nonblocking_socket(sock);

			if (bind(sock, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)) == -1) {
				std::cerr << "bind() error: " << strerror(errno) << std::endl;
				close(sock);
				return -1;
			}

			if (listen(sock, SOMAXCONN) < 0) {
				close(sock);
				std::cerr << "listen failed" << std::endl;
				return -1;
			}

			return sock;
		}

		void set_event(int sock, uint32_t events)
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

		bool check_close_event(uint32_t events) const
		{
			return events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP);
		}

		bool establish_connection(int sock, const std::shared_ptr<SessionData>& session_data)
		{
			if (SessionEngine::send_server_connection_header(session_data) != 0) {
				std::cerr << "send_server_connection() error" << std::endl;
				return false;
			}
			session_data->state = SessionState::ESTABLISHED;
			TransportPolicy::update_events(epfd, sock, session_data);
			return true;
		}

		void disconnect_from_client(int sock, std::shared_ptr<SessionData> session_data)
		{
			if (session_data) {
				TransportPolicy::disconnect(worker_ctx, session_data);

				epoll_ctl(epfd, EPOLL_CTL_DEL, sock, NULL);
				close(sock);
				session_map.erase(sock);
			}
		}

		IOResult handle_read(int sock, const std::shared_ptr<SessionData>& session_data)
		{
			IOResult input_result = TransportPolicy::fill_input_buffer(sock, session_data);
			if (input_result == IOResult::SHUTDOWN) {
				return IOResult::SHUTDOWN;
			}

			IOResult feed_result = SessionEngine::feed_input_buffer(session_data);
			if (feed_result == IOResult::SHUTDOWN) {
				return IOResult::SHUTDOWN;
			}

			TransportPolicy::update_events(epfd, sock, session_data);
			if (SessionEngine::should_close_after_disconnect(session_data)) {
				return IOResult::SHUTDOWN;
			}

			return IOResult::SUCCESS;
		}

		IOResult handle_write(int sock, const std::shared_ptr<SessionData>& session_data)
		{
			SessionEngine::fill_output_buffer(session_data);
			IOResult result = TransportPolicy::flush_output_buffer(sock, session_data);
			if (result == IOResult::SHUTDOWN) {
				return IOResult::SHUTDOWN;
			}

			TransportPolicy::update_events(epfd, sock, session_data);
			if (SessionEngine::should_close_after_disconnect(session_data)) {
				return IOResult::SHUTDOWN;
			}

			return result;
		}

		void handle_events(uint32_t ev, int sock, const std::shared_ptr<SessionData>& session_data)
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

		void handle_new_connections(int clnt_sock)
		{
			// http2 session 생성
			std::shared_ptr<SessionData> session_data = std::make_shared<SessionData>();
			if (init_session_data(session_data) != 0) {
				close(clnt_sock);
				return;
			}
			session_data->router = router;
			session_map.emplace(clnt_sock, session_data);

			if (!TransportPolicy::on_accept(worker_ctx, clnt_sock, session_data)) {
				disconnect_from_client(clnt_sock, session_data);
				return;
			}

			if constexpr (TransportPolicy::use_tls) {
				if (!TransportPolicy::do_handshake(epfd, clnt_sock, session_data)) {
					if (session_data->state == SessionState::TLS_HANDSHAKING) {
						return;
					}
					disconnect_from_client(clnt_sock, session_data);
					return;
				}
			}

			if (!establish_connection(clnt_sock, session_data)) {
				disconnect_from_client(clnt_sock, session_data);
			}
		}

		void handle_accept()
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
				}

				//std::cout << "client[" << clnt_sock << "] connected ..." << std::endl;
				handle_new_connections(clnt_sock);
			}
		}

		bool startup(const uint16_t port)
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

			set_event(server_sock, EPOLLIN);
			return true;
		}

	public:
		WorkerBase(const Router* router) : server_sock(-1), epfd(-1), router(router)
		{

		}

		~WorkerBase()
		{
			if (server_sock > 0) {
				close(server_sock);
			}

			if (epfd > 0) {
				close(epfd);
			}

			TransportPolicy::deinit_context(worker_ctx);
			session_map.clear(); // 세션 맵 반납을 명시적으로 수행 (하지 않아도 됨)
		}

		WorkerBase(const WorkerBase&) = delete;
		WorkerBase& operator=(const WorkerBase&) = delete;

		void run(const uint16_t server_port, std::string_view key_path, std::string_view cert_path)
		{
			if (!TransportPolicy::init_context(worker_ctx, key_path, cert_path)) {
				std::cerr << "worker_ctx init failed" << std::endl;
				return;
			}

			if (!startup(server_port)) {
				std::cerr << "worker startup failed" << std::endl;
				return;
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
						handle_accept();
						continue;
					}

					uint32_t ev = events[i].events;
					int clnt_sock = events[i].data.fd;
					auto iter = session_map.find(clnt_sock);
					if (iter == session_map.end() || !iter->second) {
						std::cerr << "session_data has nullptr" << std::endl;
						continue;
					}

					std::shared_ptr<SessionData> session_data = iter->second;
					if constexpr (TransportPolicy::use_tls) {
						// TLS handshake 진행 중인 경우
						if (session_data->state == SessionState::TLS_HANDSHAKING) {
							if (!TransportPolicy::do_handshake(epfd, clnt_sock, session_data)) {
								if (session_data->state == SessionState::TLS_HANDSHAKING) {
									continue;
								}
								disconnect_from_client(clnt_sock, session_data);
								continue;
							}

							if (!establish_connection(clnt_sock, session_data)) {
								disconnect_from_client(clnt_sock, session_data);
							}
							continue;
						}

					}

					handle_events(ev, clnt_sock, session_data);
				}
			}
		}
};
