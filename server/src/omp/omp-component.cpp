/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "omp-component.hpp"
#include "../config.hpp"
#include "../loader.hpp"
#include "../log.hpp"
#include "../pawn-host.hpp"
#include "version.hpp"
#include <vb-protocol.hpp>

namespace
{
VoiceBridgeComponent* g_instance = nullptr;

// open.mp only forward declares NetworkBitStream.  Its first members have kept
// this layout since the first release (bitstream version 3).
struct BitStreamView
{
	int numberOfBitsUsed;
	int numberOfBitsAllocated;
	int readOffset;
	unsigned char* data;
};

bool readBitStream(NetworkBitStream& bs, const uint8_t*& data, std::size_t& size)
{
	const auto* view = reinterpret_cast<const BitStreamView*>(&bs);
	if (!view->data || view->numberOfBitsUsed <= 0)
	{
		return false;
	}
	data = view->data;
	size = static_cast<std::size_t>((view->numberOfBitsUsed + 7) / 8);
	return true;
}

class OmpConfigSource final : public vbs::ConfigSource
{
public:
	explicit OmpConfigSource(IConfig& config)
		: config_(config)
	{
	}

	bool getString(const std::string& key, std::string& out) override
	{
		const StringView value = config_.getString(key);
		if (!value.empty())
		{
			out.assign(value.data(), value.length());
			return true;
		}
		return legacy_.getString(key, out);
	}

	bool getInt(const std::string& key, int& out) override
	{
		if (int* value = config_.getInt(key))
		{
			out = *value;
			return true;
		}
		return legacy_.getInt(key, out);
	}

	bool getBool(const std::string& key, bool& out) override
	{
		if (bool* value = config_.getBool(key))
		{
			out = *value;
			return true;
		}
		return legacy_.getBool(key, out);
	}

private:
	IConfig& config_;
	vbs::ServerCfgSource legacy_;
};
}

VoiceBridgeComponent* VoiceBridgeComponent::Get()
{
	if (!g_instance)
	{
		g_instance = new VoiceBridgeComponent();
	}
	return g_instance;
}

StringView VoiceBridgeComponent::componentName() const
{
	return "Voice Bridge";
}

SemanticVersion VoiceBridgeComponent::componentVersion() const
{
	return SemanticVersion(VOICE_BRIDGE_VERSION_MAJOR, VOICE_BRIDGE_VERSION_MINOR, VOICE_BRIDGE_VERSION_PATCH);
}

void VoiceBridgeComponent::onLoad(ICore* core)
{
	core_ = core;
	vbs::SetLoaderMode(vbs::Loader::Component);
	vbs::LogSetCore(core);
	vbs::LogSetMainThread();
	if (!core_)
	{
		return;
	}
	core_->getEventDispatcher().addEventHandler(this);
	core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
	if (core_->getNetworkBitStreamVersion() != 3)
	{
		vbs::LogWarning("unexpected open.mp bitstream version %d; report it if voice clients are not detected", core_->getNetworkBitStreamVersion());
	}
}

void VoiceBridgeComponent::onInit(IComponentList* components)
{
	if (!components || !core_)
	{
		return;
	}
	pawn_ = components->queryComponent<IPawnComponent>();
	vehicles_ = components->queryComponent<IVehiclesComponent>();
	objects_ = components->queryComponent<IObjectsComponent>();
	if (pawn_)
	{
		vbs::SetAmxFunctionTable(const_cast<void**>(pawn_->getAmxFunctions().data()));
		pawn_->getEventDispatcher().addEventHandler(this);
	}
	else
	{
		vbs::LogError("the Pawn component is not loaded; Voice Bridge natives are unavailable");
	}

	// Started here rather than in onReady: the gamemode may be loaded (and
	// create streams in OnGameModeInit) before this component is ready.
	OmpConfigSource source(core_->getConfig());
	const vbs::Config config = vbs::LoadConfig(source);
	active_ = vbs::VoiceServer::Get().start(config, this, this, &vbs::PawnHost::Get());
	started_ = true;
	vbs::LogInfo("Voice Bridge %s loaded (open.mp component) - by " VOICE_BRIDGE_AUTHOR " - " VOICE_BRIDGE_URL, VOICE_BRIDGE_VERSION);
}

