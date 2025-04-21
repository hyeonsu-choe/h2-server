#pragma once

#include <iostream>
#include <string>

constexpr const char* default_key_path = "./cert/server.key";
constexpr const char* default_cert_path = "./cert/server.crt";
constexpr uint16_t default_port = 443;

class ConfigOption {
	private:
		void parse_command_line(int argc, char** argv);

	public:
		int use_tls;
		uint16_t port;
		std::string key_path;
		std::string cert_path;

		ConfigOption(int argc, char** argv);
		~ConfigOption() = default;
		friend std::ostream& operator<<(std::ostream&, const ConfigOption&);
};
