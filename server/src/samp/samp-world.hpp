/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  Reads the game world on SA-MP by calling the server's own natives.  This
 *  works on every server build (0.3.7, 0.3.DL) without structure offsets.
 *  Native addresses are taken from the native tables of loaded scripts; the
 *  Voice Bridge includes reference every native used here.
 */

#pragma once

#include "../voice-server.hpp"
#include <amx/amx.h>
#include <vector>

namespace vbs::samp
{
class World final : public IWorld
{
public:
	void onAmxLoad(AMX* amx);
	void onAmxUnload(AMX* amx);
	// Natives seen while the server registers them (amx_Register hook), so
	// natives a script never calls are known too.
	void addNatives(const AMX_NATIVE_INFO* list, int count);

	bool playerPose(uint16_t player, Pose& out) override;
	bool vehiclePose(uint16_t vehicle, Pose& out) override;
	bool objectPose(uint16_t object, Pose& out) override;
	bool playerName(uint16_t player, std::string& out) override;

	bool canPollPlayers() const noexcept { return isPlayerConnected_ != nullptr && amx() != nullptr; }
	bool isPlayerConnected(uint16_t player);
	bool isPlayerNpc(uint16_t player);
	int maxPlayers();

private:
	AMX* amx() const noexcept { return scripts_.empty() ? nullptr : scripts_.front(); }
	void resolve(AMX* amx);
	void offer(const char* name, AMX_NATIVE native);
	cell call(AMX_NATIVE native, std::initializer_list<cell> arguments);
	bool position(AMX_NATIVE native, cell id, vb::Vec3& out);

	std::vector<AMX*> scripts_;
	AMX_NATIVE isPlayerConnected_ = nullptr;
	AMX_NATIVE isPlayerNpc_ = nullptr;
	AMX_NATIVE getMaxPlayers_ = nullptr;
	AMX_NATIVE getPlayerPos_ = nullptr;
	AMX_NATIVE getPlayerVirtualWorld_ = nullptr;
	AMX_NATIVE getPlayerInterior_ = nullptr;
	AMX_NATIVE getPlayerName_ = nullptr;
	AMX_NATIVE getVehiclePos_ = nullptr;
	AMX_NATIVE getVehicleVirtualWorld_ = nullptr;
	AMX_NATIVE getObjectPos_ = nullptr;
	AMX_NATIVE isValidObject_ = nullptr;
	bool warned_ = false;
};
}
