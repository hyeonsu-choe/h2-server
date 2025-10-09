#pragma once

#include <iostream>
#include <queue>
#include <mutex>
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
		std::function<bool(uint32_t)> check_rd_hup;
		std::function<void(int, std::shared_ptr<SessionData>)> update_events;
		std::function<IOResult(int, std::shared_ptr<SessionData>)> fill_input_buffer;
		std::function<IOResult(int, std::shared_ptr<SessionData>)> flush_output_buffer;

	private:
		std::mutex m;
		int signal_fd;
		int epfd;
		bool use_tls;
		SSL_CTX* ssl_ctx;

		std::queue<int> socket_queue;
		std::unordered_map<int, std::shared_ptr<SessionData>> session_map;

	private:
		void set_mode(bool use_tls);
		int create_epoll(size_t epoll_size);
		void set_event(int sock, uint32_t events);
		void update_event(int sock, uint32_t events, std::shared_ptr<SessionData> session_data);
		void update_events_h2c(int sock, std::shared_ptr<SessionData> session_data);
		bool should_disconnect(SessionState session_state);
		void disconnect_from_client(int sock);

		int send_server_connection_header(std::shared_ptr<SessionData> session_data);
		void handle_tls_handshake(int sock, std::shared_ptr<SessionData> session_data);
		void handle_accept();

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

		bool startup();

	public:
		Worker(const Router* router, bool use_tls);
		Worker(Worker&& worker);
		~Worker();
		Worker(const Worker&) = delete;
		Worker& operator=(const Worker&) = delete;


		void enqueue_sock(int sock);
		int dequeue_sock();
		void run(const std::string& key_path, const std::string& cert_path);
};
