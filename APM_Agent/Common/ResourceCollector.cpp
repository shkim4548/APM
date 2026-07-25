#include "pch.h"
#include "ResourceCollector.h"

#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>
#endif

SystemMetrics ResourceCollector::Collect()
{
	SystemMetrics metrics;
	metrics.cpuUsagePercent = ComputeCpuUsage();
	metrics.memTotalBytes = GetMemTotal();
	metrics.memUsedBytes = GetMemUsed();
#ifdef _WIN32
	GetDiskUsage(metrics.diskTotalBytes, metrics.diskUsedBytes, "C:\\");
#else
	GetDiskUsage(metrics.diskTotalBytes, metrics.diskUsedBytes, "/");
#endif
	ComputeNetworkUsage(metrics.netRxBytesPerSec, metrics.netTxBytesPerSec);
	return metrics;
}

#ifdef _WIN32

double ResourceCollector::ComputeCpuUsage()
{
	FILETIME idleTime, kernelTime, userTime;
	if (!::GetSystemTimes(&idleTime, &kernelTime, &userTime))
		throw std::runtime_error("ResourceCollector::ComputeCpuUsage - GetSystemTimes 실패");

	auto toUint64 = [](const FILETIME& ft) -> uint64
	{
		return (static_cast<uint64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	};

	uint64 idle = toUint64(idleTime);
	uint64 kernel = toUint64(kernelTime);   // kernelTime에는 idle이 포함됨(WinAPI 문서 명시)
	uint64 user = toUint64(userTime);
	uint64 total = kernel + user;

	double usage = 0.0;
	if (_hasPrevSample && total > _prevTotal)
	{
		uint64 idleDelta = idle - _prevIdle;
		uint64 totalDelta = total - _prevTotal;
		usage = 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
	}

	_prevIdle = idle;
	_prevTotal = total;
	_hasPrevSample = true;
	return usage;
}

void ResourceCollector::ComputeNetworkUsage(uint64& rxBytesPerSec, uint64& txBytesPerSec)
{
	PMIB_IF_TABLE2 ifTable = nullptr;
	if (::GetIfTable2(&ifTable) != NO_ERROR)
		throw std::runtime_error("ResourceCollector::ComputeNetworkUsage - GetIfTable2 실패");

	uint64 rxTotal = 0, txTotal = 0;
	for (ULONG i = 0; i < ifTable->NumEntries; ++i)
	{
		const MIB_IF_ROW2& row = ifTable->Table[i];
		if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;   // 루프백 제외 - Linux 쪽 "lo" 제외와 동일 취지
		if (row.OperStatus != IfOperStatusUp)
			continue;   // 비활성 인터페이스 제외

		rxTotal += row.InOctets;
		txTotal += row.OutOctets;
	}
	::FreeMibTable(ifTable);

	auto now = std::chrono::steady_clock::now();
	if (_hasPrevNetSample)
	{
		double elapsedSec = std::chrono::duration<double>(now - _prevNetSampleTime).count();
		if (elapsedSec > 0.0 && rxTotal >= _prevRxBytes && txTotal >= _prevTxBytes)
		{
			rxBytesPerSec = static_cast<uint64>((rxTotal - _prevRxBytes) / elapsedSec);
			txBytesPerSec = static_cast<uint64>((txTotal - _prevTxBytes) / elapsedSec);
		}
	}

	_prevRxBytes = rxTotal;
	_prevTxBytes = txTotal;
	_prevNetSampleTime = now;
	_hasPrevNetSample = true;
}

uint64 ResourceCollector::GetMemTotal()
{
	MEMORYSTATUSEX statex;
	statex.dwLength = sizeof(statex);
	if (!::GlobalMemoryStatusEx(&statex))
		throw std::runtime_error("ResourceCollector::GetMemTotal - GlobalMemoryStatusEx 실패");
	return statex.ullTotalPhys;
}

uint64 ResourceCollector::GetMemUsed()
{
	MEMORYSTATUSEX statex;
	statex.dwLength = sizeof(statex);
	if (!::GlobalMemoryStatusEx(&statex))
		throw std::runtime_error("ResourceCollector::GetMemUsed - GlobalMemoryStatusEx 실패");
	return statex.ullTotalPhys - statex.ullAvailPhys;
}

void ResourceCollector::GetDiskUsage(uint64& totalBytes, uint64& usedBytes, const String& path)
{
	ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
	if (!::GetDiskFreeSpaceExA(path.c_str(), &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes))
		throw std::runtime_error("ResourceCollector::GetDiskUsage - GetDiskFreeSpaceExA 실패: " + path);

	totalBytes = totalNumberOfBytes.QuadPart;
	usedBytes = totalBytes - totalNumberOfFreeBytes.QuadPart;
}

#else   // Linux

double ResourceCollector::ComputeCpuUsage()
{
	std::ifstream file("/proc/stat");
	if (!file.is_open())
	{
		throw std::runtime_error("ResourceCollector::ComputeCpuUsage - /proc/stat 열기 실패");
	}

	String line;
	std::getline(file, line);   // 첫줄 : "cpu  user nice system idle iowait irq softirq steal ..."

	std::istringstream iss(line);
	String label;
	uint64 user, nice, system, idle, iowait, irq, softirq, steal;
	iss >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

	uint64 idleAll = idle + iowait;
	uint64 totalAll = user + nice + system + idle + iowait + irq + softirq + steal;

	double usage = 0.0;
	if (_hasPrevSample && totalAll > _prevTotal)
	{
		uint64 idleDelta = idleAll - _prevIdle;
		uint64 totalDelta = totalAll - _prevTotal;
		usage = 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
	}

	_prevIdle = idleAll;
	_prevTotal = totalAll;
	_hasPrevSample = true;
	return usage;
}

void ResourceCollector::ComputeNetworkUsage(uint64& rxBytesPerSec, uint64& txBytesPerSec)
{
	std::ifstream file("/proc/net/dev");
	if (!file.is_open())
	{
		throw std::runtime_error("ResourceCollector::ComputeNetworkUsage - /proc/net/dev 열기 실패");
	}

	String line;
	std::getline(file, line);   // 헤더 1
	std::getline(file, line);   // 헤더 2

	uint64 rxTotal = 0, txTotal = 0;

	while (std::getline(file, line))
	{
		size_t colonPos = line.find(':');
		if (colonPos == String::npos)
			continue;

		String ifaceName = line.substr(0, colonPos);
		ifaceName.erase(0, ifaceName.find_first_not_of(" \t"));   // 앞 공백 제거
		if (ifaceName == "lo")
			continue;   // 루프백은 실제 네트워크 트래픽이 아님

		std::istringstream iss(line.substr(colonPos + 1));
		uint64 fields[16] = { 0, };
		for (int i = 0; i < 16; ++i)
			iss >> fields[i];

		rxTotal += fields[0];   // receive bytes
		txTotal += fields[8];   // transmit bytes
	}

	auto now = std::chrono::steady_clock::now();

	if (_hasPrevNetSample)
	{
		double elapsedSec = std::chrono::duration<double>(now - _prevNetSampleTime).count();
		if (elapsedSec > 0.0 && rxTotal >= _prevRxBytes && txTotal >= _prevTxBytes)
		{
			rxBytesPerSec = static_cast<uint64>((rxTotal - _prevRxBytes) / elapsedSec);
			txBytesPerSec = static_cast<uint64>((txTotal - _prevTxBytes) / elapsedSec);
		}
	}

	_prevRxBytes = rxTotal;
	_prevTxBytes = txTotal;
	_prevNetSampleTime = now;
	_hasPrevNetSample = true;
}

uint64 ResourceCollector::GetMemTotal()
{
	std::ifstream file("/proc/meminfo");
	String line;
	while (std::getline(file, line))
	{
		if (line.rfind("MemTotal:", 0) == 0)
		{
			std::istringstream iss(line);
			String label;
			uint64 kb = 0;
			iss >> label >> kb;
			return kb * 1024;
		}
	}

	throw std::runtime_error("ResourceCollector::GetMemTotal - /proc/meminfo에서 MemTotal을 찾을 수 없음");
}

uint64 ResourceCollector::GetMemUsed()
{
	std::ifstream file("/proc/meminfo");
	String line;
	uint64 total = 0, available = 0;

	while (std::getline(file, line))
	{
		std::istringstream iss(line);
		String label;
		uint64 kb = 0;
		iss >> label >> kb;

		if (label == "MemTotal:")
			total = kb;
		else if (label == "MemAvailable:")
			available = kb;
	}
	if (total == 0)
		throw std::runtime_error("ResourceCollector::GetMemUsed - /proc/meminfo 파싱 실패");

	return (total - available) * 1024;
}

void ResourceCollector::GetDiskUsage(uint64& totalBytes, uint64& usedBytes, const String& path)
{
	struct statvfs stat;
	if (::statvfs(path.c_str(), &stat) != 0)
	{
		throw std::runtime_error("ResourceCollector::GetDiskUsage - statvfs 실패: " + path);
	}
	totalBytes = static_cast<uint64>(stat.f_blocks) * stat.f_frsize;
	uint64 freeBytes = static_cast<uint64>(stat.f_bfree) * stat.f_frsize;
	usedBytes = totalBytes - freeBytes;
}

#endif
