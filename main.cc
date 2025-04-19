#include "server.h" 

int main()
{
	Server server;

	//server.add_handler("/files", files_process);
	server.listen_and_serve(8080, "./cert/server.key", "./cert/server.crt");
}
