/***************************************************************/
// This is a utility class that implements a basic worker thread
// pattern in C++11: A background thread is created, and any
// operations sent to this class are forwarded to the background
// thread, run, then returned to the main thread. It is designed
// to avoid locking the actual running operation by enforcing a
// simple data ownership rule: The data exists either in the main
// thread or the worker thread, never both. The only locking that
// happens is used to pass the data between thread boundaries.
//
//
// Classes:
// 
// WORKER_THREAD
// A convience class for managing a worker thread and its
// operations. Clients send operations to the this class, which
// runs them on a separate thread and sends the results back
// to the main UI to be handled by the application.
//
// CACHING_WORKER_THREAD
// Wraps the WorkerThread with memory management of the operations,
// so clients don't need to do any additional work.
// 
//
// Use:
// Clients need to create a class that serves as both input and
// output and performs the operation. The only rule is that it
// the class have a function with the signature "void run()".
// I.e.:
// class Operation {
// public:
//		Operation() { }
//		void setup(data) { ...optional, but must operations would need input...}
//		void run() { ...do something... }
// };
//
// Then, in the main UI, the worker class needs to be instantiated
// with a callback that will report on finished operations:
//	auto					handler([](const WorkerOperation &op) {
//		...handle result...
//	});
//	async::CachingWorkerThread<WorkerOperation> worker(handler);
//
// And finally, update() needs to be called on the worker to handle
// any finished operations.
// worker.update().
//
// Now, clients make use of the class by sending an operation:
//		worker.run([&it](Operation &op) { op.setup(it); });
//
//
// This code is unlicensed, you are free to do whatever you like
// with it. The original author bears no responsibility for any
// damage done as a result of using this code.
// Original version by Eric Hackborn.
/***************************************************************/
#ifndef ASYNC_WORKERTHREAD_H_
#define ASYNC_WORKERTHREAD_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace async {

/**
 * \class async::WorkerThread
 * \brief Handle running an operation in a separate thread. Assume a basic pattern of
 * supplying an input object, running it in the thread, and returning it as an output object.
 ** IMPLICIT INTERFACE
 *		IO::run()
 */
template <typename IO>
class WorkerThread {
public:
	// Set a handler to receive notification when an IO operation is complete. This is
	// always called from the main thread. Clients can take ownership of the ptr if they want.
	WorkerThread(	const std::function<void(std::unique_ptr<IO>&)> &handler_fn,
				const std::string &name);
	~WorkerThread();

	void								run(std::unique_ptr<IO>);

	// In this implementation, the main UI thread needs to call update() to
	// clear out and distribute any finished operations. This is something I
	// handle behind the scenes in my frameworks, typically by having a
	// convenience class that automatically does main UI updating.
	void								update();

private:
	WorkerThread();
	WorkerThread(const WorkerThread<IO>&);
	WorkerThread&						operator=(const WorkerThread<IO>&);
	void								loop();

	const std::function<void(std::unique_ptr<IO>&)>
										mHandlerFn;

	const std::string					mName;
	std::thread							mThread;
	std::atomic_bool					mStop;
	std::mutex							mConditionMutex,
										mTransportMutex;
	std::condition_variable				mCondition;

	std::vector<std::unique_ptr<IO>>	mInput,
										mOutput,
										mHandleOutput;
};

/**
 * \class async::CachingWorkerThread
 * \brief Wrap a WorkerThread in something that caches and recycles the operations.
 */
template <typename IO>
class CachingWorkerThread {
public:
	// Set a handler to receive notification when an IO operation is complete.
	CachingWorkerThread(const std::function<void(const IO&)> &handler_fn,
						const std::string &name = "");

	// A new IO is generated automatically; the start_fn can be used to initialize it
	// before it goes off to the thread to run.
	void								run(const std::function<void(IO&)> &start_fn);

	// In this implementation, the main UI thread needs to call update() to
	// clear out and distribute any finished operations. This is something I
	// handle behind the scenes in my frameworks, typically by having a
	// convenience class that automatically does main UI updating.
	void								update();

private:
	void								finished(std::unique_ptr<IO>&);

