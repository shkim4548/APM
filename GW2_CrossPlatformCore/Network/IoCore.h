#pragma once

/*------------
	IoCore
--------------*/

class IoCore : public enable_shared_from_this<IoCore>
{
public:
	IoCore();
	virtual ~IoCore();

	asio::io_context& GetContext() { return _ioContext; }

	// Worker thread���� ȣ��
	void Run();
	void RunFor(int32 milliseconds);
	void Stop();

private:
	asio::io_context	_ioContext;
	asio::executor_work_guard<asio::io_context::executor_type> _workGuard;
};

