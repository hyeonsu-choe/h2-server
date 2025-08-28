#include <signal.h>
#include "server.h" 
#include "config_opt.h"

#include "h2_handler.h"

int main(int argc, char** argv)
{
	signal(SIGPIPE, SIG_IGN);  // 파이프 깨짐 무시
	ConfigOption config_opt(argc, argv);
	std::cout << config_opt << std::endl;


	Server server(config_opt.use_tls);
	server.add_handler(GET, "/files/{file_name}", download_handler);
	server.listen_and_serve(config_opt.port, config_opt.key_path, config_opt.cert_path);
}
