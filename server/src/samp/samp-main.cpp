/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  SA-MP plugin entry points.  The same binary is also an open.mp
 *  component; when open.mp loads it through ComponentEntryPoint these
 *  functions are never called.
 */

#include "memory.hpp"
#include "samp-raknet.hpp"
#include "samp-world.hpp"
#include "../config.hpp"
#include "../log.hpp"
#include "../loader.hpp"
#include "../pawn-host.hpp"
#include "../voice-server.hpp"
#include "version.hpp"
#include <amx/amx.h>
#include <plugincommon.h>
#include <array>
#include <chrono>

using namespace vbs;

namespace
{
using logprintf_t = void (*)(const char* format, ...);

void** g_data = nullptr;
logprintf_t g_logprintf = nullptr;
samp::World g_world;
bool g_active = false;
bool g_fallbackTried = false;
std::array<bool, kMaxPlayers> g_connected {};
uint64_t g_lastPoll = 0;
mem::JumpHook g_registerHook;

// Sees every native the server registers on a script, including the ones the
// script never calls (old SampVoice gamemodes do not reference GetVehiclePos...).
int AMXAPI registerHook(AMX* amx, const AMX_NATIVE_INFO* list, int number)
{
	g_world.addNatives(list, number);
	void* target = g_registerHook.target();
	g_registerHook.remove();
	using RegisterFn = int(AMXAPI*)(AMX*, const AMX_NATIVE_INFO*, int);
	const int result = reinterpret_cast<RegisterFn>(target)(amx, list, number);
	g_registerHook.install(target, reinterpret_cast<void*>(&registerHook));
	return result;
}

class Transport final : public ITransport
{
public:
	bool sendPacket(uint16_t player, const uint8_t* data, std::size_t size, bool reliable) override
	{
		return samp::SendPacket(player, data, size, reliable);
	}

	uint32_t playerIp(uint16_t player) override
	{
		return samp::PlayerIp(player);
	}

	bool orderedDelivery() const override
	{
		return true;
	}
} g_transport;

uint64_t nowMs()
{
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void pollPlayers()
{
	if (!g_world.canPollPlayers())
	{
		return;
	}
	VoiceServer& server = VoiceServer::Get();
	const int limit = g_world.maxPlayers();
	for (int id = 0; id < kMaxPlayers; ++id)
	{
		const auto player = static_cast<uint16_t>(id);
		const bool connected = id < limit && g_world.isPlayerConnected(player) && !g_world.isPlayerNpc(player);
		if (connected && !g_connected[id])
		{
			g_connected[id] = true;
			server.onPlayerConnect(player);
		}
		else if (!connected && g_connected[id])
		{
			g_connected[id] = false;
			server.onPlayerDisconnect(player);
		}
		else if (!connected && server.isTracked(player))
		{
			// Joined (RPC 25) but rejected before becoming a player.
			server.onPlayerDisconnect(player);
		}
	}
}

void tryFallbackHook()
{
	if (g_fallbackTried || samp::Ready() || !g_data)
	{
		return;
	}
	g_fallbackTried = true;
	using GetRakServer = void* (*)();
	const auto getRakServer = reinterpret_cast<GetRakServer>(g_data[PLUGIN_DATA_RAKSERVER]);
	void* rak = getRakServer ? getRakServer() : nullptr;
	if (rak && samp::AttachTo(rak))
	{
		VoiceServer::Get().setProbeMode(true);
		LogWarning("this server build was not recognised; running in compatibility mode (voice clients are detected"
			" after they connect instead of during the join)");
	}
	else
	{
		LogError("could not access the server network layer; voice is disabled. Report your server version.");
	}
}
}

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports()
{
	return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void** data)
{
	if (!data)
	{
		return false;
	}
	g_data = data;
	g_logprintf = reinterpret_cast<logprintf_t>(data[PLUGIN_DATA_LOGPRINTF]);
	SetAmxFunctionTable(data[PLUGIN_DATA_AMX_EXPORTS]);
	LogSetPrinter(g_logprintf);
	LogSetMainThread();

	if (LoaderMode() == Loader::Component)
	{
		LogWarning("already loaded as an open.mp component; remove voice-bridge from legacy_plugins/plugins");
		return true;
	}
	SetLoaderMode(Loader::SampPlugin);

	ServerCfgSource source;
	const Config config = LoadConfig(source);

	samp::SetCallbacks({
		[](uint16_t player, const uint8_t* bytes, std::size_t size) { VoiceServer::Get().onClientJoin(player, bytes, size); },
		[](uint16_t player, const uint8_t* bytes, std::size_t size) { VoiceServer::Get().onControlPacket(player, bytes, size); },
		[](uint16_t player)
		{
			if (player < kMaxPlayers && g_connected[player])
			{
				g_connected[player] = false;
			}
			VoiceServer::Get().onPlayerDisconnect(player);
		},
	});

#if defined(_M_IX86) || defined(__i386__)
	g_registerHook.install(static_cast<void**>(data[PLUGIN_DATA_AMX_EXPORTS])[PLUGIN_AMX_EXPORT_Register], reinterpret_cast<void*>(&registerHook));
#endif

	if (!samp::InstallFactoryHook(reinterpret_cast<const void*>(g_logprintf)))
	{
		LogDebug("RakServer factory pattern not found; the fallback is used when the gamemode loads");
	}

	g_active = VoiceServer::Get().start(config, &g_transport, &g_world, &PawnHost::Get());
	LogInfo("Voice Bridge %s loaded (SA-MP plugin)", VOICE_BRIDGE_VERSION);
	return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload()
{
	if (LoaderMode() != Loader::SampPlugin)
	{
		return;
	}
	VoiceServer::Get().stop();
	samp::Uninstall();
	g_registerHook.remove();
	PawnHost::Get().clear();
	LogInfo("Voice Bridge unloaded");
	LogFlush();
	LogSetPrinter(nullptr);
	g_active = false;
}

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX* amx)
{
	if (LoaderMode() != Loader::SampPlugin)
	{
		return AMX_ERR_NONE;
	}
	g_world.onAmxLoad(amx);
	RegisterNatives(amx);
	PawnHost::Get().addScript(amx);
	tryFallbackHook();
	return AMX_ERR_NONE;
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX* amx)
{
	if (LoaderMode() != Loader::SampPlugin)
	{
		return AMX_ERR_NONE;
	}
	PawnHost::Get().removeScript(amx);
	VoiceServer::Get().releaseOwner(amx);
	g_world.onAmxUnload(amx);
	return AMX_ERR_NONE;
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick()
{
	if (LoaderMode() != Loader::SampPlugin || !g_active)
	{
		LogFlush();
		return;
	}
	const uint64_t t = nowMs();
	if (t - g_lastPoll >= 100)
	{
		g_lastPoll = t;
		pollPlayers();
	}
	VoiceServer::Get().tick();
	LogFlush();
}
