#pragma once

#include <liburing.h>

#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <memory>
#include <unordered_map>
#include <vector>
#include <iostream>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "router.h"
#include "session.h"
#include "session_engine.h"

class H2cIoUringWorker {
	private:
		static constexpr uint32_t QUEUE_SIZE = 1024;
		static constexpr size_t READ_BUF_SIZE = 16 * 1024;
		static constexpr uint8_t BATCH_CQE_COUNT = 64;

		enum class SubmitResult {
			OK,
			SKIPPED_CLOSING,
			SKIPPED_ALREADY_PENDING,
			SKIPPED_NO_DATA,
			FATAL
		};

		enum class OpType {
			ACCEPT,
			READ,
			WRITE,
		};

		union UserData {
			struct {
				uint64_t type : 8;
				uint64_t reserved: 24; // for padding
				uint64_t sock : 32;
			};
			uint64_t value;

			UserData() : value(0)
			{

			}

			UserData(OpType t, int s) : value(0)
			{
				type = static_cast<uint8_t>(t);
				sock = static_cast<uint32_t>(s);
			}

			static uint64_t pack(const OpType type, const int sock)
			{
				UserData user_data;
				user_data.type = static_cast<uint8_t>(type);
				user_data.sock = static_cast<uint32_t>(sock);

				return user_data.value;
			}

			static UserData unpack(const uint64_t value)
			{
				UserData user_data;
				user_data.value = value;

				return user_data;
			}

			friend std::ostream& operator<<(std::ostream& os, const UserData& user_data)
			{
				os << "{" << user_data.type << ", "
					<< static_cast<int>(user_data.sock) << "}";

				return os;
			}
		};

		struct Conn {
			int sock;
			std::shared_ptr<SessionData> session_data;
			std::vector<uint8_t> read_buf;
			bool reading;
			bool writing;
			bool closing;

			Conn(int sock = -1, std::shared_ptr<SessionData> session_data = nullptr)
				: sock(sock), session_data(std::move(session_data)), read_buf(READ_BUF_SIZE),
				reading(false), writing(false), closing(false)
			{
			}
		};

	private:
		const Router* router;
		int server_sock;
		sockaddr_in server_addr;
		io_uring ring;

		std::unordered_map<int, std::shared_ptr<Conn>> session_map;

	private:
		void set_nonblocking_socket(int sock) const
		{
			int flag = fcntl(sock, F_GETFL, 0);
			fcntl(sock, F_SETFL, flag | O_NONBLOCK);
		}

