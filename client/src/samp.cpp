/*
 *  Voice Bridge client
 */

#include "samp.hpp"
#include "log.hpp"
#include <cmath>
#include <cstring>
#include <string>

namespace vbc::samp
{
namespace
{
struct Layout
{
	const char* name;
	uint32_t entryPoint;
	uint32_t netGame;
	uint32_t rakClient[3]; // candidate offsets inside CNetGame, 0xFFFFFFFF = unused
	uint32_t input;
	uint32_t dialog;
	uint32_t game;
	uint32_t setCursorMode;
	uint32_t processInputEnabling;
	uint32_t pools;
	uint32_t poolsPlayer;
	uint32_t poolsVehicle;
	uint32_t poolsObject;
	uint32_t playerPoolObjects;
	uint32_t playerInfoPlayer;
	uint32_t remotePlayerPed;
	uint32_t vehiclePoolObjects;
	uint32_t objectPoolObjects;
	uint32_t playerInfoNick; // std::string inside CPlayerInfo
};

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kInputEnabled = 0x14E0;
constexpr uint32_t kDialogActive = 0x28;
constexpr uint32_t kEntityGameObject = 0x40;
constexpr uint16_t kMaxPlayers = 1004;
constexpr uint16_t kMaxVehicles = 2000;
constexpr uint16_t kMaxObjects = 1000;

// clang-format off
const Layout kLayouts[] = {
	{ "0.3.7-R1", 0x31DF13, 0x21A0F8, { 0x3C9, kNone, kNone }, 0x21A0E8, 0x21A0B8, 0x21A10C, 0x9BD30, 0x9BC10,
	  0x3CD, 0x18, 0x1C, 0x04, 0x2E, 0x00, 0x00, 0x1134, 0xFA4, 0x0C },
	{ "0.3.7-R2", 0x3195DD, 0x21A100, { 0x3C9, kNone, kNone }, 0x21A0F0, 0x21A0C0, kNone, kNone, kNone,
	  0x3CD, 0x18, 0x1C, 0x04, 0x2E, 0x00, 0x00, 0x1134, 0xFA4, 0x0C },
	{ "0.3.7-R3", 0x0CC4D0, 0x26E8DC, { 0x2C, kNone, kNone }, 0x26E8CC, 0x26E898, 0x26E8F4, 0x9FFE0, 0x9FEC0,
	  0x3DE, 0x08, 0x0C, 0x14, 0x04, 0x00, 0x00, 0x1134, 0xFA4, 0x0C },
	{ "0.3.7-R4", 0x0CBCB0, 0x26EA0C, { 0x2C, kNone, kNone }, 0x26E9FC, 0x26E9C8, kNone, kNone, kNone,
	  0x3DE, 0x08, 0x0C, 0x14, 0x04, 0x00, 0x00, 0x1134, 0xFA4, 0x0C },
	{ "0.3.7-R4-2", 0x0CBCD0, 0x26EA0C, { 0x2C, kNone, kNone }, 0x26E9FC, 0x26E9C8, kNone, kNone, kNone,
	  0x3DE, 0x08, 0x0C, 0x14, 0x04, 0x00, 0x00, 0x1134, 0xFA4, 0x0C },
	{ "0.3.7-R5", 0x0CBC90, 0x26EB94, { 0x2C, 0x00, kNone }, 0x26EB84, 0x26EB50, 0x26EBAC, 0xA06F0, 0xA05D0,
	  0x3DE, 0x04, 0x00, 0x0C, 0x1F8A, 0x10, 0x1DD, 0x1134, 0xFA4, 0x18 },
	{ "0.3.DL-R1", 0x0FDB60, 0x2ACA24, { 0x2C, kNone, kNone }, 0x2ACA14, 0x2AC9E0, 0x2ACA3C, 0xA0530, 0xA0410,
	  0x3DE, 0x08, 0x0C, 0x14, 0x26, 0x08, 0x04, 0x1134, 0x20D4, 0x14 },
};
// clang-format on

const Layout* g_layout = nullptr;
Version g_version = Version::Unknown;
uintptr_t g_base = 0;
uintptr_t g_imageEnd = 0;
uint32_t g_rakOffset = kNone;
bool g_rakScanned = false;

Version versionOf(std::size_t index)
{
	static const Version versions[] = { Version::R1, Version::R2, Version::R3, Version::R4, Version::R4, Version::R5, Version::DL };
	return index < sizeof(versions) / sizeof(versions[0]) ? versions[index] : Version::Unknown;
}

bool insideImage(uintptr_t address)
{
	return address >= g_base && address < g_imageEnd;
}

bool executable(uintptr_t address)
{
	MEMORY_BASIC_INFORMATION info {};
	if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT)
	{
		return false;
	}
	return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// A RakClient has a vtable of 60+ methods.  At the known offsets any
// executable vtable is accepted: SAMPFUNCS, moonloader and similar mods
// replace the RakClient with their own wrapper whose code lives in their
// module.  The blind scan only accepts vtables inside samp.dll.
bool looksLikeRakClient(uintptr_t object, const void* skipVtable, bool strict)
{
	uintptr_t vtable = 0;
	if (!object || !Read(object, vtable) || !vtable)
	{
		return false;
	}
	if (reinterpret_cast<const void*>(vtable) == skipVtable)
	{
		return true;
	}
	if (strict && !insideImage(vtable))
	{
		return false;
	}
	for (int i = 0; i < 48; ++i)
	{
		uintptr_t function = 0;
		if (!Read(vtable + i * sizeof(uintptr_t), function) || (strict ? !insideImage(function) : !executable(function)))
		{
			return false;
		}
	}
	return true;
}

std::string moduleOf(uintptr_t address)
{
	HMODULE module = nullptr;
	char name[MAX_PATH] {};
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(address), &module) && GetModuleFileNameA(module, name, MAX_PATH))
	{
		const char* slash = std::strrchr(name, '\\');
		return slash ? slash + 1 : name;
	}
	return "?";
}

