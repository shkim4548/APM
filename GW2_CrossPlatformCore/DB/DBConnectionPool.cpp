#include "pch.h"
#include "DBConnection.h"
#include "DBConnectionPool.h"

DBConnectionPool::DBConnectionPool()
{
}

DBConnectionPool::~DBConnectionPool()
{
	Clear();
}

// 원하는 개수만큼 연결을 미리 만들어두는 함수
bool DBConnectionPool::Connect(int32 connectionCount, const char* connectionString)
{
    WRITE_LOCK;
    for(int32 i = 0; i< connectionCount; ++i)
    {
        DBConnection* connection = new DBConnection();
        if(connection->Connect(connectionString) == false)
        {
            delete connection;
            return false;
        }

        _connections.push_back(connection);
    }
    return true;
}

void DBConnectionPool::Clear()
{
    WRITE_LOCK;
    for(DBConnection* connection : _connections)
    {
        connection->Clear();
        delete connection;
    }
    _connections.clear();
}

DBConnection* DBConnectionPool::Pop()
{
    WRITE_LOCK;
    if(_connections.empty())
        return nullptr;
    
    DBConnection* connection = _connections.back();
    _connections.pop_back();
    return connection;
}

void DBConnectionPool::Push(DBConnection* connection)
{
	WRITE_LOCK;

	_connections.push_back(connection);
}