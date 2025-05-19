#include <getopt.h>
#include "config_opt.h"

ConfigOption::ConfigOption(int argc, char** argv)
	: use_tls(1), port(default_port), key_path(default_key_path), cert_path(default_cert_path)
{
	parse_command_line(argc, argv);
}

void ConfigOption::parse_command_line(int argc, char** argv)
{
	struct option long_options[] = {
		{"port", required_argument, 0, 'p'},
		{"key", required_argument, 0, 'k'},
		{"cert", required_argument, 0, 'c'},
		{"h2c", no_argument, &use_tls, 0},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "p:k:c:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 0:
                // --h2c flag가 설정된 경우 아무것도 하지 않아도 됨
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
            case 'h':
            default:
				std::cout << "Usage: " << argv[0] << " [options]\n"
					<< "  -p, --port [PORT]       Port number (default: 443)\n"
					<< "  -k, --key [KEY_PATH]    TLS private key path\n"
					<< "  -c, --cert [CERT_PATH]  TLS certificate path\n"
					<< "      --h2c               Use cleartext (disable TLS)\n"
					<< "      --help              Show this message\n";
        }
    }
}

std::ostream& operator<<(std::ostream& os, const ConfigOption& config_opt)
{
	os << "Port: " << config_opt.port << std::endl;
	os << "TLS: " << (config_opt.use_tls ? "on" : "off") << std::endl;
	os << "Key: " << config_opt.key_path << std::endl;
	os << "Cert: " << config_opt.cert_path << std::endl;
	return os;
}
