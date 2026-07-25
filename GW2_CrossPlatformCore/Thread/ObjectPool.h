#pragma once

/*--------------
	ObjectPool
----------------*/

/*
	cross platform 은 단순히 구현
	전용 memoryPool, StompAllocator 없이 new/delete로 대체
	인터페이스만 유지하고 구현
*/

template<typename Type>
class ObjectPool
{
public:
	template<typename... Args>
	static Type* Pop(Args&&... args)
	{
		return new Type(forward<Args>(args)...);
	}

	static void Push(Type* obj)
	{
		delete obj;
	}

	template<typename... Args>
	static shared_ptr<Type> MakeShared(Args&&... args)
	{
		shared_ptr<Type> ptr = { Pop(forward<Args>(args)...), Push };
		return ptr;
	}
};

// 원본 Memory.h의 전역 MakeShared<T>() 대응 (호출부 코드를 바꾸지 않기 위한 래퍼)
template<typename Type, typename... Args>
shared_ptr<Type> MakeShared(Args&&... args)
{
	return ObjectPool<Type>::MakeShared(forward<Args>(args)...);
}
