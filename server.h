#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <functional>

#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "worker.h"
#include "http2_session.h"


enum class IOResult {
	SUCCESS,
	AGAIN,
	SHUTDOWN,
//	ERROR
};

class Server : public Worker {
	private:
		std::function<bool(uint32_t)> check_rd_hup;
		std::function<void(int, std::shared_ptr<http2_session_data_t>)> update_events;
		std::function<IOResult(int, std::shared_ptr<http2_session_data_t>)> fill_input_buffer;
		std::function<IOResult(int, std::shared_ptr<http2_session_data_t>)> flush_output_buffer;

	private:
		bool use_tls;
		int epfd;
		int server_sock;
		struct sockaddr_in addr;
		struct epoll_event *ep_events;
		SSL_CTX* ssl_ctx;
		std::unordered_map<int, std::shared_ptr<http2_session_data_t>> session_map;


		void set_mode(bool use_tls);
		void setReuseSocket(int& server_sock) const;
		void setNonBlockingSocket(int& server_sock) const;
		int createListeningSocket(struct sockaddr_in& server_addr, uint16_t server_port);
		int createEPOLL(int server_sock, size_t epoll_size);
		struct epoll_event* createEventBucket(size_t size);
		void startUp(uint16_t port);

		void set_event(int sock, uint32_t events);
		void update_event(int sock, uint32_t events, std::shared_ptr<http2_session_data_t> session_data);
		void update_events_h2c(int sock, std::shared_ptr<http2_session_data_t> session_data);
		bool should_disconnect(SessionState session_state);
		void disconnect_from_client(int sock);
//		bool disconnect_from_client_if_done(int sock, std::shared_ptr<http2_session_data_t> session_data);

		int send_server_connection_header(std::shared_ptr<http2_session_data_t> session_data);
		void handle_tls_handshake(int sock, std::shared_ptr<http2_session_data_t> session_data);
		void handle_accept();

		void handle_events(uint32_t ev, int sock, std::shared_ptr<http2_session_data_t> session_data);
		IOResult handle_read(int sock, std::shared_ptr<http2_session_data_t> session_data);
		IOResult handle_write(int sock, std::shared_ptr<http2_session_data_t> session_data);

		// h2c
		IOResult feed_input_buffer(std::shared_ptr<http2_session_data_t> session_data);
		void fill_output_buffer(std::shared_ptr<http2_session_data_t> session_data);
		IOResult fill_input_buffer_h2c(int sock, std::shared_ptr<http2_session_data_t> session_data);
		IOResult flush_output_buffer_h2c(int sock, std::shared_ptr<http2_session_data_t> session_data);

		// tls
		void update_events_tls(int sock, std::shared_ptr<http2_session_data_t> session_data);
		void update_ssl_handshake_events(int sock, std::shared_ptr<http2_session_data_t> session_data);
		bool validate_alpn(std::shared_ptr<http2_session_data_t> session_data);
		bool handle_tls_accept(int sock, std::shared_ptr<http2_session_data_t> session_data);
		SessionState do_tls_handshake(int sock, std::shared_ptr<http2_session_data_t> session_data);
		SessionState establish_connection(int sock, std::shared_ptr<http2_session_data_t> session_data);
		IOResult fill_input_buffer_tls(int sock, std::shared_ptr<http2_session_data_t> session_data);
		IOResult flush_output_buffer_tls(int sock, std::shared_ptr<http2_session_data_t> session_data);

	public:
		Server(bool);
		~Server();
		void listen_and_serve(const uint16_t port, const std::string& key_path, const std::string& cert_path);
};
