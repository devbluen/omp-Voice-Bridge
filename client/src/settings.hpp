/*
 *  Voice Bridge client
 */

#pragma once

#include <string>

namespace vbc
{
struct Settings
{
	bool soundEnabled = true;
	int volume = 100; // 0 - 200 %
	bool micEnabled = true;
	std::string micDevice; // empty = system default
	int micGain = 100; // 0 - 400 %
	int noiseGate = -55; // dBFS, -90 disables
	bool voiceActivation = false; // only used when the server allows it
	int voiceActivationLevel = -38; // dBFS
	bool showSpeakerList = true;
	bool showMicIcon = true;
	int uiScale = 100; // %
	int menuKey = 0x7A; // VK_F11: opens the voice menu

	// 3D audio
	int spatialMode = 0; // 0 realistic, 1 simple stereo, 2 off
	int roomAmount = 40; // 0 - 100: room echo that makes distance audible
	int distanceStrength = 100; // 50 - 200 %: how fast voices fade with distance
	bool orientationFromCharacter = false; // directions follow the character instead of the camera
	bool swapChannels = false;
	bool pushToTalkSound = false;

	// Microphone icon position, in thousandths of the screen
	int micIconX = 45;
	int micIconY = 620;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();
}
