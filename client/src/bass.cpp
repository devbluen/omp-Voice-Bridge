/*
 *  Voice Bridge client
 */

#include "bass.hpp"
#include "log.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace vbc::bass
{
namespace
{
Api g_api;
bool g_loaded = false;
void separateSampVolume(HMODULE module);

template <typename T>
bool resolve(HMODULE module, T& target, const char* name)
{
	target = reinterpret_cast<T>(GetProcAddress(module, name));
	if (!target)
	{
		Log("bass.dll does not export %s", name);
		return false;
	}
	return true;
}
}

bool Load()
{
	if (g_loaded)
	{
		return true;
	}
	// Our own reference, pinned: SA-MP may free bass.dll on exit while our
	// channels, DSP and UDP thread still use it.
	HMODULE module = LoadLibraryA("bass.dll");
	if (module)
	{
		HMODULE pinned = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, "bass.dll", &pinned);
	}
	if (!module)
	{
		Log("bass.dll was not found next to gta_sa.exe (it is installed with SA-MP)");
		return false;
	}

	bool ok = true;
	ok &= resolve(module, g_api.GetVersion, "BASS_GetVersion");
	ok &= resolve(module, g_api.ErrorGetCode, "BASS_ErrorGetCode");
	ok &= resolve(module, g_api.Init, "BASS_Init");
	ok &= resolve(module, g_api.GetDevice, "BASS_GetDevice");
	ok &= resolve(module, g_api.SetConfig, "BASS_SetConfig");
	ok &= resolve(module, g_api.GetConfig, "BASS_GetConfig");
	ok &= resolve(module, g_api.StreamCreate, "BASS_StreamCreate");
	ok &= resolve(module, g_api.StreamFree, "BASS_StreamFree");
	ok &= resolve(module, g_api.StreamPutData, "BASS_StreamPutData");
	ok &= resolve(module, g_api.ChannelIsActive, "BASS_ChannelIsActive");
	ok &= resolve(module, g_api.ChannelPlay, "BASS_ChannelPlay");
	ok &= resolve(module, g_api.ChannelStop, "BASS_ChannelStop");
	ok &= resolve(module, g_api.ChannelPause, "BASS_ChannelPause");
	ok &= resolve(module, g_api.ChannelSetAttribute, "BASS_ChannelSetAttribute");
	ok &= resolve(module, g_api.ChannelSetPosition, "BASS_ChannelSetPosition");
	ok &= resolve(module, g_api.ChannelGetData, "BASS_ChannelGetData");
	ok &= resolve(module, g_api.ChannelSetFX, "BASS_ChannelSetFX");
	ok &= resolve(module, g_api.ChannelRemoveFX, "BASS_ChannelRemoveFX");
	ok &= resolve(module, g_api.FXSetParameters, "BASS_FXSetParameters");
	ok &= resolve(module, g_api.RecordGetDeviceInfo, "BASS_RecordGetDeviceInfo");
	ok &= resolve(module, g_api.RecordInit, "BASS_RecordInit");
	ok &= resolve(module, g_api.RecordSetDevice, "BASS_RecordSetDevice");
	ok &= resolve(module, g_api.RecordGetDevice, "BASS_RecordGetDevice");
	ok &= resolve(module, g_api.RecordFree, "BASS_RecordFree");
	ok &= resolve(module, g_api.RecordStart, "BASS_RecordStart");
	if (!ok)
	{
		return false;
	}
	g_api.ChannelSetDSP = reinterpret_cast<decltype(g_api.ChannelSetDSP)>(GetProcAddress(module, "BASS_ChannelSetDSP"));
	g_api.ChannelGetAttribute = reinterpret_cast<decltype(g_api.ChannelGetAttribute)>(GetProcAddress(module, "BASS_ChannelGetAttribute"));
	const DWORD version = g_api.GetVersion();
	if (version == 0x02040700)
	{
		separateSampVolume(module);
	}
	Log("bass.dll %u.%u.%u.%u loaded", HIBYTE(HIWORD(version)), LOBYTE(HIWORD(version)), HIBYTE(LOWORD(version)), LOBYTE(LOWORD(version)));
	g_loaded = true;
	return true;
}

