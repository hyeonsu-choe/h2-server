#pragma once

#include <iostream>
#include <queue>
#include <functional>

#include "session.h"

struct ssl_ctx_st;
using SSL_CTX = ssl_ctx_st;

class Router;

enum class IOResult {
	SUCCESS,
	AGAIN,
	SHUTDOWN,
};

class Worker {
	private:
		const Router* router;
		std::function<void(int, std::shared_ptr<SessionData>)> update_events;
		std::function<IOResult(int, std::shared_ptr<SessionData>)> fill_input_buffer;
		std::function<IOResult(int, std::shared_ptr<SessionData>)> flush_output_buffer;

	private:
		int server_sock;
		struct sockaddr_in server_addr;

		int epfd;
		bool use_tls;
		SSL_CTX* ssl_ctx;

		std::unordered_map<int, std::shared_ptr<SessionData>> session_map;

	private:
		void set_nonblocking_socket(int& sock) const;
		void enable_address_and_port_reuse(int& sock) const;
		int create_listening_socket(const uint16_t port);
		int create_epoll(size_t epoll_size);
		void bind_callbacks_for_mode(bool use_tls);

		void set_event(int sock, uint32_t events);
		void update_event(int sock, uint32_t events, std::shared_ptr<SessionData> session_data);
		void update_events_h2c(int sock, std::shared_ptr<SessionData> session_data);
		bool should_disconnect(SessionState session_state);
		bool should_close_after_disconnect(const std::shared_ptr<SessionData>& session_data);
		void disconnect_from_client(int sock, std::shared_ptr<SessionData> session_data);
		bool check_close_event(uint32_t events);

		int send_server_connection_header(std::shared_ptr<SessionData> session_data);
		void handle_tls_handshake(int sock, std::shared_ptr<SessionData> session_data);
		void handle_new_connections(int clnt_sock);

		void handle_accept(int sock);
		void handle_events(uint32_t ev, int sock, std::shared_ptr<SessionData> session_data);
		IOResult handle_read(int sock, std::shared_ptr<SessionData> session_data);
		IOResult handle_write(int sock, std::shared_ptr<SessionData> session_data);

		// h2c
		IOResult feed_input_buffer(std::shared_ptr<SessionData> session_data);
		void fill_output_buffer(std::shared_ptr<SessionData> session_data);
		IOResult fill_input_buffer_h2c(int sock, std::shared_ptr<SessionData> session_data);
		IOResult flush_output_buffer_h2c(int sock, std::shared_ptr<SessionData> session_data);

		// tls
		void update_events_tls(int sock, std::shared_ptr<SessionData> session_data);
		void update_ssl_handshake_events(int sock, std::shared_ptr<SessionData> session_data);
		bool validate_alpn(std::shared_ptr<SessionData> session_data);
		bool handle_tls_accept(int sock, std::shared_ptr<SessionData> session_data);
		SessionState do_tls_handshake(int sock, std::shared_ptr<SessionData> session_data);
		SessionState establish_connection(int sock, std::shared_ptr<SessionData> session_data);
		IOResult fill_input_buffer_tls(int sock, std::shared_ptr<SessionData> session_data);
		IOResult flush_output_buffer_tls(int sock, std::shared_ptr<SessionData> session_data);

		bool startup(const uint16_t port);

	public:
		Worker(const Router* router, bool use_tls);
		Worker(Worker&& worker);
		~Worker();
		Worker(const Worker&) = delete;
		Worker& operator=(const Worker&) = delete;

		void run(const uint16_t server_port, std::string_view key_path, std::string_view cert_path);
};
