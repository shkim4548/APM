#pragma once
#include "DBConnection.h"

/*----------------
    DBConnection
------------------*/
bool DBConnection::Connect(const char *connectionString)
{
    _connection = unique_ptr<PGconn, PGconnDeleter>(
		::PQconnectdb(connectionString),
		[](PGconn* c) { ::PQfinish(c); }
	);

	if (::PQstatus(_connection.get()) != CONNECTION_OK)
	{
		cerr << "[DBConnection::Connect] " << ::PQerrorMessage(_connection.get()) << endl;
		return false;
	}

	return true;
}

void DBConnection::Clear()
{
    Unbind();

    _connection.reset();	// PQfinish 자동 호출

}

// SQL 쿼리와 파라미터 배열을 받아서 실행한다. (libpq는 파라미터를 텍스트로 한번에 넘긴다)
bool DBConnection::Execute(const char *query, int32 paramCount, const char *const *paramValues)
{
    Unbind();

	_result = unique_ptr<PGresult, PGresultDeleter>(
		::PQexecParams(_connection.get(), query, paramCount, nullptr, paramValues, nullptr, nullptr, 0),
		[](PGresult* r) { ::PQclear(r); }
	);

	ExecStatusType status = ::PQresultStatus(_result.get());
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
	{
		HandleError();
		return false;
	}

	return true;
}

// 데이터를 긁어 올 때 사용하는 함수 (행 커서를 한 칸 전진)
bool DBConnection::Fetch()
{
    if(_result == nullptr)
        return false;

    _row++;
    return _row < ::PQntuples(_result.get());
}

// 데이터가 몇개 있는지를 확인하는 함수
int32 DBConnection::GetRowCount()
{
    if(_result == nullptr)
		return -1;

	return static_cast<int32>(::PQntuples(_result.get()));
}

void DBConnection::Unbind()
{
    _result.reset();	// PQclear 자동 호출
	_row = -1;
}

const char* DBConnection::GetColumn(int32 columnIndex)
{
    return ::PQgetvalue(_result.get(), _row, columnIndex);
}

bool DBConnection::IsColumnNull(int32 columnIndex)
{
    return ::PQgetisnull(_result.get(), _row, columnIndex) != 0;
}

// 쿼리 실행 오류를 처리하기 위한 내용
void DBConnection::HandleError()
{
    // TODO : Log, 라이브 서버에서는 로그를 파일로 남겨야한다. 여긴 장난식
	cerr << "[DBConnection] " << ::PQresultErrorMessage(_result.get()) << endl;
}