/*
 *  Voice Bridge client
 *
 *  Everything that depends on the samp.dll build lives here.  Offsets come
 *  from SAMP-API (R1, R3, R5, DL); R2 and R4 reuse the layout of the closest
 *  build.  Every pointer read is validated, so an unexpected build degrades
 *  features instead of crashing the game.
 */

#pragma once

#include <string>

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace vbc::samp
{
enum class Version
{
	Unknown,
	R1,
	R2,
	R3,
	R4,
	R5,
	DL,
};

bool Detect(HMODULE module);
Version GetVersion();
const char* VersionName();
uintptr_t Base();

// Reads memory without faulting on bad pointers.
bool SafeRead(const void* address, void* out, std::size_t size);
// True while SafeRead is copying on this thread (its faults are expected).
bool InSafeRead();
template <typename T>
bool Read(uintptr_t address, T& out)
{
	return SafeRead(reinterpret_cast<const void*>(address), &out, sizeof(T));
}

void* NetGame();
// Address of the RakClient pointer inside CNetGame, or nullptr when it cannot
// be located safely.  `skip` is a vtable to ignore (our proxy).
void** RakClientSlot(const void* skipVtable);

bool IsChatInputActive();
bool IsDialogActive();
// Frees the mouse for the settings window (R1, R3, R5 and DL only).
bool SetCursor(bool visible);

// The entity that carries a GTA ped's position: its vehicle while it is inside
// one (the ped's own matrix is not updated there), otherwise the ped itself.
uintptr_t PedPlaceable(uintptr_t ped);

// Positions of remote entities, used when the server does not send them
// (SampVoice servers).
bool PlayerPosition(uint16_t player, float out[3]);
// Nick of a player from samp.dll's player pool (servers that do not send names).
bool PlayerName(uint16_t player, std::string& out);
bool VehiclePosition(uint16_t vehicle, float out[3]);
bool ObjectPosition(uint16_t object, float out[3]);
}
