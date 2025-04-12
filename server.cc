#include <nghttp2/nghttp2.h>
#include <unordered_map>
#include <list>
#include <vector> 

#include "server.h"

#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

#define MAKE_NV(NAME, VALUE)                                                   \
  {                                                                            \
    (uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
        NGHTTP2_NV_FLAG_NONE                                                   \
  }

const int EPOLL_SIZE = 1024;
const int BUF_SIZE = 2048;

class http2_stream_data_t {
	public:
		uint32_t stream_id;
		int fd;
		std::string request_path;

		http2_stream_data_t(uint32_t stream_id = 0) : stream_id(stream_id), fd(-1) {
		}

		~http2_stream_data_t() {
			if (fd >= 0) {
				close(fd);
			}
		}
};

class http2_session_data_t {
	public:
		std::list<std::unique_ptr<http2_stream_data_t>> streams;
		nghttp2_session* session; // shared_ptr 로 바꿔 보기
		std::vector<uint8_t> output_buffer;
		std::vector<uint8_t> input_buffer;
		uint32_t events;

	public:
		http2_session_data_t()
			: session(nullptr), events(0) {
		}

		~http2_session_data_t() {
			if (session) {
				nghttp2_session_del(session);
			}
		}

		void append_to_output_buffer(const uint8_t* data, size_t length)
		{
			output_buffer.insert(output_buffer.end(), data, data + length);
		}

		void consume_output_buffer(size_t length)
		{
			output_buffer.erase(output_buffer.begin(), output_buffer.begin() + length);
		}

		void append_to_input_buffer(const uint8_t* data, size_t length)
		{
			input_buffer.insert(input_buffer.end(), data, data + length);
		}

		void consume_input_buffer(size_t length)
		{
			input_buffer.erase(input_buffer.begin(), input_buffer.begin() + length);
		}
};


std::unordered_map<int, std::shared_ptr<http2_session_data_t>> session_map; // 나중에 shared_ptr로 바꿀 것

void update_event2(int epfd, int sock, uint32_t events)
{
	struct epoll_event ev;
	ev.events = events | EPOLLET;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev) < 0) {
		if (errno == ENOENT) {
			epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
		}
	}
}

void update_event(int epfd, int sock, uint32_t events, std::shared_ptr<http2_session_data_t> session_data)
{
	if (session_data->events == events) {
		return;
	}

	session_data->events = events;

	struct epoll_event ev;
	ev.events = events | EPOLLET;
	ev.data.fd = sock;
	if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev) < 0) {
		if (errno == ENOENT) {
			epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
		}
	}
}

static void update_events(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	uint32_t events = EPOLLIN;
	if (!session_data->output_buffer.empty() || nghttp2_session_want_write(session_data->session)) {
		events |= EPOLLOUT;
		//update_event(epfd, sock, EPOLLIN | EPOLLOUT, session_data);
	} //else {
	//	update_event(epfd, sock, EPOLLIN, session_data);
	//}
	update_event(epfd, sock, events, session_data);
}

