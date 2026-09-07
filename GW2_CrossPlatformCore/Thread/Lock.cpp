#include "pch.h"
#include "Lock.h"
#include "CoreTLS.h"
#include "DeadLockProfiler.h"

// �������� �����ϴ� LThreadId�� �������� thread_local�� ��ü �ذ��Ѵ�.
namespace
{
	thread_local uint32 sThisThreadId = 0;
	atomic<uint32> sIdCounter = 1;

	uint32 GetThisThreadId()
	{
		if (sThisThreadId == 0)
			sThisThreadId = sIdCounter.fetch_add(1);

		return sThisThreadId;
	}
}

void Lock::WriteLock(const char* name)
{
#ifdef _DEBUG
	GDeadLockProfiler->PushLock(name);
#endif

	const uint32 threadId = GetThisThreadId();
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;

	if ((threadId & 0xFFFF) == lockThreadId)
	{
		_writeCount++;
		return;
	}

	const auto beginTick = GetCurrentTick();
	const uint32 desired = ((threadId << 16) & WRITE_THREAD_MASK);

	while (true)
	{
		for (uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; ++spinCount)
		{
			uint32 expected = EMPTY_FLAG;
			if (_lockFlag.compare_exchange_strong(expected, desired))
			{
				_writeCount++;
				return;
			}
		}
		if (GetCurrentTick() - beginTick >= ACQUIRE_TIMEOUT_TICK)
		{
			CRASH("LOCK_TIMEOUT");
		}
		this_thread::yield();
	}
}

void Lock::WriteUnlock(const char* name)
{
#ifdef _DEBUG
	GDeadLockProfiler->PopLock(name);
#endif

	// ReadLock이 아직 안 풀린 상태면 WriteUnlock 불가능
	if ((_lockFlag.load() & READ_COUNT_MASK) != 0)
	{
		CRASH("INVALID_UNLOCK_ORDER");
	}

	const int32 lockCount = --_writeCount;
	if (lockCount == 0)
	{
		_lockFlag.store(EMPTY_FLAG);
	}
}

void Lock::ReadLock(const char* name)
{
#ifdef _DEBUG
	GDeadLockProfiler->PushLock(name);
#endif

	const uint32 threadId = GetThisThreadId();
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;

	if ((threadId & 0xFFFF) == lockThreadId)
	{
		_lockFlag.fetch_add(1);
		return;
	}

	const auto beginTick = GetCurrentTick();

	while (true)
	{
		for (uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; ++spinCount)
		{
			uint32 expected = (_lockFlag.load() & READ_COUNT_MASK);
			if (_lockFlag.compare_exchange_strong(expected, expected + 1))
			{
				return;
			}

			if (GetCurrentTick() - beginTick >= ACQUIRE_TIMEOUT_TICK)
			{
				CRASH("LOCK_TIMEOUT");
			}
			this_thread::yield();
		}
	}
}

void Lock::ReadUnlock(const char* name)
{
#ifdef _DEBUG
	GDeadLockProfiler->PopLock(name);
#endif
	if ((_lockFlag.fetch_sub(1) & READ_COUNT_MASK) == 0)
	{
		CRASH("MULTIPLE_UNLOCK");
	}
}