bool finite3(const float value[3])
{
	for (int i = 0; i < 3; ++i)
	{
		if (!std::isfinite(value[i]) || std::fabs(value[i]) > 50000.f)
		{
			return false;
		}
	}
	return true;
}

// GTA CPlaceable: matrix pointer at +0x14 (position at +0x30), placement at +0x04.
bool gamePosition(uintptr_t entity, float out[3])
{
	if (!entity)
	{
		return false;
	}
	uintptr_t matrix = 0;
	if (Read(entity + 0x14, matrix) && matrix)
	{
		if (SafeRead(reinterpret_cast<const void*>(matrix + 0x30), out, sizeof(float) * 3) && finite3(out))
		{
			return true;
		}
	}
	return SafeRead(reinterpret_cast<const void*>(entity + 0x04), out, sizeof(float) * 3) && finite3(out);
}

uintptr_t pool(uint32_t member)
{
	void* netGame = NetGame();
	uintptr_t pools = 0;
	uintptr_t result = 0;
	if (!netGame || !g_layout || g_layout->pools == kNone || !Read(reinterpret_cast<uintptr_t>(netGame) + g_layout->pools, pools) || !pools)
	{
		return 0;
	}
	return Read(pools + member, result) ? result : 0;
}
}

bool SafeRead(const void* address, void* out, std::size_t size)
{
	if (!address)
	{
		return false;
	}
	__try
	{
		std::memcpy(out, address, size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

bool Detect(HMODULE module)
{
	g_base = reinterpret_cast<uintptr_t>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
	g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
	const uint32_t entry = nt->OptionalHeader.AddressOfEntryPoint;
	for (std::size_t i = 0; i < sizeof(kLayouts) / sizeof(kLayouts[0]); ++i)
	{
		if (kLayouts[i].entryPoint == entry)
		{
			g_layout = &kLayouts[i];
			g_version = versionOf(i);
			Log("samp.dll %s detected", g_layout->name);
			return true;
		}
	}
	Log("unknown samp.dll build (entry point 0x%X); voice works but some features are disabled", entry);
	return false;
}

Version GetVersion()
{
	return g_version;
}

const char* VersionName()
{
	return g_layout ? g_layout->name : "unknown";
}

uintptr_t Base()
{
	return g_base;
}

void* NetGame()
{
	if (!g_layout)
	{
		return nullptr;
	}
	uintptr_t netGame = 0;
	return Read(g_base + g_layout->netGame, netGame) ? reinterpret_cast<void*>(netGame) : nullptr;
}

void** RakClientSlot(const void* skipVtable)
{
	const auto netGame = reinterpret_cast<uintptr_t>(NetGame());
	if (!netGame)
	{
		return nullptr;
	}
	const auto check = [&](uint32_t offset, bool strict) -> void**
	{
		uintptr_t object = 0;
		if (offset != kNone && Read(netGame + offset, object) && looksLikeRakClient(object, skipVtable, strict))
		{
			return reinterpret_cast<void**>(netGame + offset);
		}
		return nullptr;
	};

	if (g_rakOffset != kNone)
	{
		return check(g_rakOffset, false);
	}
	for (uint32_t offset : g_layout->rakClient)
	{
		if (void** slot = check(offset, false))
		{
			g_rakOffset = offset;
			uintptr_t object = 0;
			uintptr_t vtable = 0;
			Read(netGame + offset, object);
			Read(object, vtable);
			Log("RakClient found at CNetGame+0x%X (implemented by %s)", offset, moduleOf(vtable).c_str());
			return slot;
		}
	}
	static bool diagnosed = false;
	if (!diagnosed)
	{
		diagnosed = true;
		for (uint32_t offset : g_layout->rakClient)
		{
			uintptr_t object = 0;
			uintptr_t vtable = 0;
			if (offset != kNone)
			{
				Read(netGame + offset, object);
				Read(object, vtable);
				Log("RakClient candidate CNetGame+0x%X: object 0x%X, vtable 0x%X (%s)", offset, static_cast<unsigned>(object),
					static_cast<unsigned>(vtable), vtable ? moduleOf(vtable).c_str() : "-");
			}
		}
	}
	if (!g_rakScanned)
	{
		// Unknown layout: look for the only object with a large samp.dll vtable.
		g_rakScanned = true;
		for (uint32_t offset = 0; offset < 0x400; ++offset)
		{
			if (void** slot = check(offset, true))
			{
				g_rakOffset = offset;
				Log("RakClient found by scan at CNetGame+0x%X", offset);
				return slot;
			}
		}
		g_rakScanned = false; // CNetGame may not be fully built yet
	}
	return nullptr;
}

bool IsChatInputActive()
{
	uintptr_t input = 0;
	BOOL enabled = FALSE;
	return g_layout && Read(g_base + g_layout->input, input) && input && Read(input + kInputEnabled, enabled) && enabled;
}

bool IsDialogActive()
{
	uintptr_t dialog = 0;
	BOOL active = FALSE;
	return g_layout && Read(g_base + g_layout->dialog, dialog) && dialog && Read(dialog + kDialogActive, active) && active;
}

bool SetCursor(bool visible)
{
	if (!g_layout || g_layout->game == kNone || g_layout->setCursorMode == kNone)
	{
		return false;
	}
	uintptr_t game = 0;
	if (!Read(g_base + g_layout->game, game) || !game)
	{
		return false;
	}
	using SetCursorModeFn = void(__thiscall*)(void*, int, BOOL);
	using ProcessInputFn = void(__thiscall*)(void*);
	reinterpret_cast<SetCursorModeFn>(g_base + g_layout->setCursorMode)(reinterpret_cast<void*>(game), visible ? 2 : 0, visible ? FALSE : TRUE);
	if (!visible && g_layout->processInputEnabling != kNone)
	{
		reinterpret_cast<ProcessInputFn>(g_base + g_layout->processInputEnabling)(reinterpret_cast<void*>(game));
	}
	return true;
}

bool PlayerPosition(uint16_t player, float out[3])
{
	if (player >= kMaxPlayers || !g_layout)
	{
		return false;
	}
	const uintptr_t players = pool(g_layout->poolsPlayer);
	uintptr_t info = 0;
	uintptr_t remote = 0;
	uintptr_t ped = 0;
	uintptr_t gamePed = 0;
	return players && Read(players + g_layout->playerPoolObjects + player * sizeof(uint32_t), info) && info
		&& Read(info + g_layout->playerInfoPlayer, remote) && remote && Read(remote + g_layout->remotePlayerPed, ped) && ped
		&& Read(ped + kEntityGameObject, gamePed) && gamePosition(gamePed, out);
}

bool PlayerName(uint16_t player, std::string& out)
{
	if (player >= kMaxPlayers || !g_layout)
	{
		return false;
	}
	const uintptr_t players = pool(g_layout->poolsPlayer);
	uintptr_t info = 0;
	if (!players || !Read(players + g_layout->playerPoolObjects + player * sizeof(uint32_t), info) || !info)
	{
		return false;
	}
	// MSVC std::string: 16 byte buffer (or pointer), size, capacity.
	const uintptr_t nick = info + g_layout->playerInfoNick;
	uint32_t size = 0;
	uint32_t capacity = 0;
	if (!Read(nick + 16, size) || !Read(nick + 20, capacity) || size == 0 || size > 24 || capacity < size)
	{
		return false;
	}
	uintptr_t text = nick;
	if (capacity >= 16 && !Read(nick, text))
	{
		return false;
	}
	char buffer[25] {};
	if (!SafeRead(reinterpret_cast<const void*>(text), buffer, size))
	{
		return false;
	}
	for (uint32_t i = 0; i < size; ++i)
	{
		if (static_cast<unsigned char>(buffer[i]) < 0x20)
		{
			return false;
		}
	}
	out.assign(buffer, size);
	return true;
}

bool VehiclePosition(uint16_t vehicle, float out[3])
{
	if (vehicle >= kMaxVehicles || !g_layout)
	{
		return false;
	}
	const uintptr_t vehicles = pool(g_layout->poolsVehicle);
	uintptr_t object = 0;
	uintptr_t game = 0;
	return vehicles && Read(vehicles + g_layout->vehiclePoolObjects + vehicle * sizeof(uint32_t), object) && object
		&& Read(object + kEntityGameObject, game) && gamePosition(game, out);
}

bool ObjectPosition(uint16_t objectId, float out[3])
{
	if (objectId >= kMaxObjects || !g_layout)
	{
		return false;
	}
	const uintptr_t objects = pool(g_layout->poolsObject);
	uintptr_t object = 0;
	uintptr_t game = 0;
	return objects && Read(objects + g_layout->objectPoolObjects + objectId * sizeof(uint32_t), object) && object
		&& Read(object + kEntityGameObject, game) && gamePosition(game, out);
}
}