static nghttp2_ssize file_read_callback(nghttp2_session* session, int32_t stream_id,
                                        uint8_t* buf, size_t length,
                                        uint32_t* data_flags, nghttp2_data_source* source,
                                        void* user_data)
{
    int fd = source->fd;
    ssize_t r;

    while ((r = read(fd, buf, length)) == -1 && errno == EINTR);

    if (r == -1) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    if (r == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (nghttp2_ssize)r;
}

static int send_response(nghttp2_session *session, int32_t stream_id,
                         nghttp2_nv *nva, size_t nvlen, int fd)
{
	nghttp2_data_provider2 data_prd;
	data_prd.source.fd = fd;
	data_prd.read_callback = file_read_callback;

	int rv = nghttp2_submit_response2(session, stream_id, nva, nvlen, &data_prd);
	if (rv != 0) {
		std::cout << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}

	return 0;
}

static const char ERROR_HTML[] = "<html><head><title>404</title></head>"
                                  "<body><h1>404 Not Found</h1></body></html>";
static nghttp2_ssize error_read_callback(nghttp2_session* session, int32_t stream_id,
                                        uint8_t* buf, size_t length,
                                        uint32_t* data_flags, nghttp2_data_source* source,
                                        void* user_data )
{
	size_t copy_len = std::min(length, strlen(ERROR_HTML));
	memcpy(buf, ERROR_HTML, std::min(length, copy_len));
	*data_flags |= NGHTTP2_DATA_FLAG_EOF;

	return copy_len;
}

static int error_reply(nghttp2_session* session, http2_stream_data_t* stream_data)
{
    nghttp2_nv hdrs[] = {MAKE_NV(":status", "404")};
	nghttp2_data_provider2 data_prd;
	data_prd.read_callback = error_read_callback;

	int rv = nghttp2_submit_response2(session, stream_data->stream_id, hdrs, ARRLEN(hdrs), &data_prd);
	if (rv != 0) {
		std::cout << "Fatal error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}

    return 0;
}

static int on_request_recv(nghttp2_session* session, http2_session_data_t* session_data, http2_stream_data_t* stream_data)
{
	nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};
	const char* rel_path = stream_data->request_path.c_str();
	for (rel_path = stream_data->request_path.c_str(); *rel_path == '/'; rel_path++);

	int fd = open(rel_path, O_RDONLY);
	if (fd < 0) {
		std::cout << "errno: " << errno << ", " << strerror(errno) << std::endl;
		if (error_reply(session, stream_data) != 0) {
			return NGHTTP2_ERR_CALLBACK_FAILURE;
		}
		return 0;
	}

	stream_data->fd = fd;

	if (send_response(session, stream_data->stream_id, hdrs, ARRLEN(hdrs), fd) < 0) {
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}

	return 0;
}

static int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame *frame, void *user_data)
{
	http2_session_data_t* session_data = static_cast<http2_session_data_t*>(user_data);	
	http2_stream_data_t* stream_data;

	switch (frame->hd.type) {
		case NGHTTP2_HEADERS:
		case NGHTTP2_DATA:
			if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
				stream_data = static_cast<http2_stream_data_t*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					return 0;
				}
				return on_request_recv(session, session_data, stream_data);
			}
			break;
		default:
			break;
	}

	return 0;
}

static std::unique_ptr<http2_stream_data_t> create_http2_stream_data(http2_session_data_t* session_data, uint32_t stream_id)
{
	return std::make_unique<http2_stream_data_t>(stream_id);
}

static void delete_http2_stream_data(http2_session_data_t* session_data, http2_stream_data_t* stream_data)
{
	auto iter = std::find_if(session_data->streams.begin(), session_data->streams.end(),
			[stream_data](const std::unique_ptr<http2_stream_data_t>& ptr) {
				return ptr.get() == stream_data;
			});

	if (iter != session_data->streams.end()) {
		session_data->streams.erase(iter);
	}
}

static void add_stream_to_session(http2_session_data_t* session_data, std::unique_ptr<http2_stream_data_t> stream_data)
{
	uint32_t stream_id = stream_data->stream_id;

	nghttp2_session_set_stream_user_data(session_data->session, stream_id, stream_data.get());
	session_data->streams.push_back(std::move(stream_data));
}

static int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data)
{
	http2_session_data_t* session_data = (http2_session_data_t*)user_data;
	http2_stream_data_t* stream_data;
	(void)error_code;

	stream_data = static_cast<http2_stream_data_t*>(nghttp2_session_get_stream_user_data(session, stream_id));
	if (!stream_data) {
		return 0;
	}

	delete_http2_stream_data(session_data, stream_data);
	return 0;
}

static int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
							const uint8_t* name, size_t namelen,
							const uint8_t *value, size_t valuelen,
							uint8_t flags, void* user_data)
{
	http2_stream_data_t* stream_data;
	const char PATH[] = ":path";

	switch (frame->hd.type) {
		case NGHTTP2_HEADERS:
			if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
				break;
			}

			if (memcmp(PATH, name, namelen) == 0 && namelen == sizeof(PATH) - 1) {
				stream_data = static_cast<http2_stream_data_t*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
				if (!stream_data) {
					break;
				}

				stream_data->request_path = reinterpret_cast<const char*>(value);
			}

			// 헤더 출력
	//		std::string name_str(reinterpret_cast<const char*>(name), namelen);
	//		std::string value_str(reinterpret_cast<const char*>(value), valuelen);

	//		std::cout << "name: " << name_str << std::endl;
	//		std::cout << "value: " << value_str << std::endl;

			break;
	}

	return 0;
}

static int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
	http2_session_data_t* session_data = (http2_session_data_t*)user_data;

	if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
		return 0;
	}

	std::unique_ptr<http2_stream_data_t> stream_data = create_http2_stream_data(session_data, frame->hd.stream_id);
	add_stream_to_session(session_data, std::move(stream_data));

	return 0;
}

