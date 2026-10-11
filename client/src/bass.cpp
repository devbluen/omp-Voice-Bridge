/*
 *  Voice Bridge client
 */

#include "bass.hpp"
#include "log.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>

namespace vbc::bass
{
namespace
{
Api g_api;
bool g_loaded = false;

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

SetConfigFn g_realSetConfig = nullptr;
StreamCreateUrlFn g_realStreamCreateUrl = nullptr;
SetAttributeFn g_realSetAttribute = nullptr;
SlideAttributeFn g_realSlideAttribute = nullptr;
std::atomic<DWORD> g_sampVolume { 10000 };
std::atomic<bool> g_redirected { false };

// SA-MP's audio streams and the volume each one asked for.
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

BOOL WINAPI sampSetConfig(DWORD option, DWORD value)
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

HSTREAM WINAPI sampStreamCreateUrl(const char* url, DWORD offset, DWORD flags, void* proc, void* user)
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

BOOL WINAPI sampSetAttribute(DWORD handle, DWORD attribute, float value)
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

BOOL WINAPI sampSlideAttribute(DWORD handle, DWORD attribute, float value, DWORD time)
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

// Replaces every stored pointer to `real` inside samp.dll's image (its
// import table, rebuilt by its packer at runtime) with `hook`.
int redirectPointers(uintptr_t begin, uintptr_t end, const void* real, const void* hook)
{
	int replaced = 0;
	uintptr_t address = begin;
	while (address < end)
	{
		MEMORY_BASIC_INFORMATION info {};
		if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)))
		{
			break;
		}
		const uintptr_t regionEnd = std::min<uintptr_t>(reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize, end);
		const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if (info.State == MEM_COMMIT && (info.Protect & readable) && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
		{
			for (uintptr_t slot = (address + 3) & ~uintptr_t(3); slot + sizeof(void*) <= regionEnd; slot += sizeof(void*))
			{
				if (*reinterpret_cast<const void* const*>(slot) != real)
				{
					continue;
				}
				DWORD previous = 0;
				if (VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_EXECUTE_READWRITE, &previous))
				{
					*reinterpret_cast<const void**>(slot) = hook;
					VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), previous, &previous);
					++replaced;
				}
			}
		}
		address = regionEnd;
	}
	return replaced;
}
}

bool RedirectSampVolume(HMODULE samp)
{
	if (g_redirected)
	{
		return true;
	}
	HMODULE module = GetModuleHandleA("bass.dll");
	if (!g_loaded || !samp || !module)
	{
		return false;
	}
	g_realSetConfig = reinterpret_cast<SetConfigFn>(GetProcAddress(module, "BASS_SetConfig"));
	g_realStreamCreateUrl = reinterpret_cast<StreamCreateUrlFn>(GetProcAddress(module, "BASS_StreamCreateURL"));
	g_realSetAttribute = reinterpret_cast<SetAttributeFn>(GetProcAddress(module, "BASS_ChannelSetAttribute"));
	g_realSlideAttribute = reinterpret_cast<SlideAttributeFn>(GetProcAddress(module, "BASS_ChannelSlideAttribute"));
	if (!g_realSetConfig || !g_realStreamCreateUrl || !g_realSetAttribute)
	{
		return false;
	}
	const auto base = reinterpret_cast<uintptr_t>(samp);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;

	// SetConfig first: until samp.dll has resolved it there is nothing to do.
	const int config = redirectPointers(base, end, reinterpret_cast<const void*>(g_realSetConfig), reinterpret_cast<const void*>(&sampSetConfig));
	if (!config)
	{
		return false;
	}
	const int create = redirectPointers(base, end, reinterpret_cast<const void*>(g_realStreamCreateUrl), reinterpret_cast<const void*>(&sampStreamCreateUrl));
	const int attribute = redirectPointers(base, end, reinterpret_cast<const void*>(g_realSetAttribute), reinterpret_cast<const void*>(&sampSetAttribute));
	const int slide = g_realSlideAttribute
		? redirectPointers(base, end, reinterpret_cast<const void*>(g_realSlideAttribute), reinterpret_cast<const void*>(&sampSlideAttribute))
		: 0;
	Log("SA-MP audio stream volume separated from voice chat (%d/%d/%d/%d)", config, create, attribute, slide);
	g_redirected = true;
	return true;
}

bool SampVolumeRedirected()
{
	return g_redirected;
}

void NoteSampStreamVolume(DWORD volume)
{
	if (!g_redirected)
	{
		g_sampVolume = volume > 10000 ? 10000 : volume;
	}
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
