#pragma once
#include <cstdint>

using BYTE = uint8_t;

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

template<typename T>
using Atomic = std::atomic<T>;

using Mutex = std::mutex;
using CondVar = std::condition_variable;
using UniqueLock = std::unique_lock<std::mutex>;
using LockGuard = std::lock_guard<std::mutex>;

#define USING_SHARED_PTR(name) using name##Ref = std::shared_ptr<class name>;

USING_SHARED_PTR(Session);
USING_SHARED_PTR(PacketSession);
USING_SHARED_PTR(Listener);
USING_SHARED_PTR(ServerService);
USING_SHARED_PTR(ClientService);
USING_SHARED_PTR(SendBuffer);
USING_SHARED_PTR(SendBufferChunk);
USING_SHARED_PTR(Job);
USING_SHARED_PTR(JobQueue);
USING_SHARED_PTR(IoCore);       // IocpCoreRef ¥Î√º

#define size16(val)     static_cast<int16>(sizeof(val))
#define size32(val)     static_cast<int32>(sizeof(val))
#define len16(arr)      static_cast<int16>(sizeof(arr)/sizeof(arr[0]))
#define len32(arr)      static_cast<int32>(sizeof(arr)/sizeof(arr[0]))