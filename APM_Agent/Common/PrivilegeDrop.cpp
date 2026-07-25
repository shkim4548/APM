#include "pch.h"
#include "PrivilegeDrop.h"

#ifdef _WIN32

void PrivilegeDrop::DropTo(const String& targetUser)
{
	// Windows는 이 클래스의 설계 대상이 아님(PrivilegeDrop.h 주석 참고) -
	// 서비스 계정을 최소 권한으로 구성하는 방식은 배포 설계 시점에 별도로 다룸.
	std::cout << "[PrivilegeDrop] Windows에서는 미지원(설계상 범위 밖) - 스킵" << std::endl;
}

#else   // Linux

#include <pwd.h>
#include <unistd.h>
#include <grp.h>
#include <cstring>
#include <cerrno>

void PrivilegeDrop::DropTo(const String& targetUser)
{
	if (::geteuid() != 0)
	{
		std::cout << "[PrivilegeDrop] 이미 비-root 권한으로 실행 중 - 하향 불필요" << std::endl;
		return;
	}

	errno = 0;
	struct passwd* pw = ::getpwnam(targetUser.c_str());
	if (pw == nullptr)
		throw std::runtime_error("PrivilegeDrop::DropTo - 대상 사용자(" + targetUser + ")를 찾을 수 없음");

	// 순서가 중요: uid를 먼저 낮추면 root 권한이 사라져서 이후 setgid/setgroups가 실패한다.
	// 반드시 gid/보조그룹 -> uid 순서로 낮춰야 함.
	if (::setgid(pw->pw_gid) != 0)
		throw std::runtime_error(String("PrivilegeDrop::DropTo - setgid 실패: ") + strerror(errno));

	// 상속된 보조 그룹(supplementary groups)을 명시적으로 제거 - 흔히 빠뜨리는 단계.
	// 이걸 안 하면 uid/gid는 낮아져도 원래 root가 속해있던 그룹 권한이 남아있을 수 있음.
	if (::setgroups(0, nullptr) != 0)
		throw std::runtime_error(String("PrivilegeDrop::DropTo - setgroups 실패: ") + strerror(errno));

	if (::setuid(pw->pw_uid) != 0)
		throw std::runtime_error(String("PrivilegeDrop::DropTo - setuid 실패: ") + strerror(errno));

	// 검증: 권한 하향이 "되돌릴 수 없는" 상태인지 확인.
	// setuid(0)이 성공해버리면 saved-set-uid가 여전히 0으로 남아있다는 뜻이라 하향이 불완전한 것.
	if (::setuid(0) == 0)
		throw std::runtime_error("PrivilegeDrop::DropTo - 심각: setuid(0) 재획득이 가능함(권한 하향 실패)");

	std::cout << "[PrivilegeDrop] uid=" << pw->pw_uid << " gid=" << pw->pw_gid
		<< " (" << targetUser << ")로 권한 하향 완료, root 권한 재획득 불가 확인됨" << std::endl;
}

#endif
