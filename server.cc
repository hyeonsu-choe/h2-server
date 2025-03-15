#include "server.h"
#include <unordered_map>
#include <nghttp2/nghttp2.h>

const int EPOLL_SIZE = 16;
const int BUF_SIZE = 2048;

// HTTP/2 세션 콜백 - 클라이언트 연결 시 초기 설정
static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data) {
    std::cout << "Received headers from client (stream ID: " 
              << frame->hd.stream_id << ")\n";

	if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
		int32_t stream_id = frame->hd.stream_id;
    }

    return 0;
}

static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id,
		uint8_t *buf, size_t length, uint32_t *data_flags,
		nghttp2_data_source *source, void *user_data) {
	const char *response = "Hello, HTTP/2!";
	size_t len = strlen(response);
	memcpy(buf, response, len);
	*data_flags = NGHTTP2_DATA_FLAG_EOF; // 데이터 끝 표시
	return len;
}

// HTTP/2 데이터 요청 처리 콜백
static int on_request_recv_callback(nghttp2_session *session,
									const nghttp2_frame *frame,
                                    void *user_data) {

	if (frame->hd.type != NGHTTP2_HEADERS) {
		return 0; // HEADERS 프레임이 아닌 경우 무시
	}

    std::cout << "Request received on stream ID: " << frame->hd.stream_id << std::endl;

    // HTTP 응답 헤더
    nghttp2_nv hdrs[] = {
        {(uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-type", (uint8_t *)"text/plain", 12, 10, NGHTTP2_NV_FLAG_NONE}};

    // 응답 데이터
	nghttp2_data_provider data_prd;
	data_prd.read_callback = data_source_read_callback;

	if (session == nullptr) {
		std::cerr << "Error: session is NULL" << std::endl;
		return -1;
	}

    // 응답 제출 
    int rv = nghttp2_submit_response(session, frame->hd.stream_id, hdrs, sizeof(hdrs) / sizeof(hdrs[0]), &data_prd);
	if (rv != 0) {
		std::cerr << "Error submitting response: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}

	if (session == nullptr) {
		std::cerr << "Error: session is NULL before sending data" << nghttp2_strerror(rv) << std::endl;
		return -1;
	}

	// 세션 데이터 전송
	if (nghttp2_session_send(session) != 0) {
		std::cerr << "Error sending response" << std::endl;
		return -1;
	}

    return 0;
}

Server::Server() : epfd(-1), server_sock(-1), ep_events(nullptr) {
	memset(&addr , 0, sizeof(struct sockaddr_in));
}

Server::~Server() {
	if (server_sock > 0) {
		close(server_sock);
	}
	if (epfd > 0) {
		close(epfd);
	}
	if (ep_events) {
		delete []ep_events;
	}
}

void Server::setReuseSocket(int& server_sock) const {
	if (server_sock > 0) {
		int opt = true;
		if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt)) < 0) {
			std::cout << "setsockopt() error" << std::endl;
		}
	}
}

void Server::setNonBlockingSocket(int& sock) const {
	int flag = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flag|O_NONBLOCK);
}

int Server::createListeningSocket(struct sockaddr_in& server_addr, uint16_t server_port) {
	int server_sock = socket(PF_INET, SOCK_STREAM, 0);
	if (server_sock > 0) {
		memset(&server_addr, 0, sizeof(struct sockaddr_in));
		server_addr.sin_family = AF_INET;
		server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		server_addr.sin_port = htons(server_port);

		setReuseSocket(server_sock);
		setNonBlockingSocket(server_sock);

		if (bind(server_sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) == -1) {
			std::cout << "bind() error" << std::endl;
			throw -1;
		}
	}

	return server_sock;
}

void Server::addEvent(int epfd, int sock, int event_flag) {
	struct epoll_event event;

	event.events = event_flag;
	event.data.fd= sock;
	epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &event);
}

