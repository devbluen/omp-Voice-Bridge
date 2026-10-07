/*
 *  Voice Bridge for open.mp and SA-MP
 */

#pragma once

#include "../voice-server.hpp"
#include <Server/Components/Objects/objects.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Vehicles/vehicles.hpp>
#include <sdk.hpp>

class VoiceBridgeComponent final
	: public IComponent
	, public PawnEventHandler
	, public PlayerConnectEventHandler
	, public CoreEventHandler
	, public NetworkInEventHandler
	, public vbs::ITransport
	, public vbs::IWorld
{
public:
	PROVIDE_UID(0x8E3C5A17B2D94F60);

	static VoiceBridgeComponent* Get();

	StringView componentName() const override;
	SemanticVersion componentVersion() const override;
	void onLoad(ICore* core) override;
	void onInit(IComponentList* components) override;
	void onReady() override;
	void onFree(IComponent* component) override;
	void provideConfiguration(ILogger& logger, IEarlyConfig& config, bool defaults) override;
	void free() override;
	void reset() override;

	// PawnEventHandler
	void onAmxLoad(IPawnScript& script) override;
	void onAmxUnload(IPawnScript& script) override;

	// PlayerConnectEventHandler
	void onPlayerConnect(IPlayer& player) override;
	void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) override;

	// CoreEventHandler
	void onTick(Microseconds elapsed, TimePoint now) override;

	// NetworkInEventHandler
	bool onReceivePacket(IPlayer& peer, int id, NetworkBitStream& bs) override;
	bool onReceiveRPC(IPlayer& peer, int id, NetworkBitStream& bs) override;

	// vbs::ITransport
	bool sendPacket(uint16_t player, const uint8_t* data, std::size_t size, bool reliable) override;
	uint32_t playerIp(uint16_t player) override;
	bool orderedDelivery() const override { return false; }

	// vbs::IWorld
	bool playerPose(uint16_t player, vbs::Pose& out) override;
	bool vehiclePose(uint16_t vehicle, vbs::Pose& out) override;
	bool objectPose(uint16_t object, vbs::Pose& out) override;
	bool playerName(uint16_t player, std::string& out) override;

	~VoiceBridgeComponent();

private:
	void shutdown();

	ICore* core_ = nullptr;
	IPawnComponent* pawn_ = nullptr;
	IVehiclesComponent* vehicles_ = nullptr;
	IObjectsComponent* objects_ = nullptr;
	bool networkHooked_ = false;
	bool started_ = false;
	bool active_ = false;
};
