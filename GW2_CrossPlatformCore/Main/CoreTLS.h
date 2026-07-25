// 변경 전
extern thread_local uint32              LThreadId;
extern thread_local uint64              LEndTickCount;
extern thread_local std::stack<int32>   LLockStack;
extern thread_local class JobQueue* LCurrentJobQueue;

// 변경 후
extern thread_local uint32              LThreadId;
extern thread_local uint64              LEndTickCount;
extern thread_local std::stack<int32>   LLockStack;
extern thread_local class JobQueue* LCurrentJobQueue;