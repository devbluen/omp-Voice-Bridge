/*
 *  Voice Bridge client
 */

#include "game.hpp"
#include "log.hpp"
#include "samp.hpp"
#include <cmath>

namespace vbc::game
{
namespace
{
constexpr uintptr_t kTheCamera = 0xB6F028;
constexpr uintptr_t kPlayerPed = 0xB6F5F0;
constexpr uintptr_t kMenuActive = 0xBA67A4;

bool readVec(uintptr_t address, Vec3& out)
{
	float values[3];
	if (!samp::SafeRead(reinterpret_cast<const void*>(address), values, sizeof(values)))
	{
		return false;
	}
	for (float value : values)
	{
		if (!std::isfinite(value))
		{
			return false;
		}
	}
	out = { values[0], values[1], values[2] };
	return true;
}

bool entityPosition(uintptr_t entity, Vec3& out)
{
	uintptr_t matrix = 0;
	if (!entity)
	{
		return false;
	}
	if (samp::Read(entity + 0x14, matrix) && matrix && readVec(matrix + 0x30, out))
	{
		return true;
	}
	return readVec(entity + 0x04, out);
}

float length(const Vec3& value)
{
	return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

float dot(const Vec3& a, const Vec3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Offset of the camera's view direction in its matrix (+0x10 like every
// GTA matrix; +0x20 is then the up axis).  Confirmed at runtime: in third
// person the camera looks at the character.
uintptr_t g_cameraForward = 0x10;
bool g_cameraChecked = false;
int g_votes[2] = { 0, 0 };

void checkCameraAxes(const Vec3& cameraPosition, const Vec3& axis10, const Vec3& axis20, const Vec3& pedPosition)
{
	const Vec3 toPed { pedPosition.x - cameraPosition.x, pedPosition.y - cameraPosition.y, pedPosition.z - cameraPosition.z };
	const float distance = length(toPed);
	if (distance < 2.f || distance > 25.f)
	{
		return; // first person or a cutscene camera
	}
	const float a = dot(axis10, toPed) / (distance * length(axis10));
	const float b = dot(axis20, toPed) / (distance * length(axis20));
	if (a > 0.6f && a > b + 0.4f)
	{
		++g_votes[0];
	}
	else if (b > 0.6f && b > a + 0.4f)
	{
		++g_votes[1];
	}
	if (g_votes[0] + g_votes[1] >= 120)
	{
		g_cameraForward = g_votes[1] > g_votes[0] * 3 ? 0x20 : 0x10;
		g_cameraChecked = true;
		Log("camera view axis: +0x%X", static_cast<unsigned>(g_cameraForward));
	}
}
}

Listener GetListener(bool characterOrientation)
{
	Listener listener;
	Vec3 cameraPosition;
	Vec3 cameraAxis10;
	Vec3 cameraAxis20;
	uintptr_t cameraMatrix = 0;
	// CMatrix: right (+0x00), forward (+0x10), up (+0x20), pos (+0x30).
	const bool camera = samp::Read(kTheCamera + 0x14, cameraMatrix) && cameraMatrix && readVec(cameraMatrix + 0x30, cameraPosition)
		&& readVec(cameraMatrix + 0x10, cameraAxis10) && readVec(cameraMatrix + 0x20, cameraAxis20) && length(cameraAxis10) > 0.5f
		&& length(cameraAxis20) > 0.5f;
	const Vec3 cameraFront = g_cameraForward == 0x10 ? cameraAxis10 : cameraAxis20;
	const Vec3 cameraUp = g_cameraForward == 0x10 ? cameraAxis20 : cameraAxis10;

	uintptr_t ped = 0;
	uintptr_t pedMatrix = 0;
	Vec3 pedPosition;
	Vec3 pedFront;
	Vec3 pedUp;
	if (samp::Read(kPlayerPed, ped))
	{
		const uintptr_t onFoot = ped;
		ped = samp::PedPlaceable(ped); // inside a vehicle, use the vehicle's matrix
		listener.inVehicle = ped != onFoot;
	}
	listener.camera = cameraPosition;
	listener.cameraValid = camera;
	const bool character = ped && entityPosition(ped, pedPosition);
	const bool characterAxes = character && samp::Read(ped + 0x14, pedMatrix) && pedMatrix && readVec(pedMatrix + 0x10, pedFront)
		&& readVec(pedMatrix + 0x20, pedUp) && length(pedFront) > 0.5f && length(pedUp) > 0.5f;

	if (camera && character && !g_cameraChecked)
	{
		checkCameraAxes(cameraPosition, cameraAxis10, cameraAxis20, pedPosition);
	}

	// Sounds are always placed relative to the player's character.
	if (character)
	{
		listener.position = pedPosition;
	}
	else if (camera)
	{
		listener.position = cameraPosition;
	}
	else
	{
		return listener;
	}

	if (characterOrientation && characterAxes)
	{
		listener.front = pedFront;
		listener.up = pedUp;
	}
	else if (camera)
	{
		listener.front = cameraFront;
		listener.up = cameraUp;
	}
	else if (characterAxes)
	{
		listener.front = pedFront;
		listener.up = pedUp;
	}
	else
	{
		return listener;
	}
	listener.valid = true;
	return listener;
}

bool IsMenuActive()
{
	uint8_t active = 0;
	return samp::Read(kMenuActive, active) && active != 0;
}
}