void VoiceBridgeComponent::onReady()
{
	if (!core_ || networkHooked_)
	{
		return;
	}
	for (INetwork* network : core_->getNetworks())
	{
		if (network && network->getNetworkType() == ENetworkType_RakNetLegacy)
		{
			network->getInEventDispatcher().addEventHandler(this);
			networkHooked_ = true;
		}
	}
	if (!networkHooked_)
	{
		vbs::LogError("the RakNet (SA-MP) network was not found; voice clients cannot connect");
	}
}

void VoiceBridgeComponent::onFree(IComponent* component)
{
	if (component == pawn_)
	{
		vbs::PawnHost::Get().clear();
		pawn_ = nullptr;
	}
	else if (component == vehicles_)
	{
		vehicles_ = nullptr;
	}
	else if (component == objects_)
	{
		objects_ = nullptr;
	}
}

void VoiceBridgeComponent::provideConfiguration(ILogger&, IEarlyConfig& config, bool defaults)
{
	// Only written when open.mp generates a fresh config.json; otherwise the
	// absence of a key lets server.cfg values (voice_port...) apply.
	if (!defaults)
	{
		return;
	}
	config.setInt("voice_bridge.port", 0);
	config.setString("voice_bridge.bind", "");
	config.setString("voice_bridge.public_host", "");
	config.setBool("voice_bridge.strict_ip", false);
	config.setInt("voice_bridge.bitrate", 24000);
	config.setInt("voice_bridge.frame_ms", 100);
	config.setBool("voice_bridge.tunnel", true);
	config.setBool("voice_bridge.force_tunnel", false);
	config.setBool("voice_bridge.allow_sampvoice", true);
	config.setBool("voice_bridge.allow_voicebridge", true);
	config.setBool("voice_bridge.debug", false);
	config.setString("voice_bridge.log_file", "");
}

void VoiceBridgeComponent::shutdown()
{
	if (!started_)
	{
		return;
	}
	started_ = false;
	vbs::VoiceServer::Get().stop();
	vbs::PawnHost::Get().clear();
	vbs::LogFlush();
}

void VoiceBridgeComponent::free()
{
	shutdown();
	if (core_)
	{
		core_->getEventDispatcher().removeEventHandler(this);
		core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
		for (INetwork* network : core_->getNetworks())
		{
			if (network && network->getNetworkType() == ENetworkType_RakNetLegacy)
			{
				network->getInEventDispatcher().removeEventHandler(this);
			}
		}
	}
	if (pawn_)
	{
		pawn_->getEventDispatcher().removeEventHandler(this);
	}
	vbs::LogSetCore(nullptr);
	g_instance = nullptr;
	delete this;
}

void VoiceBridgeComponent::reset()
{
	// GMX: scripts are unloaded through onAmxUnload, which releases their
	// streams and effects.  Player sessions survive the restart.
}

VoiceBridgeComponent::~VoiceBridgeComponent()
{
	shutdown();
}

void VoiceBridgeComponent::onAmxLoad(IPawnScript& script)
{
	AMX* amx = script.GetAMX();
	if (!amx)
	{
		return;
	}
	vbs::RegisterNatives(amx);
	vbs::PawnHost::Get().addScript(amx);
}

void VoiceBridgeComponent::onAmxUnload(IPawnScript& script)
{
	AMX* amx = script.GetAMX();
	vbs::PawnHost::Get().removeScript(amx);
	vbs::VoiceServer::Get().releaseOwner(amx);
}

void VoiceBridgeComponent::onPlayerConnect(IPlayer& player)
{
	if (!player.isBot())
	{
		vbs::VoiceServer::Get().onPlayerConnect(static_cast<uint16_t>(player.getID()));
	}
}

void VoiceBridgeComponent::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason)
{
	vbs::VoiceServer::Get().onPlayerDisconnect(static_cast<uint16_t>(player.getID()));
}

