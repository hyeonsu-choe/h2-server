#include <filesystem>
#include "server.h"


Server::Server(const ConfigOption& config) : config(config)
{

}

Server::~Server()
{

}

bool Server::file_exists(std::string_view filename)
{
	if (filename.empty())
		return false;

	return std::filesystem::is_regular_file(std::filesystem::path(filename));
}

bool Server::add_handler(const METHOD method, std::string_view uri, Handler handler)
{
	return router.add(method, uri, handler);
}

bool Server::is_runnable()
{
	if (config.use_tls) {
		if (!file_exists(config.key_path)) {
			std::cerr << "key file [" << config.key_path << "] doesn't exist" << std::endl;
			return false;
		}

		if (!file_exists(config.cert_path)) {
			std::cerr << "cert file [" << config.cert_path << "] doesn't exist" << std::endl;
			return false;
		}
	}

	return true;
}

void Server::spawn_workers()
{
	workers.reserve(config.num_threads);
	threads.reserve(config.num_threads);

	// worker 객체 생성 및 등록 & 스레드 생성
	for (uint8_t i = 0; i < config.num_threads; i++) {
		if (config.use_tls) {
			workers.emplace_back(std::make_unique<WorkerAdapter<TlsWorker>>(&router));
		} else {
			workers.emplace_back(std::make_unique<WorkerAdapter<H2cWorker>>(&router));
		}
	}
	for (uint8_t i = 0; i < config.num_threads; i++) {
		threads.emplace_back([this, i]() {
					workers[i]->run(config.port, config.key_path, config.cert_path);
				});
	}
}

void Server::wait_for_workers()
{
	for (auto& t : threads) {
		t.join();
	}
}

void Server::listen_and_serve()
{
	if (!is_runnable()) {
		return;
	}

	spawn_workers();
	wait_for_workers();
}
