#pragma once
#include "Job.h"
#include "LockQueue.h"
#include "JobTimer.h"

/*---------------
	JobQueue
-----------------*/

class JobQueue : public enable_shared_from_this<JobQueue>
{
public:
	// 일반적 람다 이용
	void DoAsync(CallbackType&& callback)
	{
		Push(ObjectPool<Job>::MakeShared(std::move(callback)));
	}

	// 함수자 이용
	template<typename T, typename Ret, typename... Args>
	void DoAsync(Ret(T::*memFunc)(Args...), Args... args)
	{
		shared_ptr<T> owner = static_pointer_cast<T>(shared_from_this());	//스마트포인터 사용
		Push(ObjectPool<Job>::MakeShared(owner, memFunc, std::forward<Args>(args)...));
	}	

	// 만들고 바로 꽂는게 아니라 예약 후 빠져나온다.
	void DoTimer(uint64 tickAfter, CallbackType&& callback)
	{
		JobRef job = ObjectPool<Job>::MakeShared(std::move(callback));
		GJobTimer->Reserve(tickAfter, shared_from_this(), job);
	}
	
	// Job을 예약하는 식으로 수정한다.
	template<typename T, typename Ret, typename... Args>
	void DoTimer(uint64 tickAfter, Ret(T::* memFunc)(Args...), Args... args)
	{
		shared_ptr<T> owner = static_pointer_cast<T>(shared_from_this());	//스마트포인터 사용
		JobRef job = ObjectPool<Job>::MakeShared(owner, memFunc, std::forward<Args>(args)...);
		GJobTimer->Reserve(tickAfter, shared_from_this(), job);
	}

	void				ClearJob() { _jobs.Clear(); }

public:
	void				Push(JobRef job, bool pushOnly = false);
	void				Execute();

protected:
	LockQueue<JobRef>	_jobs;
	Atomic<int32>		_jobCount = 0;
};

