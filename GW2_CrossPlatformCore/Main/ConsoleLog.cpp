#include "pch.h"
#include "ConsoleLog.h"
#include <cstdarg>

ConsoleLog::ConsoleLog()
{
}

ConsoleLog::~ConsoleLog()
{
}

void ConsoleLog::WriteStdOut(Color color, const wchar_t* format, ...)
{
	if (format == nullptr)
		return;

	SetColor(true, color);

	va_list ap;
	va_start(ap, format);
	::vwprintf(format, ap);
	va_end(ap);

	fflush(stdout);

	SetColor(true, Color::WHITE);
}

void ConsoleLog::WriteStdErr(Color color, const wchar_t* format, ...)
{
	if (format == nullptr)
		return;

	SetColor(false, color);

	va_list ap;
	va_start(ap, format);
	::vfwprintf(stderr, format, ap);
	va_end(ap);

	fflush(stderr);

	SetColor(false, Color::WHITE);
}

void ConsoleLog::SetColor(bool stdOut, Color color)
{
	static const wchar_t* SColors[]
	{
		L"\033[0m",   // BLACK  -> 리셋
		L"\033[37m",  // WHITE
		L"\033[91m",  // RED
		L"\033[92m",  // GREEN
		L"\033[94m",  // BLUE
		L"\033[93m",  // YELLOW
	};

	FILE* out = stdOut ? stdout : stderr;
	::fputws(SColors[static_cast<int32>(color)], out);
}