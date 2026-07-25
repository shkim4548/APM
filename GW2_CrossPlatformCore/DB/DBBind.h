#pragma once
#include "DBConnection.h"
#include <type_traits>
#include <cstdlib>

template<int32 C>
struct FullBits { enum { value = (1 << (C - 1)) | FullBits<C - 1>::value}; };

template<>
struct FullBits<1> { enum { value = 1}; };

template<>
struct FullBits<0> { enum { value = 0 }; };

/*-----------
    DBBind
-------------*/
// 파라미터 바인딩 누락을 비트 플래그로 체크한다

template<int32 ParamCount>
class DBBind
{
public:
    DBBind(DBConnection& dbConnection, const char* query)
		: _dbConnection(dbConnection), _query(query), _paramValues(ParamCount)
	{
		_paramFlag = 0;
		_dbConnection.Unbind();
	}

    bool Validate()
    {
        return _paramFlag == FullBits<ParamCount>::value;
    }

    bool Execute()
	{
		ASSERT_CRASH(Validate());

		Vector<const char*> paramPtrs(ParamCount);
		for (int32 i = 0; i < ParamCount; i++)
			paramPtrs[i] = _paramValues[i].c_str();

		return _dbConnection.Execute(_query, ParamCount, paramPtrs.empty() ? nullptr : paramPtrs.data());
	}

	bool Fetch()
	{
		return _dbConnection.Fetch();
	}

public:
	template<typename T>
	void In(int32 idx, const T& value)
	{
		_paramValues[idx] = std::to_string(value);
		_paramFlag |= (1LL << idx);
	}

	void In(int32 idx, const String& value)
	{
		_paramValues[idx] = value;
		_paramFlag |= (1LL << idx);
	}

	template<typename T>
	T Out(int32 idx)
	{
		const char* text = _dbConnection.GetColumn(idx);

		if constexpr (std::is_same_v<T, String>)
			return String(text);
		else if constexpr (std::is_same_v<T, int32>)
			return static_cast<int32>(std::atoll(text));
		else if constexpr (std::is_same_v<T, int64>)
			return static_cast<int64>(std::atoll(text));
		else if constexpr (std::is_same_v<T, double>)
			return std::atof(text);
		else if constexpr (std::is_same_v<T, float>)
			return static_cast<float>(std::atof(text));
		else if constexpr (std::is_same_v<T, bool>)
			return (text[0] == 't' || text[0] == 'T' || text[0] == '1');
		else
			static_assert(sizeof(T) == 0, "DBBind::Out - unsupported type");
	}

	bool IsNull(int32 idx)
	{
		return _dbConnection.IsColumnNull(idx);
	}

protected:
	DBConnection&		_dbConnection;
	const char*			_query;
	Vector<String>		_paramValues;
	uint64				_paramFlag;
};