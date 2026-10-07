/*
 *  Voice Bridge client
 */

#include "bass.hpp"
#include "log.hpp"

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
	const DWORD version = g_api.GetVersion();
	Log("bass.dll %u.%u.%u.%u loaded", HIBYTE(HIWORD(version)), LOBYTE(HIWORD(version)), HIBYTE(LOWORD(version)), LOBYTE(LOWORD(version)));
	g_loaded = true;
	return true;
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
