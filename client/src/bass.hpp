/*
 *  Voice Bridge client
 *
 *  BASS is loaded at runtime from the bass.dll that ships with SA-MP, so the
 *  mod stays a single file.  Only the functions the mod uses are declared.
 */

#pragma once

#include <windows.h>

namespace vbc::bass
{
using HSTREAM = DWORD;
using HRECORD = DWORD;
using HFX = DWORD;
using QWORD = unsigned long long;

struct DeviceInfo
{
	const char* name;
	const char* driver;
	DWORD flags;
};

using RecordProc = BOOL(CALLBACK*)(HRECORD handle, const void* buffer, DWORD length, void* user);
using DspProc = void(CALLBACK*)(DWORD handle, DWORD channel, void* buffer, DWORD length, void* user);

constexpr DWORD kDeviceEnabled = 1;
constexpr DWORD kDeviceDefault = 2;
constexpr DWORD kDeviceInit = 4;
constexpr DWORD kDeviceLoopback = 8;
constexpr DWORD kRecordPause = 0x8000;
constexpr DWORD kActiveStopped = 0;
constexpr DWORD kActivePlaying = 1;
constexpr DWORD kActiveStalled = 2;
constexpr DWORD kActivePaused = 3;
constexpr DWORD kAttribFreq = 1;
constexpr DWORD kAttribVol = 2;
constexpr DWORD kAttribPan = 3;
constexpr DWORD kAttribEaxMix = 4;
constexpr DWORD kAttribSrc = 8;
constexpr DWORD kDataAvailable = 0;
constexpr DWORD kPosByte = 0;
constexpr DWORD kConfigBuffer = 0; // BASS_CONFIG_BUFFER, ms
constexpr DWORD kConfigUpdatePeriod = 1; // BASS_CONFIG_UPDATEPERIOD, ms
constexpr DWORD kConfigGlobalStreamVolume = 5; // BASS_CONFIG_GVOL_STREAM, 0 - 10000
constexpr int kErrorInit = 8;
constexpr int kErrorAlready = 14;
inline void* const kStreamProcPush = reinterpret_cast<void*>(static_cast<intptr_t>(-1));

struct Api
{
	DWORD(WINAPI* GetVersion)() = nullptr;
	int(WINAPI* ErrorGetCode)() = nullptr;
	BOOL(WINAPI* Init)(int, DWORD, DWORD, HWND, const GUID*) = nullptr;
	DWORD(WINAPI* GetDevice)() = nullptr;
	BOOL(WINAPI* SetConfig)(DWORD, DWORD) = nullptr;
	DWORD(WINAPI* GetConfig)(DWORD) = nullptr;
	HSTREAM(WINAPI* StreamCreate)(DWORD, DWORD, DWORD, void*, void*) = nullptr;
	BOOL(WINAPI* StreamFree)(HSTREAM) = nullptr;
	DWORD(WINAPI* StreamPutData)(HSTREAM, const void*, DWORD) = nullptr;
	DWORD(WINAPI* ChannelIsActive)(DWORD) = nullptr;
	BOOL(WINAPI* ChannelPlay)(DWORD, BOOL) = nullptr;
	BOOL(WINAPI* ChannelStop)(DWORD) = nullptr;
	BOOL(WINAPI* ChannelPause)(DWORD) = nullptr;
	BOOL(WINAPI* ChannelSetAttribute)(DWORD, DWORD, float) = nullptr;
	BOOL(WINAPI* ChannelSetPosition)(DWORD, QWORD, DWORD) = nullptr;
	DWORD(WINAPI* ChannelGetData)(DWORD, void*, DWORD) = nullptr;
	HFX(WINAPI* ChannelSetFX)(DWORD, DWORD, int) = nullptr;
	DWORD(WINAPI* ChannelSetDSP)(DWORD, DspProc, void*, int) = nullptr; // optional
	BOOL(WINAPI* ChannelGetAttribute)(DWORD, DWORD, float*) = nullptr; // optional (diagnostics)
	BOOL(WINAPI* ChannelRemoveFX)(DWORD, HFX) = nullptr;
	BOOL(WINAPI* FXSetParameters)(HFX, const void*) = nullptr;
	BOOL(WINAPI* RecordGetDeviceInfo)(DWORD, DeviceInfo*) = nullptr;
	BOOL(WINAPI* RecordInit)(int) = nullptr;
	BOOL(WINAPI* RecordSetDevice)(DWORD) = nullptr;
	DWORD(WINAPI* RecordGetDevice)() = nullptr;
	BOOL(WINAPI* RecordFree)() = nullptr;
	HRECORD(WINAPI* RecordStart)(DWORD, DWORD, DWORD, RecordProc, void*) = nullptr;
};

// Loads bass.dll (already loaded by SA-MP in most cases).  Returns false if
// the library or a required export is missing.
bool Load();
bool Loaded();
const Api& Get();

// BASS output must be initialised before channels are created.  SA-MP does
// it at startup; when it did not (audio streams disabled) the mod does it.
bool EnsureOutput(HWND window);
}
