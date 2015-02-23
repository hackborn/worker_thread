// WorkerThreadTest.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "worker_thread.h"

namespace {
void					update_loop();
std::atomic_bool		STOP;

// Define a class to run on the worker thread. There is one
// rule on this class, that it have a function named run().
class WorkerOperation {
public:
	WorkerOperation()		{ }

	const std::string&		getData() const {
		return mData;
	}

	void					setup(const std::string &data) {
		mData = data;
	}

	void					run() {
		std::cout << "\trun on worker thread (data=" << mData << ", thread id=" << std::this_thread::get_id() << ")" << std::endl;
		// Do SOMETHING to the data. Don't let this thread go to waste.
		mData += mData;
	}

private:
	std::string				mData;
};

// Boilerplate for the testing harness. This is used to get
// keyboard data into the "fake main UI" thread.
std::mutex					TRANSPORT_MUTEX;
std::vector<std::string>	TRANSPORT;
}

int _tmain(int argc, _TCHAR* argv[]) {
	// Start a thread merely to mimic having a main UI thread.
	STOP.store(false);
	std::thread			t(std::thread([](){update_loop();}));

	std::cout << "Type input and press enter. The input will be doubled on a worker thread, then sent back to the main thread. Press 'q' to quit." << std::endl;
	bool				keep_running(true);
	while (keep_running) {
		std::string		input;
		std::cin >> input;
		if (!input.empty()) {
			if (input.size() == 1 && (input[0] == 'q' || input[0] == 'Q')) {
				keep_running = false;
			} else {
				std::cout << "Entered " << input << std::endl;
				// Pass the data to the main thread
				std::lock_guard<std::mutex> lock(TRANSPORT_MUTEX);
				TRANSPORT.push_back(input);
			}
		}
	}

	try {
		STOP.store(true);
		t.join();
	} catch (std::exception const&) {
	}

	return 0;
}

namespace {

void						update_loop() {
	// This thread is basically a fake main-UI thread, something that would
	// have an update() function called say 60 times a second.
	
	auto					handler([](const WorkerOperation &op) {
		std::cout << "\thandle result on main thread (data=" << op.getData() << ", thread id=" << std::this_thread::get_id() << ")" << std::endl;
	});
	async::CachingWorkerThread<WorkerOperation>
								worker(handler);

	std::vector<std::string>	data;
	while (!STOP.load()) {
		// Grab any input and process it
		{
			data.clear();
			std::lock_guard<std::mutex> lock(TRANSPORT_MUTEX);
			data.swap(TRANSPORT);
		}
		if (!data.empty()) {
			for (const auto& it : data) {
				worker.run([&it](WorkerOperation &op) { op.setup(it); });
			}
		}

		// Let the main thread handle worker thread operations.
		worker.update();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

}
