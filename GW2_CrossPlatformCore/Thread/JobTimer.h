#pragma once

struct JobData
{
	JobData(weak_ptr<JobQueue> owner, JobRef job) : owner(owner), job(job)
	{

	}

	weak_ptr<JobQueue>	owner;	// weak_ptr인 이유는 예약을 한참 후에 한다면 shared_ptr은 그 시간 동안 소멸x
	JobRef				job;
};

struct TimerItem
{
	bool operator<(const TimerItem& other) const
	{
		return executeTick > other.executeTick;
	}

	uint64 executeTick = 0;
	JobData* jobData = nullptr;	
	//Item이 우선순위큐에 들어가 있다해도, 위치가 바뀌면서 복사시 RefCount에 영향을 주지 않기 위해 생포인터 사용
};

/*-------------
	JobTimer
---------------*/

class JobTimer
{
public:
	void Reserve(uint64 tickAfter, weak_ptr<JobQueue> owner, JobRef job);	// 뭔가를 잡타이머에 예약
	void Distribute(uint64 now);	// Execute와 비슷한 의미
	void Clear();


private:
	USE_LOCK;
	PriorityQueue<TimerItem>	_items;
	Atomic<bool>				_distributing = false;
};

