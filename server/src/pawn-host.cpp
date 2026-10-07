/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "pawn-host.hpp"
#include "log.hpp"
#include <algorithm>
#include <iterator>

extern void* pAMXFunctions; // amx-functions.cpp

namespace vbs
{
void SetAmxFunctionTable(void* table)
{
	pAMXFunctions = table;
}

PawnHost& PawnHost::Get()
{
	static PawnHost instance;
	return instance;
}

void PawnHost::addScript(AMX* amx)
{
	if (amx && std::find(scripts_.begin(), scripts_.end(), amx) == scripts_.end())
	{
		scripts_.push_back(amx);
	}
}

void PawnHost::removeScript(AMX* amx)
{
	scripts_.erase(std::remove(scripts_.begin(), scripts_.end(), amx), scripts_.end());
}

void PawnHost::clear()
{
	scripts_.clear();
}

void PawnHost::call(const char* name, std::initializer_list<cell> arguments)
{
	// A callback may load or unload scripts, so iterate over a copy.
	const std::vector<AMX*> scripts = scripts_;
	for (AMX* amx : scripts)
	{
		if (std::find(scripts_.begin(), scripts_.end(), amx) == scripts_.end())
		{
			continue;
		}
		int index = 0;
		if (amx_FindPublic(amx, name, &index) != AMX_ERR_NONE)
		{
			continue;
		}
		for (auto it = std::rbegin(arguments); it != std::rend(arguments); ++it)
		{
			amx_Push(amx, *it);
		}
		cell result = 0;
		const int error = amx_Exec(amx, &result, index);
		if (error != AMX_ERR_NONE)
		{
			LogWarning("AMX error %d while running %s", error, name);
		}
	}
}

void PawnHost::onActivationKey(uint16_t player, uint8_t key, bool pressed)
{
	call(pressed ? "OnPlayerActivationKeyPress" : "OnPlayerActivationKeyRelease", { static_cast<cell>(player), static_cast<cell>(key) });
}

void PawnHost::onHandshake(uint16_t player, uint8_t version, bool extended, bool micro)
{
	call("VB_OnPlayerClientDetected", { static_cast<cell>(player), static_cast<cell>(version), static_cast<cell>(extended), static_cast<cell>(micro) });
}

void PawnHost::onTransport(uint16_t player, uint8_t transport)
{
	call("VB_OnPlayerTransportChange", { static_cast<cell>(player), static_cast<cell>(transport) });
}

void PawnHost::onTalking(uint16_t player, bool talking)
{
	call(talking ? "VB_OnPlayerStartTalking" : "VB_OnPlayerStopTalking", { static_cast<cell>(player) });
}

void PawnHost::onClientStatus(uint16_t player, bool micAvailable, bool micMuted, bool soundMuted)
{
	call("VB_OnPlayerClientStatus", { static_cast<cell>(player), static_cast<cell>(micAvailable), static_cast<cell>(micMuted), static_cast<cell>(soundMuted) });
}
}
