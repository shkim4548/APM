#include "pch.h"
#include "PacketHandler.h"

std::unordered_map<uint16, PacketHandler::RawHandler>& PacketHandler::Handlers()
{
	static std::unordered_map<uint16, RawHandler> handlers;
	return handlers;
}

void PacketHandler::Dispatch(uint16 id, const String& payload)
{
	auto& handlers = Handlers();
	auto it = handlers.find(id);
	if (it == handlers.end())
	{
		std::cerr << "[PacketHandler] no handler registered for packet id=" << id << std::endl;
		return;
	}

	it->second(payload);
}
