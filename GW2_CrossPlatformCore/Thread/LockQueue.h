#pragma once

template<typename T>
class LockQueue
{
public:
	void Push(T item)
	{
		WRITE_LOCK;
		_items.push(item);
	}

	T Pop()
	{
		WRITE_LOCK;
		if (_items.empty())
			return T();

		T ret = _items.front();
		_items.pop();
		return ret;
	}

	void PopAll(OUT Vector<T>& items)
	{
		WRITE_LOCK;
		while (T item = Pop())
			items.push_back(item);
	}

	void Clear()
	{
		WRITE_LOCK;
		//_item.clear();	
		_items = Queue<T>();
		// 일부 STL에선 clear 함수가 없으므로 빈 큐로 밀어버린다
	}

private:
	USE_LOCK;
	Queue<T> _items;	// Allocator를 이용할 수 있는 자료형으로 변경
};