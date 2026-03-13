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

class LoadBalancer {
	std::vector<Worker>& workers;
	std::vector<uint32_t> indirect_table;

	// balance table의 엔트리 전체에 0 ~ worker_num 순서로 반복해서 입력
	void init_indirect_table(const uint32_t worker_num)
	{
		uint32_t i = 0;
		for (auto& entry : indirect_table) {
			entry = (i++ % worker_num);
		}
	}

	bool is_full(uint32_t index)
	{
		return workers[index].is_full();
	}

	int find_available_worker(uint32_t start)
	{
		auto size = workers.size();
		auto index = (start + 1) % size;

		while (index != start) {
			if (!is_full(index)) {
				return index;
			}

			index = (index + 1) % size;
		}

		return -1;
	}

public:
	LoadBalancer(std::vector<Worker>& workers, uint32_t worker_num = 1, uint32_t table_size = 1024)
		: workers(workers), indirect_table(table_size)
	{
		init_indirect_table(worker_num);
	}

	void reset(uint32_t worker_num)
	{
		init_indirect_table(worker_num);
	}

	int acquire_worker_index(uint32_t key)
	{
		int table_index = key % indirect_table.size();
		auto worker_index = indirect_table[table_index];

		if (!is_full(worker_index)) {
			return worker_index;
		}

		int new_index = find_available_worker(worker_index);
		if (new_index == -1) {
			return -1;
		}

		indirect_table[table_index] = new_index;
		return new_index;
	}

	friend std::ostream& operator<<(std::ostream& os, const LoadBalancer& lb)
	{
		for (auto& data : lb.indirect_table) {
			os << data << std::endl;
		}

		return os;
	}
};

class Server {
	private:
		Router router;

		std::vector<Worker> workers;
		std::vector<std::thread> threads;
		LoadBalancer load_balancer;

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
		void spawn_workers(uint8_t num_threads, const bool use_tls, std::string_view key_path, std::string_view cert_path);
		bool file_exists(std::string_view filename);
		bool is_runnable(const bool use_tls, std::string_view key_path, std::string_view cert_path);

	public:
		Server(bool);
		~Server();
		bool add_handler(const METHOD method, std::string_view uri, Handler handler);
		void listen_and_serve(const uint8_t num_threads, const bool use_tls, const uint16_t port, std::string_view key_path, std::string_view cert_path);
};
