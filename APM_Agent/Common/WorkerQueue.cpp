#include "pch.h"
#include "WorkerQueue.h"

WorkerQueue::WorkerQueue()
	: _worker([this]() { WorkerLoop(); })
{
}

WorkerQueue::~WorkerQueue()
{
	{
		std::lock_guard<std::mutex> guard(_mutex);
		_stop = true;
	}
	_cv.notify_one();
	_worker.join();
}

void WorkerQueue::Push(std::function<void()> job)
{
	{
		std::lock_guard<std::mutex> guard(_mutex);
		_jobs.push(std::move(job));
	}
	_cv.notify_one();
}

void WorkerQueue::WorkerLoop()
{
	while (true)
	{
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait(lock, [this]() { return _stop || !_jobs.empty(); });

			// _stop이 요청됐어도 남은 job은 마저 비우고 종료 - 큐가 실제로 빈 경우에만 탈출.
			if (_jobs.empty())
				return;

			job = std::move(_jobs.front());
			_jobs.pop();
		}

		job();
	}
}
