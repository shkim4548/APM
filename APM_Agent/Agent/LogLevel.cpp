#include "pch.h"
#include "LogLevel.h"

namespace
{
std::atomic<int> g_logLevel{ static_cast<int>(LogLevel::Info) };
}

void SetLogLevel(int level)
{
    g_logLevel.store(level);
}

LogLevel GetLogLevel()
{
    return static_cast<LogLevel>(g_logLevel.load());
}
