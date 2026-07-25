#pragma once
#include "pch.h"

/*-----------------
	PrivilegeDrop
-------------------*/
// 포트 바인딩 등 root가 필요한 초기화 이후, 즉시 비특권 사용자로 권한을 하향한다.
// 목적: 프로세스가 침해당했을 때 root 권한으로 실행 중인 것보다 피해 범위를 최소화.
// Linux 전용(POSIX setuid/setgid). Windows는 서비스 계정을 최소 권한으로 구성하는
// 방식이라 이 클래스의 대상이 아님(README 참고).
// 주의: 멀티스레드 프로그램이라면 다른 스레드를 만들기 전에 반드시 호출해야 함
// (setuid는 호출한 스레드에만 적용되는 게 아니라 프로세스 전체 자격증명을 바꾸지만,
//  이미 시작된 스레드가 특권 상태에서 뭔가를 캐싱해뒀을 가능성을 배제하려면 순서가 중요).

class PrivilegeDrop
{
public:
	// targetUser가 시스템에 존재하는 계정이어야 함(예: "nobody").
	// 이미 root가 아니면(euid != 0) 아무 것도 하지 않고 조용히 반환.
	static void DropTo(const String& targetUser);
};