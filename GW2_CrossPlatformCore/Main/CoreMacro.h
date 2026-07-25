#pragma once

// MSVC SAL 전용 환경 대응
//#ifdef _MSC_VER
#define __analysis_assume(expr)
//#endif

#define OUT
#define SETTER

#define NAMESPACE_BEGIN(name) namespace name{
#define NAMESPACE_END			}

/*---------------
	  Lock
---------------*/

#define USE_MANY_LOCKS(count)	Lock _locks[count];
#define USE_LOCK				USE_MANY_LOCKS(1)
#define	READ_LOCK_IDX(idx)		ReadLockGuard readLockGuard_##idx(_locks[idx], typeid(this).name());
#define READ_LOCK				READ_LOCK_IDX(0)
#define	WRITE_LOCK_IDX(idx)		WriteLockGuard writeLockGuard_##idx(_locks[idx], typeid(this).name());
#define WRITE_LOCK				WRITE_LOCK_IDX(0)

/*---------------
	  Crash
---------------*/

inline void PrintStackTrace()
{
#ifdef _WIN32
	void* stack[62];
	HANDLE process = ::GetCurrentProcess();
	::SymInitialize(process, nullptr, TRUE);

	uint16 frames = ::CaptureStackBackTrace(0, 62, stack, nullptr);
	SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
	symbol->MaxNameLen = 255;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

	for (uint16 i = 0; i < frames; i++)
	{
		::SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
		cerr << "[" << frames - i - 1 << "] " << symbol->Name
			 << " - 0x" << hex << symbol->Address << dec << "\n";
	}
	free(symbol);
#else
	void* stack[62];
	int32 frames = backtrace(stack, 62);
	char** symbols = backtrace_symbols(stack, frames);

	for (int32 i = 0; i < frames; i++)
		cerr << "[" << i << "] " << symbols[i] << "\n";

	free(symbols);
#endif
}

inline void CrashLog(const char* cause, const char* file, int32 line, const char* func)
{
	cerr << "[CRASH] cause=" << cause
		 << " file=" << file
		 << " line=" << line
		 << " func=" << func << "\n";

	ofstream ofs("crash.log", ios::app);
	if (ofs.is_open())
	{
		ofs << "[CRASH] cause=" << cause
			<< " file=" << file
			<< " line=" << line
			<< " func=" << func << "\n";
	}
}

#define CRASH(cause)								\
{													\
	CrashLog(cause, __FILE__, __LINE__, __FUNCTION__);	\
	PrintStackTrace();								\
	abort();										\
}

#define ASSERT_CRASH(expr)			\
{									\
	if (!(expr))					\
	{								\
		CRASH(#expr);				\
	}								\
}

// 크로스플랫폼 GetTickCount64 대체
inline uint64 GetCurrentTick()
{
	using namespace chrono;
	return static_cast<uint64>(
		duration_cast<milliseconds>(
			steady_clock::now().time_since_epoch()
		).count()
		);
}
