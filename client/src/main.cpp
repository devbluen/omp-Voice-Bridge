/*
 *  Voice Bridge client
 *
 *  Installation: copy voice-bridge.asi into the GTA San Andreas folder (an
 *  ASI loader is required; SA-MP players usually have one).  Works with
 *  SA-MP 0.3.7 R1, R2, R3, R4, R5 and 0.3.DL.
 */

#include "crash.hpp"
#include "log.hpp"
#include "overlay.hpp"
#include "samp.hpp"
#include "settings.hpp"
#include "version.hpp"
#include "voice.hpp"
#include <windows.h>

namespace
{
DWORD WINAPI initialise(LPVOID)
{
	using namespace vbc;
	LogOpen(GameDirectory() + "voicebridge.log");
	Log("Voice Bridge client %s (build " VOICE_BRIDGE_COMMIT ") - by " VOICE_BRIDGE_AUTHOR " - " VOICE_BRIDGE_URL, VOICE_BRIDGE_VERSION);
	crash::Install();

	// SA-MP injects samp.dll after the ASI plugins are loaded.
	HMODULE sampModule = nullptr;
	while (!(sampModule = GetModuleHandleA("samp.dll")))
	{
		Sleep(50);
	}

	for (const char* other : { "sampvoice.asi", "sampvoice.dll" })
	{
		if (GetModuleHandleA(other))
		{
			Log("%s is installed too: remove it, Voice Bridge replaces it", other);
			MessageBoxA(nullptr, "sampvoice.asi is installed in the GTA folder.\nRemove it: Voice Bridge already supports SampVoice servers.",
				"Voice Bridge", MB_OK | MB_ICONWARNING | MB_SYSTEMMODAL);
			return 0;
		}
	}

	samp::Detect(sampModule);
	LoadSettings();
	SaveSettings(); // writes defaults so players can find the file
	overlay::Install();
	return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(instance);
		// Never unloaded before the process ends: ASI loaders may free plugins
		// while the game shuts down, but our hooks (window procedure,
		// Direct3D, BASS) stay installed and would then call freed code.
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCSTR>(&DllMain), &self);
		if (HANDLE thread = CreateThread(nullptr, 0, initialise, nullptr, 0, nullptr))
		{
			CloseHandle(thread);
		}
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		vbc::TryLog(reserved ? "game closing" : "client unloaded");
	}
	return TRUE;
}
