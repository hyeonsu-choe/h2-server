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

#include "config_opt.h"
#include "worker.h"
#include "session.h"
#include "router.h"

class Server {
	private:
		Router router;
		ConfigOption config;

		std::vector<Worker> workers;
		std::vector<std::thread> threads;

		bool file_exists(std::string_view filename);
		void spawn_workers();
		void wait_for_workers();

	public:
		Server(const ConfigOption& config);
		~Server();

		bool add_handler(const METHOD method, std::string_view uri, Handler handler);
		bool is_runnable();
		void listen_and_serve();
};