namespace
{
using SetConfigFn = BOOL(WINAPI*)(DWORD, DWORD);
using StreamCreateUrlFn = HSTREAM(WINAPI*)(const char*, DWORD, DWORD, void*, void*);
using SetAttributeFn = BOOL(WINAPI*)(DWORD, DWORD, float);
using SlideAttributeFn = BOOL(WINAPI*)(DWORD, DWORD, float, DWORD);

// Originals (trampolines once hooked).
SetConfigFn g_realSetConfig = nullptr;
StreamCreateUrlFn g_realStreamCreateUrl = nullptr;
SetAttributeFn g_realSetAttribute = nullptr;
SlideAttributeFn g_realSlideAttribute = nullptr;
std::atomic<DWORD> g_sampVolume { 10000 };
bool g_separated = false;

// Audio streams created by others (SA-MP's PlayAudioStreamForPlayer) and the
// volume each one asked for.
struct SampStream
{
	HSTREAM handle = 0;
	float volume = 1.f;
};
std::mutex g_streamsMutex;
SampStream g_streams[8];
int g_nextStream = 0;

float radioFactor()
{
	return static_cast<float>(g_sampVolume.load()) / 10000.f;
}

SampStream* findStream(DWORD handle)
{
	for (SampStream& stream : g_streams)
	{
		if (stream.handle && stream.handle == handle)
		{
			return &stream;
		}
	}
	return nullptr;
}

// SA-MP sets the global stream volume to the GTA radio volume: keep it for
// its audio streams only.  The mod itself calls the trampolines.
BOOL WINAPI hookSetConfig(DWORD option, DWORD value)
{
	if (option != kConfigGlobalStreamVolume)
	{
		return g_realSetConfig(option, value);
	}
	g_sampVolume = value > 10000 ? 10000 : value;
	std::lock_guard<std::mutex> lock(g_streamsMutex);
	for (const SampStream& stream : g_streams)
	{
		if (stream.handle)
		{
			g_realSetAttribute(stream.handle, kAttribVol, stream.volume * radioFactor());
		}
	}
	return TRUE;
}

HSTREAM WINAPI hookStreamCreateUrl(const char* url, DWORD offset, DWORD flags, void* proc, void* user)
{
	const HSTREAM handle = g_realStreamCreateUrl(url, offset, flags, proc, user);
	if (handle)
	{
		std::lock_guard<std::mutex> lock(g_streamsMutex);
		g_streams[g_nextStream] = { handle, 1.f };
		g_nextStream = (g_nextStream + 1) % static_cast<int>(sizeof(g_streams) / sizeof(g_streams[0]));
		g_realSetAttribute(handle, kAttribVol, radioFactor());
	}
	return handle;
}

BOOL WINAPI hookSetAttribute(DWORD handle, DWORD attribute, float value)
{
	if (attribute == kAttribVol)
	{
		std::lock_guard<std::mutex> lock(g_streamsMutex);
		if (SampStream* stream = findStream(handle))
		{
			stream->volume = value;
			value *= radioFactor();
		}
	}
	return g_realSetAttribute(handle, attribute, value);
}

BOOL WINAPI hookSlideAttribute(DWORD handle, DWORD attribute, float value, DWORD time)
{
	if (attribute == kAttribVol && value >= 0.f)
	{
		std::lock_guard<std::mutex> lock(g_streamsMutex);
		if (SampStream* stream = findStream(handle))
		{
			stream->volume = value;
			value *= radioFactor();
		}
	}
	return g_realSlideAttribute(handle, attribute, value, time);
}

bool startsWith(const void* code, const uint8_t* expected, std::size_t size)
{
	return code && std::memcmp(code, expected, size) == 0;
}

// Detours `target` to `hook`.  Its first `size` bytes must be whole, position
// independent instructions (at least 5 bytes): they are copied to a
// trampoline that jumps back after them.  Returns the trampoline.
void* detour(void* target, std::size_t size, void* hook)
{
	auto* code = static_cast<uint8_t*>(target);
	auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!trampoline)
	{
		return nullptr;
	}
	std::memcpy(trampoline, code, size);
	trampoline[size] = 0xE9;
	const auto back = static_cast<int32_t>(reinterpret_cast<intptr_t>(code + size) - reinterpret_cast<intptr_t>(trampoline + size + 5));
	std::memcpy(trampoline + size + 1, &back, sizeof(back));

	DWORD previous = 0;
	if (!VirtualProtect(code, size, PAGE_EXECUTE_READWRITE, &previous))
	{
		VirtualFree(trampoline, 0, MEM_RELEASE);
		return nullptr;
	}
	uint8_t patch[8];
	std::memset(patch, 0x90, sizeof(patch));
	patch[0] = 0xE9;
	const auto jump = static_cast<int32_t>(reinterpret_cast<intptr_t>(hook) - reinterpret_cast<intptr_t>(code + 5));
	std::memcpy(patch + 1, &jump, sizeof(jump));
	std::memcpy(code, patch, size);
	VirtualProtect(code, size, previous, &previous);
	FlushInstructionCache(GetCurrentProcess(), code, size);
	return trampoline;
}