int Server::createEPOLL(int server_sock, size_t epoll_size) {
	int epfd = epoll_create(EPOLL_SIZE);
	if (epfd <= 0) {
		std::cout << "epoll_create() error" << std::endl;
		throw -1;
	}

	return epfd;
}

struct epoll_event* Server::createEventBucket(size_t size) {
	struct epoll_event* ep_events = new struct epoll_event[EPOLL_SIZE];
	if (!ep_events) {
		std::cout << "failed to create ep_events" << std::endl;
		throw -1;
	}

	return ep_events;
}

void Server::startUp(uint16_t port) {
	try {
		server_sock = createListeningSocket(addr, port);
		if (listen(server_sock, 5)==-1) {
			std::cout << "listen() error" << std::endl;
			throw -1;
		}

		epfd = createEPOLL(server_sock, EPOLL_SIZE);
		addEvent(epfd, server_sock, EPOLLIN);
		ep_events = createEventBucket(EPOLL_SIZE);
	} catch (int execept_code) {
		std::cout << "program abort due to exception" << std::endl;
		exit(0);
	}
}

//void Server::operator()() {
//}

void Server::run(uint16_t port) {
	startUp(port);

	int client_sock = -1;
	struct sockaddr_in client_addr;
	socklen_t client_addr_size;
	char buffer[BUF_SIZE];
//	std::unordered_map<int, nghttp2_session*> session_map;
	struct epoll_event event;

	while (!isShutdown()) {
		std::cout << "epoll_wait()" << std::endl;
		uint32_t actived_event_count = epoll_wait(epfd, ep_events, EPOLL_SIZE, -1);
		if (actived_event_count < 0) {
			std::cout << "epoll_wait() error" << std::endl;
			break;
		}

		for (uint32_t i = 0; i < actived_event_count; i++) {
			if (ep_events[i].data.fd == server_sock) {
				client_addr_size = sizeof(client_addr);
				client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_addr_size);
				if (client_sock < 0) continue;

				setNonBlockingSocket(client_sock);
				addEvent(epfd, client_sock, EPOLLIN|EPOLLET);

				std::cout << "client[" << client_sock << "] connected ..." << std::endl;

				// http2 session 생성

			} else {
				client_sock = ep_events[i].data.fd;
				int flags = fcntl(client_sock, F_GETFL, 0);
				if (!(flags & O_NONBLOCK)) {
					std::cout << "Warning: client_sock is not nonblocking" << std::endl;

				}
				//int total_len = 0;
				while (true) {
					//int read_len = read(client_sock, buffer + total_len, BUF_SIZE - total_len);
					int read_len = read(client_sock, buffer, BUF_SIZE);
					std::cout << "read len : " << read_len << std::endl;
					if (read_len == 0) {  // close request
							epoll_ctl(epfd, EPOLL_CTL_DEL, client_sock, NULL);
							close(client_sock);
							std::cout << "closed client[" << client_sock << "]" << std::endl;
							break;
					} else if (read_len < 0) {
						std::cout << "errno: "<< errno << ", " << strerror(errno) << std::endl;
						if (errno == EINTR) { // 인터럽트 시그널로 인한 read 반환
							continue; // need recv again
						}
						if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
							std::cout << "EWOULDBLOCK!!!"  << std::endl;
							usleep(5000000);
							event.events = EPOLLIN | EPOLLET;
							event.data.fd = client_sock;
							epoll_ctl(epfd, EPOLL_CTL_MOD, client_sock, &event);
							break; // need recv next
						}
						//break;

					} else {
				//		total_len += read_len;
						buffer[read_len] = 0;
						std::cout << "received message : " << buffer << std::endl;
						//break;
				/*	// 굳이 필요 없음,	
						if (total_len >= BUF_SIZE) {
							event.events = EPOLLIN | EPOLLET;
							event.data.fd = client_sock;
							epoll_ctl(epfd, EPOLL_CTL_MOD, client_sock, &event);
							break;
						}
				*/		
					}

				}
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

}

