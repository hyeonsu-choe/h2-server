#include <getopt.h>
#include "config_opt.h"

ConfigOption::ConfigOption(int argc, char** argv)
	: is_help_mode(false), mode(MODE_TLS), port(default_port), key_path(default_key_path), cert_path(default_cert_path), num_threads(1)
{
	parse_command_line(argc, argv);
}

void ConfigOption::print_usage(const char* name)
{
	std::cout << "Usage: " << name << " [options]" << std::endl
		<< "  -p, --port [PORT]       Port number (default: 443)" << std::endl
		<< "  -k, --key [KEY_PATH]    TLS private key path" << std::endl
		<< "  -c, --cert [CERT_PATH]  TLS certificate path" << std::endl
		<< "  -n, --num-threads [NUMBER]	Number of Threads (default: 1)" << std::endl
		<< "      --h2c               Use cleartext (disable TLS)" << std::endl
		<< "      --h2c-io-uring      Use cleartext in io_uring mode (disable TLS)" << std::endl
		<< "      --help              Show this message" << std::endl;
}

void ConfigOption::parse_command_line(int argc, char** argv)
{
	struct option long_options[] = {
		{"port", required_argument, 0, 'p'},
		{"key", required_argument, 0, 'k'},
		{"cert", required_argument, 0, 'c'},
		{"h2c", no_argument, 0, MODE_H2C},
		{"h2c-io-uring", no_argument, 0, MODE_H2C_IO_URING},
		{"thread", required_argument, 0, 'n'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "p:k:c:n:h", long_options, &option_index)) != -1) {
        switch (opt) {
			case MODE_TLS:
				mode = MODE_TLS;
				break;
			case MODE_H2C:
				mode = MODE_H2C;
				break;
			case MODE_H2C_IO_URING:
				mode = MODE_H2C_IO_URING;
				break;
            case 'p':
                port = static_cast<uint16_t>(std::atoi(optarg));
                break;
            case 'k':
                key_path = optarg;
                break;
            case 'c':
                cert_path = optarg;
                break;
			case 'n':
				num_threads = static_cast<uint8_t>(std::atoi(optarg));
				break;
            case 'h':
				is_help_mode = true;
				print_usage(argv[0]);
				break;
			case '?':
            default:
				print_usage(argv[0]);
				exit(EXIT_FAILURE);
        }
    }
}

std::ostream& operator<<(std::ostream& os, const ConfigOption& config_opt)
{
	os << "Backend: " << (config_opt.mode == MODE_H2C_IO_URING ? "io_uring" : "epoll") << std::endl;
	os << "Port: " << config_opt.port << std::endl;
	os << "TLS: " << (config_opt.mode == MODE_TLS ? "on" : "off") << std::endl;
	if (config_opt.mode == MODE_TLS) {
		os << "Key: " << config_opt.key_path << std::endl;
		os << "Cert: " << config_opt.cert_path << std::endl;
	}
	os << "Threads: " << static_cast<int>(config_opt.num_threads) << std::endl;
	return os;
}
