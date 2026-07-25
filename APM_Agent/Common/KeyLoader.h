#pragma once
#include "pch.h"
#include <array>

/*------------
	KeyLoader
--------------*/
// hex 문자열(64자 = 32바이트) 파일을 읽어 대칭키로 변환.
// AriaCipher::Key/HmacUtil::Key 둘 다 std::array<BYTE,32>라 이 함수 하나로 공용.

std::array<BYTE, 32> LoadKeyFromHexFile(const String& path);
