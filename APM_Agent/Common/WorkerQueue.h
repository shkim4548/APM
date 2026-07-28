#pragma once
#include "pch.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>

/*-------------
	WorkerQueue
---------------*/
// 단일 워커 스레드가 순차적으로 소비하는 스레드 안전 작업 큐. std::mutex/condition_variable만
// 사용 - GW2_CrossPlatformCore/Thread/JobQueue(+Lock)는 여러 프로젝트에서 이미 검증된 코드라
// 그대로 두기로 하고(2026-07-27), 대신 이 용도 전용으로 APM_Agent 안에 작은 큐를 새로 둠.
// 배경: JobQueue가 기대는 Lock::WriteUnlock()이 소유 스레드 비트를 절대 안 지우는 버그가 있어
// 네트워크 스레드가 한 번 락을 잡았다 놓으면 워커 스레드가 영원히 못 잡고 10초 뒤 크래시함 -
// 1-7 재실측 중 실제로 재현됨.
class WorkerQueue
{
public:
	WorkerQueue();
	~WorkerQueue();

	void Push(std::function<void()> job);

private:
	void WorkerLoop();

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	std::queue<std::function<void()>> _jobs;
	bool _stop = false;
	std::thread _worker;   // 마지막에 선언 - 위 멤버들이 이미 다 만들어진 뒤에 워커 스레드를 기동
};
