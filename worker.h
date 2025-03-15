#pragma once

#include <iostream>


class Worker {
	private:
		bool is_shutdown;
		bool is_runnable;

	public:
		Worker();
		~Worker();
		virtual bool isShutdown() const;
		virtual bool isRunnable() const;
		virtual void suspend();
		virtual void resume();
		virtual void shutdown();
};
