#pragma once
#include "NetAddress.h"
#include "IoCore.h"

enum class ServiceType : uint8
{
	Server,
	Client
};

using SessionFactory = function<SessionRef(void)>;

/*-------------
	Service
--------------*/

class Service : public enable_shared_from_this<Service>
{
public:
	Service(ServiceType type, NetAddress address, IoCoreRef core, SessionFactory factory, int32 maxSessionCount = 1);
	virtual ~Service();

	virtual bool	Start() = 0;
	bool			CanStart() { return _sessionFactory != nullptr; }

	virtual void	CloseService();
	void			SetSessionFactory(SessionFactory func) { _sessionFactory = func; }
	void			Broadcast(SendBufferRef sendBuffer);
	SessionRef		CreateSession();
	void			AddSession(SessionRef session);
	void			ReleaseSession(SessionRef session);
	int32			GetCurrentSessionCount() { return _sessionCount; }
	int32			GetMaxSessionCount() { return _maxSessionCount; }

public:
	ServiceType		GetServiceType() { return _type; }
	NetAddress		GetNetAddress() { return _netAddress; }
	IoCoreRef&		GetIoCore() { return _ioCore; }

protected:
	USE_LOCK;
	ServiceType _type;
	NetAddress _netAddress = {};
	IoCoreRef _ioCore;

	Set<SessionRef> _sessions;
	int32 _sessionCount = 0;
	int32 _maxSessionCount = 0;
	SessionFactory _sessionFactory;
};

/*------------------
	ClientService
--------------------*/
class ClientService : public Service
{
public:
	ClientService(NetAddress targetAddress, IoCoreRef core, SessionFactory factory, int32 maxSessionCount = 1);
	virtual ~ClientService() { }
	virtual bool Start() override;
};

/*-----------------
	ServerService
-------------------*/
class ServerService : public Service
{
public:
	ServerService(NetAddress address, IoCoreRef core, SessionFactory factory, int32 maxSessionCount = 1);
	virtual ~ServerService() { }

	virtual bool Start() override;
	virtual void CloseService() override;
	bool GetIsRunning() { return _isRunning; }

private:
	ListenerRef _listener = nullptr;
	Atomic<bool> _isRunning = false;;
};