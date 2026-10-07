/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "samp-raknet.hpp"
#include "memory.hpp"
#include "../log.hpp"
#include <vb-protocol.hpp>
#include <array>
#include <cstring>
#include <vector>

#if defined(_M_IX86) || defined(__i386__)
	#define VB_SAMP_HOOKS 1
#else
	#define VB_SAMP_HOOKS 0
#endif

namespace vbs::samp
{
namespace
{
#pragma pack(push, 1)
struct PlayerID
{
	uint32_t binaryAddress;
	uint16_t port;
};

struct Packet
{
	uint16_t playerIndex;
	PlayerID playerId;
	uint32_t length;
	uint32_t bitSize;
	uint8_t* data;
	bool deleteData;
};

struct RPCParameters
{
	uint8_t* input;
	uint32_t numberOfBitsOfData;
	PlayerID sender;
};
#pragma pack(pop)

// Same memory layout as RakNet::BitStream in the SA-MP server.
struct RakBitStream
{
	int numberOfBitsUsed;
	int numberOfBitsAllocated;
	int readOffset;
	uint8_t* data;
	bool copyData;
	uint8_t stackData[256];
};

using RPCFunction = void (*)(RPCParameters*);

constexpr uint16_t kMaxPlayers = 1000;
constexpr int kHighPriority = 1;
constexpr int kUnreliableSequenced = 7;
constexpr int kReliableOrdered = 9;

#ifdef _WIN32
	#define VB_THISCALL __thiscall
	#define VB_HOOKCALL __fastcall
	#define VB_HOOK_THIS void* self, void* /* edx */
constexpr std::size_t kSendIndex = 7;
constexpr std::size_t kReceiveIndex = 10;
constexpr std::size_t kDeallocateIndex = 12;
constexpr std::size_t kRegisterRpcIndex = 29;
constexpr std::size_t kGetIndexIndex = 57;
const char* const kFactoryPattern = "\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x51"
									"\x68\x18\x0E\x00\x00\xE8\xFF\xFF\xFF\xFF\x83\xC4\x04\x89\x04"
									"\x24\x85\xC0\xC7\x44\x24\xFF\x00\x00\x00\x00\x74\x16";
const char* const kFactoryMask = "xxxxxxxxxxxxxxxx????x????xxxxxxxxxxx?xxxxxx";
#else
	#define VB_THISCALL
	#define VB_HOOKCALL
	#define VB_HOOK_THIS void* self
constexpr std::size_t kSendIndex = 9;
constexpr std::size_t kReceiveIndex = 11;
constexpr std::size_t kDeallocateIndex = 13;
constexpr std::size_t kRegisterRpcIndex = 30;
constexpr std::size_t kGetIndexIndex = 58;
const char* const kFactoryPattern = "\x04\x24\xFF\xFF\xFF\xFF\x89\x75\xFF\x89\x5D\xFF\xE8\xFF\xFF"
									"\xFF\xFF\x89\x04\x24\x89\xC6\xE8\xFF\xFF\xFF\xFF\x89\xF0\x8B"
									"\x5D\xFF\x8B\x75\xFF\x89\xEC\x5D\xC3";
const char* const kFactoryMask = "xx????xx?xx?x????xxxxxx????xxxx?xx?xxxx";
#endif

using SendFn = bool(VB_THISCALL*)(void*, RakBitStream*, int, int, char, PlayerID, bool);
using ReceiveFn = Packet*(VB_THISCALL*)(void*);
using DeallocateFn = void(VB_THISCALL*)(void*, Packet*);
using RegisterRpcFn = void(VB_THISCALL*)(void*, uint8_t*, RPCFunction);
using GetIndexFn = int(VB_THISCALL*)(void*, PlayerID);

Callbacks g_callbacks;
mem::JumpHook g_factoryHook;
void* g_rak = nullptr;
SendFn g_send = nullptr;
ReceiveFn g_receive = nullptr;
DeallocateFn g_deallocate = nullptr;
RegisterRpcFn g_registerRpc = nullptr;
GetIndexFn g_getIndex = nullptr;
RPCFunction g_originalJoin = nullptr;
bool g_registerHooked = false;
uint8_t g_joinId = vb::kRpcClientJoin;

std::array<PlayerID, kMaxPlayers> g_addresses {};
std::array<bool, kMaxPlayers> g_known {};

void remember(uint16_t index, const PlayerID& id)
{
	if (index < kMaxPlayers && id.binaryAddress && id.binaryAddress != 0xFFFFFFFFu)
	{
		g_addresses[index] = id;
		g_known[index] = true;
	}
}

void forget(uint16_t index)
{
	if (index < kMaxPlayers)
	{
		g_known[index] = false;
	}
}

void JoinHook(RPCParameters* parameters)
{
	if (parameters && g_getIndex && g_rak)
	{
		const int index = g_getIndex(g_rak, parameters->sender);
		if (index >= 0 && index < kMaxPlayers)
		{
			remember(static_cast<uint16_t>(index), parameters->sender);
			if (g_callbacks.join)
			{
				const std::size_t bytes = (parameters->numberOfBitsOfData + 7) / 8;
				g_callbacks.join(static_cast<uint16_t>(index), parameters->input, bytes);
			}
		}
	}
	if (g_originalJoin)
	{
		g_originalJoin(parameters);
	}
}

void VB_HOOKCALL RegisterRpcHook(VB_HOOK_THIS, uint8_t* id, RPCFunction function)
{
	if (id && *id == vb::kRpcClientJoin && function && function != JoinHook)
	{
		g_originalJoin = function;
		function = JoinHook;
		LogDebug("client join RPC wrapped");
	}
	g_registerRpc(self, id, function);
}

Packet* VB_HOOKCALL ReceiveHook(VB_HOOK_THIS)
{
	for (;;)
	{
		Packet* packet = g_receive(self);
		if (!packet || !packet->data || packet->length == 0)
		{
			return packet;
		}
		const uint8_t id = packet->data[0];
		if (id == vb::kRakPacketId)
		{
			remember(packet->playerIndex, packet->playerId);
			if (g_callbacks.packet && packet->length > 1)
			{
				g_callbacks.packet(packet->playerIndex, packet->data + 1, packet->length - 1);
			}
			g_deallocate(self, packet);
			continue;
		}
		if (id == vb::kIdDisconnectionNotification || id == vb::kIdConnectionLost)
		{
			if (g_callbacks.disconnect)
			{
				g_callbacks.disconnect(packet->playerIndex);
			}
			forget(packet->playerIndex);
		}
		else
		{
			remember(packet->playerIndex, packet->playerId);
		}
		return packet;
	}
}

bool attach(void* rak, bool hookRegistration)
{
	if (!rak || g_rak)
	{
		return g_rak == rak;
	}
	void** table = *static_cast<void***>(rak);
	g_send = reinterpret_cast<SendFn>(table[kSendIndex]);
	g_deallocate = reinterpret_cast<DeallocateFn>(table[kDeallocateIndex]);
	g_getIndex = reinterpret_cast<GetIndexFn>(table[kGetIndexIndex]);
	g_receive = reinterpret_cast<ReceiveFn>(mem::PatchVirtual(rak, kReceiveIndex, reinterpret_cast<void*>(&ReceiveHook)));
	if (hookRegistration)
	{
		g_registerRpc = reinterpret_cast<RegisterRpcFn>(mem::PatchVirtual(rak, kRegisterRpcIndex, reinterpret_cast<void*>(&RegisterRpcHook)));
		g_registerHooked = true;
	}
	g_rak = rak;
	return true;
}

void* FactoryHook()
{
	void* target = g_factoryHook.target();
	g_factoryHook.remove();
	void* rak = reinterpret_cast<void* (*)()>(target)();
	if (rak)
	{
		attach(rak, true);
	}
	return rak;
}
}

void SetCallbacks(Callbacks callbacks)
{
	g_callbacks = std::move(callbacks);
}

bool InstallFactoryHook(const void* serverAddress)
{
#if VB_SAMP_HOOKS
	uint8_t* start = nullptr;
	std::size_t size = 0;
	if (!mem::CodeRange(serverAddress, start, size))
	{
		LogWarning("could not read the server executable");
		return false;
	}
	uint8_t* found = mem::FindPattern(start, size, kFactoryPattern, kFactoryMask);
	if (!found)
	{
		return false;
	}
	return g_factoryHook.install(found - 7, reinterpret_cast<void*>(&FactoryHook));
#else
	(void)serverAddress;
	return false;
#endif
}

bool AttachTo(void* rakServer)
{
#if VB_SAMP_HOOKS
	return attach(rakServer, false);
#else
	(void)rakServer;
	return false;
#endif
}

void Uninstall()
{
	g_factoryHook.remove();
	if (!g_rak)
	{
		return;
	}
	void** table = *static_cast<void***>(g_rak);
	if (table[kReceiveIndex] == reinterpret_cast<void*>(&ReceiveHook))
	{
		mem::PatchVirtual(g_rak, kReceiveIndex, reinterpret_cast<void*>(g_receive));
	}
	if (g_registerHooked && table[kRegisterRpcIndex] == reinterpret_cast<void*>(&RegisterRpcHook))
	{
		mem::PatchVirtual(g_rak, kRegisterRpcIndex, reinterpret_cast<void*>(g_registerRpc));
	}
	if (g_originalJoin && g_registerRpc)
	{
		g_registerRpc(g_rak, &g_joinId, g_originalJoin);
	}
	g_rak = nullptr;
	g_originalJoin = nullptr;
	g_registerHooked = false;
	g_known.fill(false);
}

bool Ready()
{
	return g_rak != nullptr;
}

bool HasJoinHook()
{
	return g_originalJoin != nullptr;
}

bool SendPacket(uint16_t player, const uint8_t* data, std::size_t size, bool reliable)
{
	if (!g_rak || !g_send || player >= kMaxPlayers || !g_known[player] || !data || size == 0)
	{
		return false;
	}
	RakBitStream stream {};
	stream.numberOfBitsUsed = static_cast<int>(size * 8);
	stream.numberOfBitsAllocated = static_cast<int>(size * 8);
	stream.readOffset = 0;
	stream.data = const_cast<uint8_t*>(data);
	stream.copyData = false;
	return g_send(g_rak, &stream, kHighPriority, reliable ? kReliableOrdered : kUnreliableSequenced, 0, g_addresses[player], false);
}

uint32_t PlayerIp(uint16_t player)
{
	return player < kMaxPlayers && g_known[player] ? g_addresses[player].binaryAddress : 0;
}
}