		void enable_address_and_port_reuse(int& sock) const
		{
			if (sock > 0) {
				int opt = true;
				if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt)) < 0) {
					std::cerr << "setsockopt(REUSEADDR) error: " << strerror(errno) << std::endl;
				}

				if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
					std::cerr << "setsockopt(SO_REUSEPORT) error: " << strerror(errno) << std::endl;
				}
			}
		}

		int create_listening_socket(const uint16_t port)
		{
			int sock = socket(PF_INET, SOCK_STREAM, 0);
			if (sock < 0) {
				std::cerr << "socket() error: " << strerror(errno) << std::endl;
				return -1;
			}

			memset(&server_addr, 0, sizeof(struct sockaddr_in));
			server_addr.sin_family = AF_INET;
			server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
			server_addr.sin_port = htons(port);

			enable_address_and_port_reuse(sock);
			set_nonblocking_socket(sock);

			if (bind(sock, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)) == -1) {
				std::cerr << "bind() error: " << strerror(errno) << std::endl;
				close(sock);
				return -1;
			}

			if (listen(sock, SOMAXCONN) < 0) {
				close(sock);
				std::cerr << "listen() error" << std::endl;
				return -1;
			}

			return sock;
		}

		bool startup(const uint16_t port)
		{
			if (io_uring_queue_init(QUEUE_SIZE, &ring, 0) < 0) {
				std::cerr << "io_uring_queue_init() failed" << std::endl;
				return false;
			}

			server_sock = create_listening_socket(port);
			if (server_sock < 0) {
				std::cerr << "create_listening_socket() failed" << std::endl;
				return false;
			}

			if (submit_accept() < 0) {
				std::cerr << "submit_accept() failed" << std::endl;
				return false;
			}

			if (io_uring_submit(&ring) < 0) {
				std::cerr << "io_uring_submit() failed" << std::endl;
				return false;
			}

			return true;
		}

		int submit_accept()
		{
			io_uring_sqe* sqe = io_uring_get_sqe(&ring);
			if (!sqe) {
				std::cerr << "io_uring_get_sqe() failed" << std::endl;
				return -1;
			}

			io_uring_prep_multishot_accept(sqe, server_sock, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
			io_uring_sqe_set_data64(sqe, UserData::pack(OpType::ACCEPT, server_sock));
			return 0;
		}

		SubmitResult submit_read(const std::shared_ptr<Conn>& conn)
		{
			if (!conn) {
				return SubmitResult::FATAL;
			}
			if (conn->closing) {
				return SubmitResult::SKIPPED_CLOSING;
			}
			if (conn->reading) {
				return SubmitResult::SKIPPED_ALREADY_PENDING;
			}

			io_uring_sqe* sqe = io_uring_get_sqe(&ring);
			if (!sqe) {
				std::cerr << "io_uring_get_sqe() failed" << std::endl;
				return SubmitResult::FATAL;
			}

			io_uring_prep_recv(sqe, conn->sock, conn->read_buf.data(), conn->read_buf.size(), 0);
			io_uring_sqe_set_data64(sqe, UserData::pack(OpType::READ, conn->sock));
			conn->reading = true;
			return SubmitResult::OK;
		}

		SubmitResult submit_write(const std::shared_ptr<Conn>& conn)
		{
			if (!conn) {
				return SubmitResult::FATAL;
			}
			if (conn->closing) {
				return SubmitResult::SKIPPED_CLOSING;
			}
			if (conn->writing) {
				return SubmitResult::SKIPPED_ALREADY_PENDING;
			}
			if (conn->session_data->output_buffer.empty()) {
				return SubmitResult::SKIPPED_NO_DATA;
			}

			io_uring_sqe* sqe = io_uring_get_sqe(&ring);
			if (!sqe) {
				std::cerr << "io_uring_get_sqe() failed" << std::endl;
				return SubmitResult::FATAL;
			}

			io_uring_prep_send(sqe,
					conn->sock,
					conn->session_data->output_buffer.data(),
					conn->session_data->output_buffer.size(),
					0);
			io_uring_sqe_set_data64(sqe, UserData::pack(OpType::WRITE, conn->sock));
			conn->writing = true;
			return SubmitResult::OK;
		}

		void disconnect_from_client(const std::shared_ptr<Conn>& conn)
		{
			if (!conn || conn->closing) {
				return;
			}

			conn->closing = true;
			conn->reading = false;
			conn->writing = false;

			if (conn->session_data) {
				conn->session_data->close_session();
			}

			if (conn->sock >= 0) {
				close(conn->sock);
			}

			session_map.erase(conn->sock);
		}

		bool establish_connection(const std::shared_ptr<Conn>& conn)
		{
			if (SessionEngine::send_server_connection_header(conn->session_data) != 0) {
				std::cerr << "send_server_connection() error" << std::endl;
				return false;
			}

			conn->session_data->state = SessionState::ESTABLISHED;
			SessionEngine::fill_output_buffer(conn->session_data);

			if (submit_write(conn) == SubmitResult::FATAL) {
				std::cerr << "submit_write() in establish_connection() error" << std::endl;
				return false;
			}
			if (submit_read(conn) == SubmitResult::FATAL) {
				std::cerr << "submit_read() in establish_connection() error" << std::endl;
				return false;
			}

			return true;
		}

		void handle_new_connection(int clnt_sock)
		{
			std::shared_ptr<SessionData> session_data = std::make_shared<SessionData>();
			if (init_session_data(session_data) != 0) {
				close(clnt_sock);
				return;
			}

			session_data->router = router;

			auto conn = std::make_shared<Conn>(clnt_sock, session_data);
			session_map.emplace(clnt_sock, conn);

			if (!establish_connection(conn)) {
				disconnect_from_client(conn);
			}
		}

		void handle_accept(int res, bool has_more)
		{
			if (!has_more) {
				if (submit_accept() < 0) {
					return;
				}
			}

			if (res < 0) {
				if (res != -EAGAIN && res != -EWOULDBLOCK && res != -EINTR) {
					std::cerr << "accept cqe error: " << strerror(-res) << std::endl;
				}
				return;
			}

			handle_new_connection(res);
		}

		bool ensure_read(const std::shared_ptr<Conn>& conn)
		{
			if (!conn || conn->closing) {
				return false;
			}

			SubmitResult result = submit_read(conn);
			switch (result) {
				case SubmitResult::OK:
				case SubmitResult::SKIPPED_ALREADY_PENDING:
					return true;

				case SubmitResult::SKIPPED_CLOSING:
					return false;

				case SubmitResult::SKIPPED_NO_DATA:
					// read에는 사실상 잘 나오지 않는 상태지만 방어적으로 처리
					return true;

				case SubmitResult::FATAL:
					std::cerr << "submit_read() error" << std::endl;
					disconnect_from_client(conn);
					return false;
			}
			return false;
		}

		bool ensure_write(const std::shared_ptr<Conn>& conn)
		{
			if (!conn || conn->closing) {
				return false;
			}

			SubmitResult result = submit_write(conn);
			switch (result) {
				case SubmitResult::OK:
				case SubmitResult::SKIPPED_ALREADY_PENDING:
				case SubmitResult::SKIPPED_NO_DATA:
					return true;

				case SubmitResult::SKIPPED_CLOSING:
					return false;

				case SubmitResult::FATAL:
					std::cerr << "submit_write() error" << std::endl;
					disconnect_from_client(conn);
					return false;
			}
			return false;
		}


		void handle_read(const std::shared_ptr<Conn>& conn, int res)
		{
			if (!conn) {
				return;
			}

			conn->reading = false;

			if (res == 0) {
				disconnect_from_client(conn);
				return;
			}

			if (res < 0) {
				if (res == -EINTR || res == -EAGAIN || res == -EWOULDBLOCK) {
					ensure_read(conn);
					return;
				}
				disconnect_from_client(conn);
				return;
			}

			conn->session_data->append_to_input_buffer(conn->read_buf.data(), static_cast<size_t>(res));

			IOResult feed_result = SessionEngine::feed_input_buffer(conn->session_data);
			if (feed_result == IOResult::SHUTDOWN) {
				disconnect_from_client(conn);
				return;
			}

			SessionEngine::fill_output_buffer(conn->session_data);

			if (SessionEngine::should_close_after_disconnect(conn->session_data)) {
				if (conn->session_data->output_buffer.empty()) {
					disconnect_from_client(conn);
					return;
				}

				// disconnect 전 마지막 flush
				ensure_write(conn);
				return;
			}

			// write 할 데이터가 존재한다면
			if (!conn->session_data->output_buffer.empty()) {
				if (!ensure_write(conn)) {
					return;
				}
			}

			// 다음 입력도 계속 수신하여 처리
			ensure_read(conn);
		}

		void handle_write(const std::shared_ptr<Conn>& conn, int res)
		{
			if (!conn) {
				return;
			}

			conn->writing = false;

			if (res < 0) {
				if (res == -EINTR || res == -EAGAIN || res == -EWOULDBLOCK) {
					ensure_write(conn);
					return;
				}
				disconnect_from_client(conn);
				return;
			}

			if (res == 0) {
				disconnect_from_client(conn);
				return;
			}

			conn->session_data->consume_output_buffer(static_cast<size_t>(res));
			SessionEngine::fill_output_buffer(conn->session_data);

			if (!conn->session_data->output_buffer.empty()) {
				ensure_write(conn);
				return;
			}

			if (SessionEngine::should_close_after_disconnect(conn->session_data)) {
				disconnect_from_client(conn);
				return;
			}

			if (!conn->reading) {
				ensure_read(conn);
			}
		}

	public:
		explicit H2cIoUringWorker(const Router* router)
			: router(router), server_sock(-1)
		{
			memset(&server_addr, 0, sizeof(server_addr));
			memset(&ring, 0, sizeof(ring));
		}

		~H2cIoUringWorker()
		{
			for (auto& [sock, conn] : session_map) {
				if (conn) {
					close(conn->sock);
				}
			}
			session_map.clear();

			if (server_sock >= 0) {
				close(server_sock);
			}

			io_uring_queue_exit(&ring);
		}

		H2cIoUringWorker(const H2cIoUringWorker&) = delete;
		H2cIoUringWorker& operator=(const H2cIoUringWorker&) = delete;

		void run(uint16_t port, std::string_view, std::string_view)
		{
			std::vector<io_uring_cqe*> cqes(BATCH_CQE_COUNT);

			if (!startup(port)) {
				std::cerr << "io_uring worker startup failed" << std::endl;
				return;
			}

			while (true) {
				int ret = io_uring_submit_and_wait(&ring, 1);
				if (ret < 0) {
					if (ret == -EINTR) {
						continue;
					}
					std::cerr << "io_uring_wait_cqe() error: " << strerror(-ret) << std::endl;
					break;
				}

				int count = io_uring_peek_batch_cqe(&ring, cqes.data(), BATCH_CQE_COUNT);
				for (uint8_t i = 0; i < count; i++) {
					io_uring_cqe* cqe = cqes[i];
					UserData user_data = UserData::unpack(io_uring_cqe_get_data64(cqe));
					OpType type = static_cast<OpType>(user_data.type);

					int res = cqe->res; // accept 일땐 client socket, 그 외에는 wrtie/read 반환 값

					if (type == OpType::ACCEPT) {
						handle_accept(res, (cqe->flags & IORING_CQE_F_MORE));
					} else {
						auto it = session_map.find(user_data.sock);
						std::shared_ptr<Conn> conn = (it != session_map.end()) ? it->second : nullptr;

						if (type == OpType::READ) {
							handle_read(conn, res);
						} else if (type == OpType::WRITE) {
							handle_write(conn, res);
						}
					}
				}

				io_uring_cq_advance(&ring, count);
			}
		}
};
