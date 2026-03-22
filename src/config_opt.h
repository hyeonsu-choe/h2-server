#pragma once

#include <iostream>
#include <cstdint>
#include <string>

constexpr const char* default_key_path = "./cert/server.key";
constexpr const char* default_cert_path = "./cert/server.crt";
constexpr uint16_t default_port = 443;

enum {
	MODE_TLS = 0, // epoll
	MODE_H2C = 1, // epoll
	MODE_H2C_IO_URING = 2 // io uring
};

class ConfigOption {
	private:
		void parse_command_line(int argc, char** argv);
		void print_usage(const char* name);

	public:
		bool is_help_mode;
		int mode;
		uint16_t port;
		uint8_t num_threads;
		std::string key_path;
		std::string cert_path;

		ConfigOption(int argc, char** argv);
		~ConfigOption() = default;
		friend std::ostream& operator<<(std::ostream&, const ConfigOption&);
};
