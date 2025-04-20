#pragma once

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

class Server : public Worker {
	private:
		int epfd;
		int server_sock;
		struct sockaddr_in addr;
		struct epoll_event *ep_events;

		void setReuseSocket(int& server_sock) const;
		void setNonBlockingSocket(int& server_sock) const;
		int createListeningSocket(struct sockaddr_in& server_addr, uint16_t server_port);
		int createEPOLL(int server_sock, size_t epoll_size);
		struct epoll_event* createEventBucket(size_t size);
		void startUp(uint16_t port);
		void handle_accept();

	public:
		Server();
		~Server();
		void listen_and_serve(const uint16_t port, const char* key_path, const char* crt_path);
};
