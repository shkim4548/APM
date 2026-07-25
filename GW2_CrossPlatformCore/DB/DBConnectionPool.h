#pragma once
#include <libpq-fe.h>
#include "Types.h"

/*---------------------
    DBConnectionPool
-----------------------*/

class DBConnection;

class DBConnectionPool
{
public:
    DBConnectionPool();
    ~DBConnectionPool();

    bool Connect(int32 connectionCount, const char* connectionString);
    void Clear();

    DBConnection* Pop();
    void Push(DBConnection* connection);

private:
    USE_LOCK;
    Vector<DBConnection*> _connections;
};

