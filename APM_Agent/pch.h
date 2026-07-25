#pragma once

#ifdef _WIN32
// asio는 _WIN32_WINNT 미정의 시 Windows 7(0x0601)을 기본 타겟으로 가정함 -
// ApmSession::GetConnectionInfo()의 SIO_TCP_INFO/TCP_INFO_v0(mstcpip.h)는
// Windows 10 이상 전용이라 이 기본값 아래서는 헤더 자체에서 선언이 숨겨짐.
// 반드시 asio.hpp include 이전에 정의해야 함.
#define _WIN32_WINNT 0x0A00
#endif

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array>
#include <vector>
#include <deque>
#include <unordered_map>
#include <string>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <functional>
#include <cstring>

#include "Types.h"
#include "Container.h"
