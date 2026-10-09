/*
 *  Voice Bridge for open.mp and SA-MP
 */

#pragma once

#include "voice-server.hpp"
#include <amx/amx.h>
#include <vector>

namespace vbs
{
// Both loaders expose the classic AMX function table (SA-MP passes it to
// Load, open.mp through IPawnComponent::getAmxFunctions), so natives and
// callbacks share one implementation.
void SetAmxFunctionTable(void* table);

int RegisterNatives(AMX* amx);

class PawnHost final : public IScriptEvents
{
public:
	static PawnHost& Get();

	void addScript(AMX* amx);
	void removeScript(AMX* amx);
	void clear();
	const std::vector<AMX*>& scripts() const noexcept { return scripts_; }

	void onActivationKey(uint16_t player, uint8_t key, bool pressed) override;
	void onHandshake(uint16_t player, uint8_t version, bool extended, bool micro) override;
	void onTransport(uint16_t player, uint8_t transport) override;
	void onTalking(uint16_t player, bool talking) override;
	void onClientStatus(uint16_t player, bool micAvailable, bool micMuted, bool soundMuted) override;
	void onVoiceIgnored(uint16_t player, uint8_t reason) override;

private:
	// Calls `name` in every script that implements it.  Arguments are pushed
	// in reverse order as the AMX expects.
	void call(const char* name, std::initializer_list<cell> arguments);

	std::vector<AMX*> scripts_;
};
}