	CachingWorkerThread();
	CachingWorkerThread(const CachingWorkerThread<IO>&);
	CachingWorkerThread&				operator=(const CachingWorkerThread<IO>&);

	const std::function<void(const IO&)>
										mHandlerFn;

	WorkerThread<IO>					mWorker;
	std::vector<std::unique_ptr<IO>>	mRetired;
};

/**
 * IMPLEMENTATION - WorkerThread
 */
template <typename IO>
WorkerThread<IO>::WorkerThread(	const std::function<void(std::unique_ptr<IO>&)> &handler_fn,
								const std::string &name)
		: mName(name)
		, mHandlerFn(handler_fn) {
	mStop.store(false);
	mThread = std::thread([this](){loop();});
}

template <typename IO>
WorkerThread<IO>::~WorkerThread() {
	try {
		mStop.store(true);
		mCondition.notify_all();
		mThread.join();
	} catch (std::exception const&) {
	}
}

template <typename IO>
void WorkerThread<IO>::run(std::unique_ptr<IO> io) {
	if (!io) return;
	try {
		{
			std::lock_guard<std::mutex> lock(mTransportMutex);
			mInput.push_back(std::move(io));
		}
		mCondition.notify_all();
	} catch (std::exception const&) {
	}
}

template <typename IO>
void WorkerThread<IO>::update() {
	mHandleOutput.clear();
	{
		std::lock_guard<std::mutex> lock(mTransportMutex);
		mHandleOutput.swap(mOutput);
	}
	if (mHandleOutput.empty() || !mHandlerFn) return;
	for (auto& e : mHandleOutput) {
		if (e) mHandlerFn(e);
	}
}

template <typename IO>
void WorkerThread<IO>::loop() {
	std::vector<std::unique_ptr<IO>>	input;

	while (!mStop.load()) {
		// Get input
		{
			std::lock_guard<std::mutex> lock(mTransportMutex);
			input.swap(mInput);
		}
		// Process and push to output
		{
			for (auto& e : input) {
				try {
					e->run();
					std::lock_guard<std::mutex> lock(mTransportMutex);
					mOutput.push_back(std::move(e));
				} catch (std::exception const&) {
				}
			}
			input.clear();
		}

		// Wait
		if (mStop.load()) break;
		std::unique_lock<std::mutex> lock(mConditionMutex);
		bool			needs_wait(true);
		{
			std::lock_guard<std::mutex> lock(mTransportMutex);
			needs_wait = mInput.empty();
		}
	    if (needs_wait) {
			mCondition.wait(lock);
		}
	}
}

/**
 * IMPLEMENTATION - CachingWorkerThread
 */
template <typename IO>
CachingWorkerThread<IO>::CachingWorkerThread(	const std::function<void(const IO&)> &handler_fn,
												const std::string &name)
		: mHandlerFn(handler_fn)
		, mWorker([this](std::unique_ptr<IO>& ptr){finished(ptr); }, name) {
}

template <typename IO>
void CachingWorkerThread<IO>::run(const std::function<void(IO&)> &start_fn) {
	try {
		std::unique_ptr<IO>			ptr;
		while (!mRetired.empty()) {
			ptr = std::move(mRetired.back());
			mRetired.pop_back();
			if (ptr) break;
		}
		if (!ptr) ptr.reset(new IO());
		if (!ptr) return;
		if (start_fn) start_fn(*(ptr.get()));
		mWorker.run(std::move(ptr));
	} catch (std::exception const&) {
	}
}

template <typename IO>
void CachingWorkerThread<IO>::update() {
	mWorker.update();
}

template <typename IO>
void CachingWorkerThread<IO>::finished(std::unique_ptr<IO> &ptr) {
	if (!ptr || !mHandlerFn) return;
	try {
		mHandlerFn(*(ptr.get()));
		mRetired.push_back(std::move(ptr));
	} catch (std::exception const&) {
	}
}

} // namespace async

#endif