// bass.dll 2.4.7 (the one installed with SA-MP).
const uint8_t kSetConfigStart[] = { 0x8B, 0x4C, 0x24, 0x04, 0x8D, 0x44, 0x24, 0x08 }; // mov ecx,[esp+4]; lea eax,[esp+8]
const uint8_t kStreamCreateUrlStart[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 }; // push ebp; mov ebp,esp; push ebx; push esi; push edi
const uint8_t kSetAttributeStart[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08 }; // push ebp; mov ebp,esp; mov ecx,[ebp+8]
const uint8_t kSlideAttributeStart[] = { 0x55, 0x8B, 0xEC, 0x51, 0x8D, 0x45, 0xFC }; // push ebp; mov ebp,esp; push ecx; lea eax,[ebp-4]

// SA-MP follows the GTA radio volume with BASS's global stream volume, which
// also scales the voices; the mod used to force it back every frame and that
// tug of war made audio streams (PlayAudioStreamForPlayer) choppy.  Called
// from Load, before any voice channel exists.
void separateSampVolume(HMODULE module)
{
	void* setConfig = reinterpret_cast<void*>(GetProcAddress(module, "BASS_SetConfig"));
	void* createUrl = reinterpret_cast<void*>(GetProcAddress(module, "BASS_StreamCreateURL"));
	void* setAttribute = reinterpret_cast<void*>(GetProcAddress(module, "BASS_ChannelSetAttribute"));
	void* slideAttribute = reinterpret_cast<void*>(GetProcAddress(module, "BASS_ChannelSlideAttribute"));
	if (!startsWith(setConfig, kSetConfigStart, sizeof(kSetConfigStart)) || !startsWith(createUrl, kStreamCreateUrlStart, sizeof(kStreamCreateUrlStart))
		|| !startsWith(setAttribute, kSetAttributeStart, sizeof(kSetAttributeStart)))
	{
		Log("SA-MP audio stream volume not separated from voice chat (unknown bass.dll code)");
		return;
	}

	// The current value is SA-MP's radio volume (or the default 10000).
	const DWORD current = g_api.GetConfig(kConfigGlobalStreamVolume);
	g_sampVolume = current <= 10000 ? current : 10000;

	// The attribute hook first: the others call its trampoline.
	g_realSetAttribute = reinterpret_cast<SetAttributeFn>(detour(setAttribute, sizeof(kSetAttributeStart), reinterpret_cast<void*>(&hookSetAttribute)));
	if (!g_realSetAttribute)
	{
		return;
	}
	g_api.ChannelSetAttribute = g_realSetAttribute;
	g_realStreamCreateUrl =
		reinterpret_cast<StreamCreateUrlFn>(detour(createUrl, sizeof(kStreamCreateUrlStart), reinterpret_cast<void*>(&hookStreamCreateUrl)));
	if (startsWith(slideAttribute, kSlideAttributeStart, sizeof(kSlideAttributeStart)))
	{
		g_realSlideAttribute =
			reinterpret_cast<SlideAttributeFn>(detour(slideAttribute, sizeof(kSlideAttributeStart), reinterpret_cast<void*>(&hookSlideAttribute)));
	}
	g_realSetConfig = reinterpret_cast<SetConfigFn>(detour(setConfig, sizeof(kSetConfigStart), reinterpret_cast<void*>(&hookSetConfig)));
	if (!g_realSetConfig)
	{
		return;
	}
	g_api.SetConfig = g_realSetConfig;
	g_realSetConfig(kConfigGlobalStreamVolume, 10000);
	g_separated = true;
	Log("SA-MP audio stream volume separated from voice chat (radio volume %u/10000)", static_cast<unsigned>(g_sampVolume.load()));
}
}

bool SampVolumeSeparated()
{
	return g_separated;
}

bool Loaded()
{
	return g_loaded;
}

const Api& Get()
{
	return g_api;
}

bool EnsureOutput(HWND window)
{
	if (!g_loaded)
	{
		return false;
	}
	if (g_api.GetDevice() != static_cast<DWORD>(-1))
	{
		return true;
	}
	if (g_api.Init(-1, 48000, 0, window, nullptr) || g_api.ErrorGetCode() == kErrorAlready)
	{
		Log("BASS output initialised by Voice Bridge");
		return true;
	}
	Log("BASS output could not be initialised (error %d)", g_api.ErrorGetCode());
	return false;
}
}
