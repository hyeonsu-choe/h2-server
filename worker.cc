#include "worker.h"

//std::queue<std::string> work_queue;
//std::mutex work_queue_lock;
//std::mutex sig_matcher_list_lock;
//std::condition_variable cond;


Worker::Worker() : is_shutdown(false), is_runnable(true) {

}

Worker::~Worker() {

}

bool Worker::isShutdown() const {
	return is_shutdown;
}

bool Worker::isRunnable() const {
	return is_runnable;
}

void Worker::suspend() {
	is_runnable = false;
}

void Worker::resume() {
	is_runnable = true;
}

void Worker::shutdown() {
	is_shutdown = true;
}





