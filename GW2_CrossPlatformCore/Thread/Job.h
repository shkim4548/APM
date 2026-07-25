#pragma once
#include <functional>

/*-------------
	Job
---------------*/

using CallbackType = std::function<void()>;

class Job
{
public:
	// Job으로 래핑해서 람다 캡쳐함수를 사용하겠다.
	Job(CallbackType&& callback) : _callback(std::move(callback))
	{
	}

	// 멤버 함수로 사용할 경우가 많으므로 특별히 추가하는 버전이다.
	template<typename T, typename Ret, typename... Args>
	Job(shared_ptr<T> owner, Ret(T::* memFunc)(Args...), Args&&...args/*보편 참조*/)
	{
		// shared_ptr, weak_ptr 중 하나를 선택해서 사용 가능하다, 여기선 shared사용
		_callback = [owner, memFunc, args...]()
		{
			(owner.get()->*memFunc)(args...);
		};
	}

	void Execute()
	{
		_callback();
	}
private:
	CallbackType _callback;
};