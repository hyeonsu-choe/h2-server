#include <signal.h>
#include "server.h" 
#include "config_opt.h"

#include "handler.h"

int main(int argc, char** argv)
{
	signal(SIGPIPE, SIG_IGN);  // 파이프 깨짐 무시
	ConfigOption config_opt(argc, argv);

	if (!config_opt.is_help_mode) {
		std::cout << config_opt << std::endl;

		Server server(config_opt.use_tls);
		server.add_handler(GET, "/files/{file_name}", downloader);
		server.add_handler(POST, "/files", uploader);
		server.listen_and_serve(config_opt.num_threads, config_opt.use_tls, config_opt.port, config_opt.key_path, config_opt.cert_path);
	}
}
