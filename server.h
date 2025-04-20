#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
//#include <queue>
#include <chrono>
#include <thread>
//#include <condition_variable>

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
	CLOSED,
	ERROR
};

class Server : public Worker {
	private:
		int epfd;
		int server_sock;
		struct sockaddr_in addr;
		struct epoll_event *ep_events;
		SSL_CTX* ssl_ctx;
		std::unordered_map<int, std::shared_ptr<http2_session_data_t>> session_map;

		void setReuseSocket(int& server_sock) const;
		void setNonBlockingSocket(int& server_sock) const;
		int createListeningSocket(struct sockaddr_in& server_addr, uint16_t server_port);
		int createEPOLL(int server_sock, size_t epoll_size);
		struct epoll_event* createEventBucket(size_t size);
		void startUp(uint16_t port);

		void set_event(int sock, uint32_t events);
		void update_event(int sock, uint32_t events, std::shared_ptr<http2_session_data_t> session_data);
		void update_events(int sock, std::shared_ptr<http2_session_data_t> session_data);
		void disconnect_from_client(int sock);
		void disconnect_from_client_if_done(int sock, std::shared_ptr<http2_session_data_t> session_data);

		IOResult fill_input_buffer(int sock, std::shared_ptr<http2_session_data_t> session_data);
		IOResult feed_input_buffer(std::shared_ptr<http2_session_data_t> session_data);
		void handle_read(int sock, std::shared_ptr<http2_session_data_t> session_data);
		void fill_output_buffer(std::shared_ptr<http2_session_data_t> session_data);
		IOResult flush_output_buffer(int sock, std::shared_ptr<http2_session_data_t> session_data);
		void handle_write(int sock, std::shared_ptr<http2_session_data_t> session_data);

		int send_server_connection_header(std::shared_ptr<http2_session_data_t> session_data);
		void handle_tls_handshake(int sock, std::shared_ptr<http2_session_data_t> session_data);
		void handle_accept();

		// ssl
		void update_ssl_handshake_events(int sock, std::shared_ptr<http2_session_data_t> session_data);
		bool validate_alpn(std::shared_ptr<http2_session_data_t> session_data);
		SessionState do_tls_handshake(int sock, std::shared_ptr<http2_session_data_t> session_data);
		SessionState establish_connection(int sock, std::shared_ptr<http2_session_data_t> session_data);

	public:
		Server();
		~Server();
		void listen_and_serve(const uint16_t port, const char* key_path, const char* crt_path);
};
