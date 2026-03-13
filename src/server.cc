#include <filesystem>
#include "server.h"


Server::Server(bool use_tls = true)
	: workers(), load_balancer(workers),
	epfd(-1), server_sock(-1)
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
}

void Server::set_reuse_socket(int& server_sock) const
{
	if (server_sock > 0) {
		int opt = true;
		if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt)) < 0) {
			std::cout << "setsockopt(REUSEADDR) error: " << strerror(errno) << std::endl;
		}
	}
}

void Server::set_nonblocking_socket(int& sock) const
{
	int flag = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flag|O_NONBLOCK);
}

int Server::create_listening_socket(struct sockaddr_in& server_addr, uint16_t server_port)
{
	int server_sock = socket(PF_INET, SOCK_STREAM, 0);
	if (server_sock > 0) {
		memset(&server_addr, 0, sizeof(struct sockaddr_in));
		server_addr.sin_family = AF_INET;
		server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		server_addr.sin_port = htons(server_port);

		set_reuse_socket(server_sock);
		set_nonblocking_socket(server_sock);

		if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)) == -1) {
			std::cout << "bind() error: " << strerror(errno) << std::endl;
			throw -1;
		}
	}
	return server_sock;
}

int Server::create_epoll(size_t epoll_size)
{
	int epfd = epoll_create(epoll_size);
	if (epfd <= 0) {
		std::cout << "epoll_create() error: " << strerror(errno) << std::endl;
		throw -1;
	}
	return epfd;
}

void Server::startup(uint16_t port)
{
	try {
		server_sock = create_listening_socket(addr, port);
		if (listen(server_sock, SOMAXCONN) == -1) {
			std::cout << "listen() error" << std::endl;
			throw -1;
		}

		epfd = create_epoll(1);
		set_event(server_sock, EPOLLIN);
	} catch (int execept_code) {
		std::cout << "program abort due to exception" << std::endl;
		exit(0);
	}
}

void Server::set_event(int sock, uint32_t events)
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

bool Server::add_handler(const METHOD method, std::string_view uri, Handler handler)
{
	return router.add(method, uri, handler);
}

void Server::handle_accept()
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
		} else {
			//std::cout << "client[" << clnt_sock << "] connected ..." << std::endl;
			int worker_index = load_balancer.acquire_worker_index(clnt_sock);
			if (worker_index < 0) {
				std::cerr << "enqueing failure : all worker is full" << std::endl;
				close(clnt_sock);
				continue; // break 해버리면, 커널의 대기큐에 남아 있는 요청들이, 다음번 이벤트 발생 시 까지 처리가 보류될 수 있음(edge trigger 모드 이므로)
			}
			if (!workers[worker_index].enqueue_sock(clnt_sock)) {
				std::cerr << "queue is full, drpped connection" << std::endl;
				close(clnt_sock);
				continue;
			}
		}
	}
}

bool Server::file_exists(std::string_view filename)
{
	if (filename.empty())
		return false;

	return std::filesystem::is_regular_file(std::filesystem::path(filename));
}

bool Server::is_runnable(const bool use_tls, std::string_view key_path, std::string_view cert_path)
{
	if (use_tls) {
		if (!file_exists(key_path)) {
			std::cerr << "key file [" << key_path << "] doesn't exist" << std::endl;
			return false;
		}

		if (!file_exists(cert_path)) {
			std::cerr << "cert file [" << cert_path << "] doesn't exist" << std::endl;
			return false;
		}
	}

	return true;
}

void Server::spawn_workers(const uint8_t num_threads, const bool use_tls, std::string_view key_path, std::string_view cert_path)
{
	workers.reserve(num_threads);
	threads.reserve(num_threads);

	// worker 객체 생성 및 등록 & 스레드 생성
	for (uint8_t i = 0; i < num_threads; i++) {
		workers.emplace_back(&router, use_tls);
	}
	for (uint8_t i = 0; i < num_threads; i++) {
		threads.emplace_back(&Worker::run, &workers[i], key_path, cert_path);
	}
}

void Server::listen_and_serve(const uint8_t num_threads, const bool use_tls, const uint16_t port, std::string_view key_path, std::string_view cert_path)
{
	if (!is_runnable(use_tls, key_path, cert_path)) {
		return;
	}

	startup(port);
	load_balancer.reset(num_threads);
	spawn_workers(num_threads, use_tls, key_path, cert_path);

	while (true) {
		struct epoll_event ev;
		int event_count = epoll_wait(epfd, &ev, 1, -1);
		if (event_count < 0) {
			if (errno == EINTR) {
				continue;
			}
			std::cerr << "epoll_wait() error" << strerror(errno) << std::endl;
			break;
		}

		if (event_count > 0) {
			handle_accept();
		}
	}

	for (auto& t : threads) {
		t.join();
	}
}
