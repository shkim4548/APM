#pragma once
#include <atomic>

// step Phase A : "로그 레벨 조정" 명령의 최소 구현. 전역 레벨 하나만 두고,
// 지금은 LocalAlertEvaluator의 로그 한 곳에만 적용해 동작을 증명한다.
// 기존 코드 전체(std::cout/cerr 직접 호출)를 이 체계로 옮기는 건 범위 밖 -
// 모든 로그 호출부를 다 고쳐야 해서 Phase A 취지(빠르게 검증)에 안 맞음.
enum class LogLevel : int { Error = 0, Warning = 1, Info = 2, Debug = 3 };

void SetLogLevel(int level);
LogLevel GetLogLevel();
