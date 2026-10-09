/*
 *  Voice Bridge client
 */

#include "settings.hpp"
#include "log.hpp"
#include <windows.h>
#include <algorithm>

namespace vbc
{
namespace
{
constexpr const char* kSection = "VoiceBridge";

std::string path()
{
	return GameDirectory() + "voicebridge.ini";
}

constexpr int kSettingsVersion = 2;

int readInt(const char* key, int fallback, int minimum, int maximum)
{
	const int value = static_cast<int>(GetPrivateProfileIntA(kSection, key, fallback, path().c_str()));
	return std::clamp(value, minimum, maximum);
}

bool readBool(const char* key, bool fallback)
{
	return GetPrivateProfileIntA(kSection, key, fallback ? 1 : 0, path().c_str()) != 0;
}

std::string readString(const char* key, const std::string& fallback)
{
	char buffer[256] {};
	GetPrivateProfileStringA(kSection, key, fallback.c_str(), buffer, sizeof(buffer), path().c_str());
	return buffer;
}

void write(const char* key, const std::string& value)
{
	WritePrivateProfileStringA(kSection, key, value.c_str(), path().c_str());
}

void write(const char* key, int value)
{
	write(key, std::to_string(value));
}
}

Settings& GetSettings()
{
	static Settings settings;
	return settings;
}

void LoadSettings()
{
	Settings& s = GetSettings();
	const Settings defaults;
	s.soundEnabled = readBool("SoundEnabled", defaults.soundEnabled);
	s.volume = readInt("Volume", defaults.volume, 0, 200);
	s.micEnabled = readBool("MicEnabled", defaults.micEnabled);
	s.micDevice = readString("MicDevice", defaults.micDevice);
	s.micGain = readInt("MicGain", defaults.micGain, 0, 400);
	s.noiseGate = readInt("NoiseGate", defaults.noiseGate, -90, -10);
	s.voiceActivation = readBool("VoiceActivation", defaults.voiceActivation);
	s.voiceActivationLevel = readInt("VoiceActivationLevel", defaults.voiceActivationLevel, -80, -5);
	s.showSpeakerList = readBool("ShowSpeakerList", defaults.showSpeakerList);
	s.showHeadIcons = readBool("ShowHeadIcons", defaults.showHeadIcons);
	s.showMicIcon = readBool("ShowMicIcon", defaults.showMicIcon);
	s.uiScale = readInt("UiScale", defaults.uiScale, 50, 300);
	s.menuKey = readInt("MenuKey", defaults.menuKey, 0, 255);
	s.language = readInt("Language", defaults.language, 0, 2);
	s.spatialMode = readInt("SpatialMode", defaults.spatialMode, 0, 2);
	s.roomAmount = readInt("RoomAmount", defaults.roomAmount, 0, 100);
	s.distanceStrength = readInt("DistanceStrength", defaults.distanceStrength, 50, 200);
	s.orientationFromCharacter = readBool("OrientationFromCharacter", defaults.orientationFromCharacter);
	s.swapChannels = readBool("SwapChannels", defaults.swapChannels);
	s.pushToTalkSound = readBool("PushToTalkSound", defaults.pushToTalkSound);
	if (readInt("SettingsVersion", 1, 0, 1000) < kSettingsVersion)
	{
		// Version 2 fixed mirrored left/right with the camera (players had
		// turned SwapChannels on to compensate) and changed these defaults.
		s.orientationFromCharacter = defaults.orientationFromCharacter;
		s.swapChannels = defaults.swapChannels;
		s.pushToTalkSound = defaults.pushToTalkSound;
	}
	s.micIconX = readInt("MicIconX", defaults.micIconX, 0, 1000);
	s.micIconY = readInt("MicIconY", defaults.micIconY, 0, 1000);
	if (s.menuKey < 0x08)
	{
		s.menuKey = defaults.menuKey; // mouse buttons and older "0 = disabled" files
	}
}

void SaveSettings()
{
	const Settings& s = GetSettings();
	write("SoundEnabled", s.soundEnabled ? 1 : 0);
	write("Volume", s.volume);
	write("MicEnabled", s.micEnabled ? 1 : 0);
	write("MicDevice", s.micDevice);
	write("MicGain", s.micGain);
	write("NoiseGate", s.noiseGate);
	write("VoiceActivation", s.voiceActivation ? 1 : 0);
	write("VoiceActivationLevel", s.voiceActivationLevel);
	write("ShowSpeakerList", s.showSpeakerList ? 1 : 0);
	write("ShowHeadIcons", s.showHeadIcons ? 1 : 0);
	write("ShowMicIcon", s.showMicIcon ? 1 : 0);
	write("UiScale", s.uiScale);
	WritePrivateProfileStringA(kSection, "MenuCommand", nullptr, path().c_str());
	write("MenuKey", s.menuKey);
	write("Language", s.language);
	write("SpatialMode", s.spatialMode);
	write("RoomAmount", s.roomAmount);
	write("DistanceStrength", s.distanceStrength);
	write("OrientationFromCharacter", s.orientationFromCharacter ? 1 : 0);
	write("SwapChannels", s.swapChannels ? 1 : 0);
	write("PushToTalkSound", s.pushToTalkSound ? 1 : 0);
	write("MicIconX", s.micIconX);
	write("MicIconY", s.micIconY);
	write("SettingsVersion", kSettingsVersion);
}
}
