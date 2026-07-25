#pragma once

// Asio �ݵ�� �ֻ��
#define ASIO_STANDALONE
#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#endif
#include "asio.hpp"

// ǥ�� ���̺귯��
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <list>
#include <queue>
#include <stack>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

using namespace std;

// �÷����� �ּ� ���
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#endif

// ������Ʈ ��� ��� (���� ���� ����)
#include "Types.h"
#include "Container.h"
#include "CoreMacro.h"
#include "CoreGlobal.h"
#include "CoreTLS.h"
#include "Lock.h"
#include "ObjectPool.h"
#include "LockQueue.h"
#include "JobTimer.h"
#include "JobQueue.h"
#include "SendBuffer.h"
#include "Session.h"
#include "ConsoleLog.h"