void VoiceBridgeComponent::onTick(Microseconds, TimePoint)
{
	if (active_)
	{
		vbs::VoiceServer::Get().tick();
	}
	vbs::LogFlush();
}

bool VoiceBridgeComponent::onReceivePacket(IPlayer& peer, int id, NetworkBitStream& bs)
{
	if (id != vb::kRakPacketId)
	{
		return true;
	}
	const uint8_t* data = nullptr;
	std::size_t size = 0;
	if (readBitStream(bs, data, size) && size > 1)
	{
		vbs::VoiceServer::Get().onControlPacket(static_cast<uint16_t>(peer.getID()), data + 1, size - 1);
	}
	return false;
}

bool VoiceBridgeComponent::onReceiveRPC(IPlayer& peer, int id, NetworkBitStream& bs)
{
	if (id != vb::kRpcClientJoin || peer.isBot())
	{
		return true;
	}
	const uint8_t* data = nullptr;
	std::size_t size = 0;
	if (readBitStream(bs, data, size))
	{
		vbs::VoiceServer::Get().onClientJoin(static_cast<uint16_t>(peer.getID()), data, size);
	}
	return true;
}

bool VoiceBridgeComponent::sendPacket(uint16_t player, const uint8_t* data, std::size_t size, bool reliable)
{
	if (!core_)
	{
		return false;
	}
	IPlayer* target = core_->getPlayers().get(player);
	if (!target)
	{
		return false;
	}
	return target->sendPacket(Span<uint8_t>(const_cast<uint8_t*>(data), size * 8),
		reliable ? OrderingChannel_Reliable : OrderingChannel_SyncPacket, false);
}

uint32_t VoiceBridgeComponent::playerIp(uint16_t player)
{
	IPlayer* target = core_ ? core_->getPlayers().get(player) : nullptr;
	if (!target)
	{
		return 0;
	}
	const PeerNetworkData& data = target->getNetworkData();
	return data.networkID.address.ipv6 ? 0 : data.networkID.address.v4;
}

bool VoiceBridgeComponent::playerPose(uint16_t player, vbs::Pose& out)
{
	IPlayer* target = core_ ? core_->getPlayers().get(player) : nullptr;
	if (!target)
	{
		return false;
	}
	Vector3 position = target->getPosition();
	// Inside a vehicle the vehicle is where the player is (same rule as the
	// game itself), whatever the player sync last reported.
	if (vehicles_)
	{
		if (IPlayerVehicleData* data = queryExtension<IPlayerVehicleData>(*target))
		{
			if (IVehicle* vehicle = data->getVehicle())
			{
				position = vehicle->getPosition();
			}
		}
	}
	out.position = { position.x, position.y, position.z };
	out.world = target->getVirtualWorld();
	out.interior = static_cast<int>(target->getInterior());
	return true;
}

bool VoiceBridgeComponent::vehiclePose(uint16_t vehicle, vbs::Pose& out)
{
	IVehicle* target = vehicles_ ? vehicles_->get(vehicle) : nullptr;
	if (!target)
	{
		return false;
	}
	const Vector3 position = target->getPosition();
	out.position = { position.x, position.y, position.z };
	out.world = target->getVirtualWorld();
	out.interior = vbs::kAnyWorld;
	return true;
}

bool VoiceBridgeComponent::objectPose(uint16_t object, vbs::Pose& out)
{
	IObject* target = objects_ ? objects_->get(object) : nullptr;
	if (!target)
	{
		return false;
	}
	const Vector3 position = target->getPosition();
	out.position = { position.x, position.y, position.z };
	out.world = vbs::kAnyWorld;
	out.interior = vbs::kAnyWorld;
	return true;
}

bool VoiceBridgeComponent::playerName(uint16_t player, std::string& out)
{
	IPlayer* target = core_ ? core_->getPlayers().get(player) : nullptr;
	if (!target)
	{
		return false;
	}
	const StringView name = target->getName();
	out.assign(name.data(), name.length());
	return !out.empty();
}

COMPONENT_ENTRY_POINT()
{
	return VoiceBridgeComponent::Get();
}
