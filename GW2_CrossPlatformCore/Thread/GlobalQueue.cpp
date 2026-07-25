#include "pch.h"
#include "GlobalQueue.h"

/*----------------
	GlobalQueue
------------------*/

GlobalQueue::GlobalQueue()
{
}

GlobalQueue::~GlobalQueue()
{
}

void GlobalQueue::Push(JobQueueRef jobQueue)
{
	_jobQueues.Push(jobQueue);	// 이미 구현된 기능을 재사용
}

JobQueueRef GlobalQueue::Pop()
{
	return _jobQueues.Pop();
}
