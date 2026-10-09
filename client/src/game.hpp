/*
 *  Voice Bridge client
 *
 *  GTA:SA 1.0 US addresses (the only executable SA-MP supports).
 */

#pragma once

namespace vbc::game
{
struct Vec3
{
	float x = 0.f;
	float y = 0.f;
	float z = 0.f;
};

struct Listener
{
	Vec3 position; // the player's character (the camera only when there is none)
	Vec3 front;
	Vec3 up;
	bool valid = false;
	// Diagnostics
	Vec3 camera;
	bool cameraValid = false;
	bool inVehicle = false;
};

// Orientation of the character or of the camera (what the player sees).
Listener GetListener(bool characterOrientation);
bool IsMenuActive();
}
