#pragma once
#include "pch.h"
#include <libpq-fe.h>
#include "Types.h"

/*
    DBConnection
*/

using PGconnDeleter = void(*)(PGconn*);
using PGresultDeleter = void(*)(PGresult*);

class DBConnection
{
public:
    bool Connect(const char* connectionString);
    void Clear();

    bool Execute(const char* query, int32 paramCount, const char* const *paramValues);
    bool Fetch();
    int32 GetRowCount();
    void Unbind();

    const char* GetColumn(int32 columnIndex);
    bool IsColumnNull(int32 columnIndex);

private:
    void HandleError();

private:
    unique_ptr<PGconn, PGconnDeleter>		_connection{ nullptr, [](PGconn* c) { ::PQfinish(c); } };
	unique_ptr<PGresult, PGresultDeleter>	_result{ nullptr, [](PGresult* r) { ::PQclear(r); } };
	int32									_row = -1;
};