static int init_http2_session_data(std::shared_ptr<http2_session_data_t> session_data)
{
	nghttp2_session_callbacks *callbacks;
	try {
		if (nghttp2_session_callbacks_new(&callbacks) != 0) {
			throw std::runtime_error("nghttp2_session_callbacks_new() error");
		}

		nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback); // 프레임 모두 도착 시 호출 (프레임 n개 도착 시, n번 호출)
		nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback); // 스트림 닫히려고 할 때 호출
		nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback); // 헤더의 name-value 쌍 확인 및 저장
		nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback); //HEADERS 또는 PUSH_PROMISE 프레임에서 헤더 블록 수신 시작 시 호출

		if (nghttp2_session_server_new(&session_data->session, callbacks, session_data.get()) != 0) {
			throw std::runtime_error("nghttp2_session_server_new() error");
		}

	} catch (const std::exception& ex) {
		std::cerr << "exception: " << ex.what() << std::endl;
		if (callbacks) {
			nghttp2_session_callbacks_del(callbacks);
		}

		return -1;
	}

	nghttp2_session_callbacks_del(callbacks);
	return 0;
}

static void disconnect_from_client(int epfd, int sock)
{
	epoll_ctl(epfd, EPOLL_CTL_DEL, sock, NULL);
	close(sock);
	session_map.erase(sock);

//	std::cout << "closed client[" << sock << "]" << std::endl;
}

static int send_server_connection_header(std::shared_ptr<http2_session_data_t> session_data)
{
	nghttp2_settings_entry iv[1] = {
		{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
	};

	int rv = nghttp2_submit_settings(session_data->session, NGHTTP2_FLAG_NONE, iv, sizeof(iv) / sizeof(iv[0]));
	if (rv != 0) {
		std::cerr << "nghttp2_submit_settings() error: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}

	return 0;
}

enum class IOResult {
	SUCCESS,
	AGAIN,
	CLOSED,
	ERROR
};

static IOResult fill_input_buffer(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	unsigned char buffer[BUF_SIZE];

	while (1) {
		ssize_t read_len = read(sock, buffer, BUF_SIZE);
		if (read_len == 0) {
			return IOResult::CLOSED;
		} else if (read_len < 0) {
			if (errno == EINTR) { // 인터럽트 시그널로 인한 read 반환
				continue;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK) { // 소켓 버퍼에 더 이상 읽을 데이터가 없음(EOF가 아님)
				return IOResult::AGAIN;
			} 

			std::cerr << "read() error: " << strerror(errno) << std::endl;
			return IOResult::ERROR;
		}

		session_data->append_to_input_buffer(buffer, read_len);
	}

	return IOResult::SUCCESS;
}

static IOResult feed_input_buffer(std::shared_ptr<http2_session_data_t> session_data)
{
	while (!session_data->input_buffer.empty()) {
		nghttp2_ssize fed_len = nghttp2_session_mem_recv2(
				session_data->session,
				session_data->input_buffer.data(),
				session_data->input_buffer.size());
		if (fed_len < 0) {
			std::cerr << "nghttp2_session_mem_recv2() error: " << nghttp2_strerror((int)fed_len) << std::endl;
			return IOResult::ERROR;
		}

		session_data->consume_input_buffer(fed_len);
	}

	return IOResult::SUCCESS;
}

static void handle_read(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	IOResult input_result = fill_input_buffer(sock, session_data);
	if (input_result == IOResult::CLOSED || input_result == IOResult::ERROR) {
		disconnect_from_client(epfd, sock);
		return;
	}

	IOResult feed_result = feed_input_buffer(session_data);
	if (feed_result == IOResult::ERROR) {
		disconnect_from_client(epfd, sock);
		return;
	}

	update_events(epfd, sock, session_data);

	if (!nghttp2_session_want_read(session_data->session) &&
		!nghttp2_session_want_write(session_data->session) &&
		session_data->output_buffer.empty()) {
		disconnect_from_client(epfd, sock);
	}
}

static void fill_output_buffer(std::shared_ptr<http2_session_data_t> session_data)
{
	while (nghttp2_session_want_write(session_data->session)) {
		const uint8_t* data;
		size_t length = nghttp2_session_mem_send2(session_data->session, &data);
		if (length <= 0) {
			break;
		}
		session_data->append_to_output_buffer(data, length);
	}
}

static IOResult flush_output_buffer(int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	while (!session_data->output_buffer.empty()) {
		ssize_t written_len = write(sock, session_data->output_buffer.data(), session_data->output_buffer.size());
		if (written_len < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return IOResult::AGAIN; // 현재 처리 할 데이터가 없으므로 다음 EPOLLOUT 때 처리

			std::cerr << "write() error: " << strerror(errno) << std::endl;
			return IOResult::ERROR;
		}
		session_data->consume_output_buffer(written_len);
	}

	return IOResult::SUCCESS;
}

static void handle_write(int epfd, int sock, std::shared_ptr<http2_session_data_t> session_data)
{
	fill_output_buffer(session_data);
	IOResult flush_result = flush_output_buffer(sock, session_data);
	if (flush_result == IOResult::ERROR) {
		disconnect_from_client(epfd, sock);
		return;
	}

	update_events(epfd, sock, session_data);

	if (!nghttp2_session_want_read(session_data->session) &&
		!nghttp2_session_want_write(session_data->session) &&
		session_data->output_buffer.empty()) {
		disconnect_from_client(epfd, sock);
	}
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
			std::cout << "setsockopt() error: " << strerror(errno) << std::endl;
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
			std::cout << "bind() error: " << strerror(errno) << std::endl;
			throw -1;
		}
	}

	return server_sock;
}

