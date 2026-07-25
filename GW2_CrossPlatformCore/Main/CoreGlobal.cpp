#include "pch.h"
#include "CoreGlobal.h"
#include "ThreadManager.h"
#include "DeadLockProfiler.h"
#include "SendBuffer.h"
#include "GlobalQueue.h"
#include "IoCore.h"
#include "JobTimer.h"
#ifdef GW2_ENABLE_POSTGRES
#include "DBConnectionPool.h"
#endif
#include "ConsoleLog.h"

ThreadManager*      GThreadManager      = nullptr;
SendBufferManager*  GSendBufferManager  = nullptr;
GlobalQueue*        GGlobalQueue        = nullptr;
JobTimer*           GJobTimer           = nullptr;
DeadLockProfiler*   GDeadLockProfiler   = nullptr;

IoCore*             GIoCore = nullptr;
#ifdef GW2_ENABLE_POSTGRES
DBConnectionPool*   GDBConnectionPool = nullptr;
#endif
ConsoleLog*         GConsoleLogger = nullptr;

class CoreGlobal
{
public:
    CoreGlobal()
    {
        GThreadManager      = new ThreadManager();
        GSendBufferManager  = new SendBufferManager();
        GGlobalQueue        = new GlobalQueue();
        GJobTimer           = new JobTimer();
        GDeadLockProfiler   = new DeadLockProfiler();
        GIoCore             = new IoCore();
#ifdef GW2_ENABLE_POSTGRES
        GDBConnectionPool   = new DBConnectionPool();
#endif
        GConsoleLogger      = new ConsoleLog();
    }

    ~CoreGlobal()
    {
        delete GThreadManager;
        delete GSendBufferManager;
        delete GGlobalQueue;
        delete GJobTimer;
        delete GDeadLockProfiler;
        delete GIoCore;
#ifdef GW2_ENABLE_POSTGRES
        delete GDBConnectionPool;
#endif
        delete GConsoleLogger;
    }
} GCoreGlobal;