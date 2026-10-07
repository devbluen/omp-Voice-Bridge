/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "samp-world.hpp"
#include "../log.hpp"
#include <algorithm>
#include <cstring>

namespace vbs::samp
{
namespace
{
float toFloat(cell value)
{
	float result;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

struct Wanted
{
	const char* name;
	AMX_NATIVE World::*slot;
};
}

void World::onAmxLoad(AMX* amx)
{
	if (!amx || std::find(scripts_.begin(), scripts_.end(), amx) != scripts_.end())
	{
		return;
	}
	scripts_.push_back(amx);
	resolve(amx);
}

void World::onAmxUnload(AMX* amx)
{
	scripts_.erase(std::remove(scripts_.begin(), scripts_.end(), amx), scripts_.end());
}

void World::resolve(AMX* amx)
{
	if (!amx->base)
	{
		return;
	}
	const auto* header = reinterpret_cast<const AMX_HEADER*>(amx->base);
	if (header->defsize <= 0 || header->libraries < header->natives)
	{
		return;
	}
	const int count = (header->libraries - header->natives) / header->defsize;
	for (int i = 0; i < count; ++i)
	{
		const auto* entry = reinterpret_cast<const AMX_FUNCSTUB*>(amx->base + header->natives + i * header->defsize);
		const char* name = nullptr;
		if (header->defsize == static_cast<int>(sizeof(AMX_FUNCSTUBNT)))
		{
			name = reinterpret_cast<const char*>(amx->base + reinterpret_cast<const AMX_FUNCSTUBNT*>(entry)->nameofs);
		}
		else
		{
			name = entry->name;
		}
		if (!name || !entry->address)
		{
			continue;
		}
		offer(name, reinterpret_cast<AMX_NATIVE>(static_cast<uintptr_t>(entry->address)));
	}
	if (!warned_ && (!isPlayerConnected_ || !getPlayerPos_))
	{
		warned_ = true;
		LogWarning("IsPlayerConnected/GetPlayerPos were not found in the loaded script. Include <voice-bridge> or <sampvoice>"
			" from Voice Bridge in the gamemode so dynamic streams can track players.");
	}
}

void World::addNatives(const AMX_NATIVE_INFO* list, int count)
{
	// A negative count means the list ends with a null entry (the SA-MP
	// server registers its natives that way).
	for (int i = 0; list && (count < 0 || i < count); ++i)
	{
		if (!list[i].name)
		{
			break;
		}
		if (list[i].func)
		{
			offer(list[i].name, list[i].func);
		}
	}
}

void World::offer(const char* name, AMX_NATIVE native)
{
	{
		static const Wanted wanted[] = {
			{ "IsPlayerConnected", &World::isPlayerConnected_ },
			{ "IsPlayerNPC", &World::isPlayerNpc_ },
			{ "GetMaxPlayers", &World::getMaxPlayers_ },
			{ "GetPlayerPos", &World::getPlayerPos_ },
			{ "GetPlayerVirtualWorld", &World::getPlayerVirtualWorld_ },
			{ "GetPlayerInterior", &World::getPlayerInterior_ },
			{ "GetPlayerName", &World::getPlayerName_ },
			{ "GetVehiclePos", &World::getVehiclePos_ },
			{ "GetVehicleVirtualWorld", &World::getVehicleVirtualWorld_ },
			{ "GetObjectPos", &World::getObjectPos_ },
			{ "IsValidObject", &World::isValidObject_ },
		};
		for (const Wanted& item : wanted)
		{
			if (!(this->*item.slot) && std::strcmp(name, item.name) == 0)
			{
				this->*item.slot = native;
			}
		}
	}
}

cell World::call(AMX_NATIVE native, std::initializer_list<cell> arguments)
{
	AMX* script = amx();
	if (!native || !script)
	{
		return 0;
	}
	cell params[8] {};
	params[0] = static_cast<cell>(arguments.size() * sizeof(cell));
	std::size_t index = 1;
	for (cell argument : arguments)
	{
		params[index++] = argument;
	}
	return native(script, params);
}

bool World::position(AMX_NATIVE native, cell id, vb::Vec3& out)
{
	AMX* script = amx();
	if (!native || !script)
	{
		return false;
	}
	cell address = 0;
	cell* physical = nullptr;
	if (amx_Allot(script, 3, &address, &physical) != AMX_ERR_NONE || !physical)
	{
		return false;
	}
	cell params[5] = { 4 * static_cast<cell>(sizeof(cell)), id, address, address + static_cast<cell>(sizeof(cell)),
		address + 2 * static_cast<cell>(sizeof(cell)) };
	const cell ok = native(script, params);
	out.x = toFloat(physical[0]);
	out.y = toFloat(physical[1]);
	out.z = toFloat(physical[2]);
	amx_Release(script, address);
	return ok != 0;
}

bool World::isPlayerConnected(uint16_t player)
{
	return call(isPlayerConnected_, { player }) != 0;
}

bool World::isPlayerNpc(uint16_t player)
{
	return isPlayerNpc_ && call(isPlayerNpc_, { player }) != 0;
}

int World::maxPlayers()
{
	const cell value = getMaxPlayers_ ? call(getMaxPlayers_, {}) : 0;
	return value > 0 && value <= kMaxPlayers ? static_cast<int>(value) : kMaxPlayers;
}

bool World::playerPose(uint16_t player, Pose& out)
{
	if (!position(getPlayerPos_, player, out.position))
	{
		return false;
	}
	out.world = static_cast<int>(call(getPlayerVirtualWorld_, { player }));
	out.interior = static_cast<int>(call(getPlayerInterior_, { player }));
	return true;
}

bool World::vehiclePose(uint16_t vehicle, Pose& out)
{
	if (!position(getVehiclePos_, vehicle, out.position))
	{
		return false;
	}
	out.world = static_cast<int>(call(getVehicleVirtualWorld_, { vehicle }));
	out.interior = kAnyWorld;
	return true;
}

bool World::objectPose(uint16_t object, Pose& out)
{
	if (isValidObject_ && call(isValidObject_, { object }) == 0)
	{
		return false;
	}
	if (!position(getObjectPos_, object, out.position))
	{
		return false;
	}
	out.world = kAnyWorld;
	out.interior = kAnyWorld;
	return true;
}

bool World::playerName(uint16_t player, std::string& out)
{
	AMX* script = amx();
	if (!getPlayerName_ || !script)
	{
		return false;
	}
	constexpr cell kLength = 25;
	cell address = 0;
	cell* physical = nullptr;
	if (amx_Allot(script, kLength, &address, &physical) != AMX_ERR_NONE || !physical)
	{
		return false;
	}
	physical[0] = 0;
	cell params[4] = { 3 * static_cast<cell>(sizeof(cell)), player, address, kLength };
	const cell length = getPlayerName_(script, params);
	char buffer[kLength + 1] {};
	amx_GetString(buffer, physical, 0, sizeof(buffer));
	amx_Release(script, address);
	if (length <= 0 || !buffer[0])
	{
		return false;
	}
	out = buffer;
	return true;
}
}
