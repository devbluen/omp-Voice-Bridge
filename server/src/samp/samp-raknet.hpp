/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  SA-MP 0.3.7 / 0.3.DL server networking.  The RakServer instance is taken
 *  from RakNetworkFactory::GetRakServerInterface (pattern hook) so the
 *  client join RPC can be wrapped before the server registers it.  When the
 *  pattern is not found the instance is taken from the plugin data table
 *  instead; packets still work but the join RPC cannot be read.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace vbs::samp
{
struct Callbacks
{
	std::function<void(uint16_t player, const uint8_t* data, std::size_t size)> join;
	std::function<void(uint16_t player, const uint8_t* data, std::size_t size)> packet;
	std::function<void(uint16_t player)> disconnect;
};

void SetCallbacks(Callbacks callbacks);

// Hooks GetRakServerInterface.  `serverAddress` is any address inside the
// server executable (logprintf).
bool InstallFactoryHook(const void* serverAddress);
// Hooks an already created RakServer (PLUGIN_DATA_RAKSERVER).
bool AttachTo(void* rakServer);
void Uninstall();

bool Ready();
bool HasJoinHook();

bool SendPacket(uint16_t player, const uint8_t* data, std::size_t size, bool reliable);
uint32_t PlayerIp(uint16_t player);
}
