#include "server.h" 

int main()
{
	Server server;

	//server.add_handler("/files", files_process);
	server.run(8080);
}
