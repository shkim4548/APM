#include "pch.h"
#include "KeyLoader.h"
#include <fstream>

std::array<BYTE, 32> LoadKeyFromHexFile(const String& path)
{
	std::ifstream file(path);
	if (!file.is_open())
		throw std::runtime_error("LoadKeyFromHexFile - 파일 열기 실패: " + path);

	String hex;
	file >> hex;

	if (hex.size() != 64)
		throw std::runtime_error("LoadKeyFromHexFile - 키 길이가 32바이트(64 hex 문자)가 아님: " + path);

	std::array<BYTE, 32> key{};
	for (size_t i = 0; i < 32; ++i)
	{
		String byteStr = hex.substr(i * 2, 2);
		key[i] = static_cast<BYTE>(std::stoul(byteStr, nullptr, 16));
	}

	return key;
}
