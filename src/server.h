#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "worker.h"
#include "session.h"
#include "router.h"

class Server {
	private:
		Router router;

		std::vector<Worker> workers;
		std::vector<std::thread> threads;

		uint8_t load_count;
		int epfd;
		int server_sock;
		struct sockaddr_in addr;

		void set_reuse_socket(int& server_sock) const;
		void set_nonblocking_socket(int& server_sock) const;
		int create_listening_socket(struct sockaddr_in& server_addr, uint16_t server_port);
		int create_epoll(size_t epoll_size);
		void startup(uint16_t port);

		void set_event(int sock, uint32_t events);
		void handle_accept();
		void spawn_workers(uint8_t num_threads, const bool use_tls, const std::string& key_path, const std::string& cert_path);
		bool file_exists(const std::string& filename);
		bool is_runnable(const bool use_tls, const std::string& key_path, const std::string& cert_path);

	public:
		Server(bool);
		~Server();
		bool add_handler(const METHOD method, const std::string uri, Handler handler);
		void listen_and_serve(const uint8_t num_threads, const bool use_tls, const uint16_t port, const std::string& key_path, const std::string& cert_path);
};
