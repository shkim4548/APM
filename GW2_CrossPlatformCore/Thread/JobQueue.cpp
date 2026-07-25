#include "pch.h"
#include "JobQueue.h"
#include "GlobalQueue.h"

/*-----------------
	JobQueue
-------------------*/

void JobQueue::Push(JobRef job, bool pushOnly)
{
	const int prevCount = _jobCount.fetch_add(1);	//prevCount를 증가 한다음에 반드시 job을 추가해야한다
	_jobs.Push(job);	// WRITE_LOCK

	// 첫번째 Job을 넣은 쓰레드가 실행까지 담당
	if (prevCount == 0)
	{
		// LCurrentJobQueue가 비었나를 미리 확인해본다.
		if (LCurrentJobQueue == nullptr && pushOnly == false)
		{
			Execute();
		}
		else
		{
			// 여유 있는 다른 스레드가 실행하도록 GlobalQueue에 넘긴다
			GGlobalQueue->Push(shared_from_this());	// 다른놈한테 떠넘기기
		}
	}
}

// 루프를 돌면서 Execute에 전달된 내용을 실행한다.
void JobQueue::Execute()
{
	LCurrentJobQueue = this;

	while (true)
	{
		Vector<JobRef> jobs;
		_jobs.PopAll(OUT jobs);

		const int32 jobCount = static_cast<int32>(jobs.size());
		for (int32 i = 0; i < jobCount; i++)
			jobs[i]->Execute();

		// 남은 일감이 0개라면 종료
		if (_jobCount.fetch_sub(jobCount) == jobCount)
		{
			LCurrentJobQueue = nullptr;
			return;
		}

		const uint64 now = GetCurrentTick();
		if (now >= LEndTickCount)
		{
			LCurrentJobQueue = nullptr;
			// 여유 있는 다른 쓰레드가 실행하도록 GlobalQueue에 넘긴다.
			GGlobalQueue->Push(shared_from_this());
			break;
		}
	}
}
