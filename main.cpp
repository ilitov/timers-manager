#include <iostream>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

using namespace std::chrono_literals;

class TimersManager {
public:
	using TimerCallback = std::function<void()>;

private:
	using TimeoutType = std::chrono::steady_clock::time_point;

	template <typename TimeoutType>
	static constexpr bool IsTimeoutDuration = std::is_same_v<TimeoutType, std::chrono::duration<typename TimeoutType::rep, typename TimeoutType::period>>;

	struct Timer {
		TimeoutType timeout{};
		TimerCallback callback{};

		bool operator>(const Timer &rhs) const {
			return timeout > rhs.timeout;
		}
	};

	static TimeoutType timeNow() {
		return std::chrono::steady_clock::now();
	}

public:
	TimersManager() {
		m_worker = std::jthread([this](std::stop_token stopToken) {
			workerLoop(stopToken);
		});
	}

	TimersManager(const TimersManager &) = delete;
	TimersManager &operator = (const TimersManager &) = delete;
	TimersManager(TimersManager &&) = delete;
	TimersManager &operator=(TimersManager &&) = delete;

	~TimersManager() {
		// Make sure the worker is stopped before clearing any memory
		if (m_worker.joinable()) {
			m_worker.request_stop();
			m_cv.notify_one();
			m_worker.join();
		}
	}

	template <typename Timeout>
	requires IsTimeoutDuration<Timeout>
	void insertTimer(TimerCallback cb, Timeout timeout) {
		const bool wakeUpWorker = std::invoke([&] {
			std::lock_guard lock(m_mtx);

			const TimeoutType internalTimeout = timeNow() + std::chrono::duration_cast<typename TimeoutType::duration>(timeout);
			const TimeoutType previousNearestTimeout = m_timers.empty() ? TimeoutType::max() : m_timers.front().timeout;

			// Add new timer and heapify
			m_timers.push_back(Timer{ internalTimeout, std::move(cb) });
			std::push_heap(m_timers.begin(), m_timers.end(), std::greater{});

			// This timer is on the top, wake up the worker
			if (internalTimeout < previousNearestTimeout) {
				m_shouldProcessTimers = true;
			}

			return m_shouldProcessTimers;
		});

		if (wakeUpWorker) {
			m_cv.notify_one();
		}
	}

private:
	void workerLoop(std::stop_token stopToken) {
		std::cout << "TimersManager worker started\n";

		const auto waitPred = [this, stopToken] { return m_shouldProcessTimers || stopToken.stop_requested(); };

		while (true) {
			TimerCallback cb;

			{
				std::unique_lock lock(m_mtx);

				if (!m_timers.empty()) {
					const TimeoutType nearestTimeout = m_timers.front().timeout;
					m_cv.wait_until(lock, nearestTimeout, waitPred);
				}
				else {
					m_cv.wait(lock, waitPred);
				}

				// Check if we have to exit(note we don't process all pending timers)
				if (stopToken.stop_requested()) {
					break;
				}

				m_shouldProcessTimers = false;

				if (m_timers.front().timeout <= timeNow()) {
					cb = std::move(m_timers.front().callback);

					// Reorder the vector and remove the popped element
					std::pop_heap(m_timers.begin(), m_timers.end(), std::greater{});
					m_timers.pop_back();
				}
			}

			if (cb) {
				cb();
			}
		}

		std::cout << "TimersManager worker exiting...\n";
	}

private:
	std::mutex m_mtx;
	std::condition_variable m_cv;
	bool m_shouldProcessTimers{ false };
	std::vector<Timer> m_timers;
	std::jthread m_worker;
};

// Test timer to measure the accuracy of the manager
struct TestTimer {
	TestTimer()
		: creationTime(std::chrono::steady_clock::now()) {

	}

	void operator()() {
		const auto executionTime = std::chrono::steady_clock::now();
		const auto diff = executionTime - creationTime;

		std::cout << "Slept for "
			<< std::chrono::duration_cast<std::chrono::seconds>(diff).count() << "s/"
			<< std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "ms\n";

		// Reset the state
		creationTime = executionTime;
	}

	std::chrono::steady_clock::time_point creationTime;
};

// Small thunk to simulate repeating timers without adding more flags and conditions to the manager
struct RepeatingTimer {
	TimersManager &manager;
	TimersManager::TimerCallback callback;
	std::chrono::seconds timeout;

	void operator()() {
		callback();
		manager.insertTimer(RepeatingTimer{ manager, std::move(callback), timeout }, timeout);
	}
};

int main() {
	TimersManager timers;

	timers.insertTimer(TestTimer{}, 3s);
	timers.insertTimer(TestTimer{}, 2s);
	timers.insertTimer(TestTimer{}, 1s);
	timers.insertTimer(TestTimer{}, 0s);
	timers.insertTimer(TestTimer{}, 5.5s);
	timers.insertTimer(TestTimer{}, 500ms);
	timers.insertTimer(RepeatingTimer{ timers, TestTimer{}, 1s }, 4s);

	char c;
	std::cin >> c;

	return 0;
}