int Server::createEPOLL(int server_sock, size_t epoll_size) {
	int epfd = epoll_create(EPOLL_SIZE);
	if (epfd <= 0) {
		std::cout << "epoll_create() error: " << strerror(errno) << std::endl;
		throw -1;
	}

	return epfd;
}

struct epoll_event* Server::createEventBucket(size_t size) {
	struct epoll_event* ep_events = new struct epoll_event[EPOLL_SIZE];
	if (!ep_events) {
		std::cout << "allocation to ep_events error" << std::endl;
		throw -1;
	}

	return ep_events;
}

void Server::startUp(uint16_t port) {
	try {
		server_sock = createListeningSocket(addr, port);
		if (listen(server_sock, 100)==-1) {
			std::cout << "listen() error" << std::endl;
			throw -1;
		}

		epfd = createEPOLL(server_sock, EPOLL_SIZE);
		update_event2(epfd, server_sock, EPOLLIN);
		ep_events = createEventBucket(EPOLL_SIZE);
	} catch (int execept_code) {
		std::cout << "program abort due to exception" << std::endl;
		exit(0);
	}
}

void Server::run(uint16_t port) {
	startUp(port);
//	cpu_set_t cpuset;
//	CPU_ZERO(&cpuset);
//	CPU_SET(2, &cpuset);
//	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	int clnt_sock = -1;
	struct sockaddr_in client_addr;
	socklen_t client_addr_size;
	struct epoll_event event;

	while (!isShutdown()) {
		uint32_t actived_event_count = epoll_wait(epfd, ep_events, EPOLL_SIZE, -1);
		if (actived_event_count < 0) {
			std::cout << "epoll_wait() error" << std::endl;
			break;
		}

		for (uint32_t i = 0; i < actived_event_count; i++) {
			if (ep_events[i].data.fd == server_sock) {
				client_addr_size = sizeof(client_addr);
				while (true) {
					clnt_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_addr_size);
					if (clnt_sock < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							//std::cerr << "accept() :  EWOULDBLOCK()" << std::endl;
						} else {
							std::cout << "accept() : ERROR" << std::endl;
						}
						break;
					} else {
						setNonBlockingSocket(clnt_sock);
//						std::cout << "client[" << clnt_sock << "] connected ..." << std::endl;

						// http2 session 생성
						std::shared_ptr<http2_session_data_t> session_data = std::make_shared<http2_session_data_t>();
						if (init_http2_session_data(session_data) != 0) {
							close(clnt_sock);
							continue;
						}

						if (send_server_connection_header(session_data) != 0) {
							close(clnt_sock);
							continue;
						}

						// 이벤트 등록 및 소켓과 세션 데이터 등록
						update_events(epfd, clnt_sock, session_data);
						session_map.emplace(clnt_sock, session_data);
					}
				}
			} else {
				uint32_t ev = ep_events[i].events;
				clnt_sock = ep_events[i].data.fd;

				auto iter = session_map.find(clnt_sock);
				if (iter != session_map.end()) {
					std::shared_ptr<http2_session_data_t> session_data = iter->second;
					if (!iter->second) {
						std::cout << "session_data doesn't exist" << std::endl;
						continue;
					}

					if (ev & EPOLLIN) {
						handle_read(epfd, clnt_sock, session_data);
					}

					if (ev & EPOLLOUT) {
						handle_write(epfd, clnt_sock, session_data);
					}
				} else {
					std::cout << "session_data doesn't exist" << std::endl;
				}
			}
		}

//		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	session_map.clear(); // 세션 맵 반납을 명시적으로 수행 (하지 않아도 됨)
}

