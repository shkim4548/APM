#include "pch.h"
#include "JobTimer.h"
#include "JobQueue.h"

/*-------------
	JobTimer
---------------*/

void JobTimer::Reserve(uint64 tickAfter, weak_ptr<JobQueue> owner, JobRef job)
{
	const uint64 executeTick = GetCurrentTick() + tickAfter;
	JobData* jobData = ObjectPool<JobData>::Pop(owner, job);

	WRITE_LOCK;	// �������� ����� �����̹Ƿ� ���� �Ǵ�.

	_items.push(TimerItem{ executeTick, jobData });
}

// ���Ⱑ �ٽ��̴�.
void JobTimer::Distribute(uint64 now)
{
	// 1ȸ�� 1�����常 �����Ų��
	if (_distributing.exchange(true) == true)	// �������� �̹� �Լ��� ������
		return;

	// �ִ��� ������ ���� �Լ����� Ǫ�õ� �������� �������.
	Vector<TimerItem> items;
	
	{
		WRITE_LOCK;
		while (_items.empty() == false)
		{
			const TimerItem& timerItem = _items.top();
			if (now < timerItem.executeTick)
				break;

			items.push_back(timerItem);
			_items.pop();
		}
	}

	for (TimerItem& item : items)
	{
		if (JobQueueRef owner = item.jobData->owner.lock())	// ���⼭ null üũ�� �� ���̴�.
			owner->Push(item.jobData->job);

		ObjectPool<JobData>::Push(item.jobData);
	}

	// �������� Ǯ���ش�
	_distributing.store(false);
}

void JobTimer::Clear()
{
	WRITE_LOCK;
	while (_items.empty() == false)
	{
		const TimerItem& timerItem = _items.top();
		ObjectPool<JobData>::Push(timerItem.jobData);
		_items.pop();
	}
}
